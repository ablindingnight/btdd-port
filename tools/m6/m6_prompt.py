"""m6_prompt.py: the prompt for decompiling one routine to C (M6). Private pipeline: the prompt holds the routine's
asm and goes only to the local model; nothing here is committed or shipped.

Message layout (fixed part first, so llama-server can reuse its cached prefix across routines):
  system: the fixed instructions + the full candidate API (m6_api.h)
  user:   routine facts -> RAM names it uses -> ROM tables it reads -> asm listing (decomp, `git show HEAD:`)
          -> the M5 mechanical C of the same routine (exact but unreadable; a reference for semantics)
          -> what the test compares (outputs)

  python tools/m6/m6_prompt.py BANK:ADDR          print the two messages
"""
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, os.path.join(REPO, 'tools'))
sys.path.insert(0, os.path.join(REPO, 'recomp'))
sys.path.insert(0, HERE)
from m6502 import decode  # noqa: E402
import m6_pick  # noqa: E402
import m6_test  # noqa: E402

DECOMP = os.path.join(REPO, '..', 'btdd-decomp')

SYSTEM = """You translate one 6502 routine of an NES game into a readable, exactly equivalent C function.

Rules:
- Write exactly one function `void routine(M6 *m)` in one ```c fenced block. No main, no other functions
  except small static helpers if they make it clearer. Do not include headers: the API below is already included.
- It must do exactly what the 6502 code does, for every possible entry state: same RAM writes, same values,
  and the same hardware accesses (bus_read/bus_write) in the same order. Branch conditions must follow the 6502
  flags exactly (carry in ADC/SBC/ROL/ROR, 8-bit wraparound, signed/unsigned compares). Use uint8_t/unsigned
  arithmetic and mask to 8 bits where the 6502 would wrap.
- RAM: use RAM(addr). Use the given RAM names (they are #defined addresses), e.g. RAM(ent_hp + m->x).
  Zero page and $0100-$07FF are plain RAM. Any address >= $2000 (hardware registers, ROM tables, the bank
  register at $8000+) must go through bus_read/bus_write.
- The stack: PHA/PLA/PHP/PLP pairs inside the routine become local variables; do not emulate S.
- Registers and flags live in m->a, m->x, m->y, m->c, m->z, m->n, m->v (0/1). Set at the end every output the
  task lists; you may also keep them updated as you go.
- Prefer clear C: loops instead of gotos where the structure allows, meaningful local names, short comments
  that say what the code does in game terms when the names make it clear. Correctness comes first.

The API (already included):
```c
typedef struct M6 { uint8_t a, x, y; uint8_t c, z, n, v; uint8_t *ram; void *ctx; } M6;
uint8_t bus_read(M6 *m, uint16_t addr);           /* hardware registers, ROM of the mapped bank */
void bus_write(M6 *m, uint16_t addr, uint8_t value);
#define RAM(addr) (m->ram[(addr) & 0x7FF])
```
Answer with the ```c block only (thinking first is fine)."""


def asm_listing(bank, name):
    """The routine's block from the decomp source (header comment through the line before the next one)."""
    try:
        src = subprocess.run(['git', '-C', DECOMP, 'show', 'HEAD:src/bank%d.s' % bank], capture_output=True,
                             text=True, check=True).stdout
    except (subprocess.CalledProcessError, FileNotFoundError):
        return None
    lines = src.splitlines()
    for i, l in enumerate(lines):
        if re.match(r'^%s:' % re.escape(name), l):
            j = i
            while j > 0 and lines[j - 1].startswith(';'):
                j -= 1
            k = i + 1
            while k < len(lines) and not lines[k].startswith('; $') and not lines[k].startswith('.incbin') \
                    and not re.match(r'^[A-Za-z_]\w*:', lines[k]):
                k += 1
            return '\n'.join(lines[j:k]).rstrip()
    return None


def mechanical_c(bank, start, end, mem):
    import gen  # recomp/gen.py
    g = gen.Gen(bank, mem)
    return g.routine('', start, end)[2]


def outputs_of(ins):
    """Registers the routine changes (callers may use them) + C when it ends in CLC/SEC; RTS."""
    regs = set()
    for a, (m, mode, n, v) in ins:
        if m in ('lda', 'pla', 'txa', 'tya', 'adc', 'sbc', 'and', 'ora', 'eor') or \
                (m in ('asl', 'lsr', 'rol', 'ror') and mode == 'acc'):
            regs.add('a')
        if m in ('ldx', 'tax', 'tsx', 'inx', 'dex'):
            regs.add('x')
        if m in ('ldy', 'tay', 'iny', 'dey'):
            regs.add('y')
    for i in range(1, len(ins)):
        if ins[i][1][0] == 'rts' and ins[i - 1][1][0] in ('clc', 'sec'):
            regs.add('c')
    return ''.join(r for r in 'axyc' if r in regs)


def build(bank, addr):
    raw = open(m6_test.ROM, 'rb').read()[16:]
    mem = raw[bank * 0x8000:(bank + 1) * 0x8000]
    r = next(x for x in m6_pick.routines() if x['bank'] == bank and x['start'] == addr)
    leaf, flags, ins = m6_pick.analyse(mem, r)
    names = {a: n for n, a, size, kind, naming in m6_test.ram_names() if size >= 1}
    spans = [(a, a + size - 1, n, kind) for n, a, size, kind, naming in m6_test.ram_names()]
    used, tables = [], []
    for a, (m, mode, n, v) in ins:
        if v is None or mode in ('imm', 'rel'):
            continue
        if v < 0x2000:
            for lo, hi, nm, kind in spans:
                if lo <= v <= hi and (nm, lo) not in [(u[0], u[1]) for u in used]:
                    used.append((nm, lo, kind))
        elif v >= 0x8000 and m not in ('jmp', 'jsr'):
            tables.append(v)
    outs = outputs_of(ins)
    calls = m6_pick.call_counts().get((bank, addr), 0)
    asm = asm_listing(bank, r['name']) or '(decomp listing not found)'
    mech = mechanical_c(bank, r['start'], r['end'], mem)
    parts = ['Routine %s: bank %d, $%04X-$%04X, %d instructions, called %d times in the 2-player TAS.'
             % (r['name'], bank, r['start'], r['end'], len(ins), calls)]
    if r.get('proposed'):
        parts.append('Proposed name (unreviewed): %s' % r['proposed'])
    if used:
        parts.append('RAM names it uses (#defined addresses): ' +
                     ', '.join('%s=$%04X (%s)' % (n, a, k) for n, a, k in used))
    if tables:
        parts.append('It reads ROM at: ' + ', '.join('$%04X' % t for t in sorted(set(tables))) +
                     ' (use bus_read for these)')
    parts.append('6502 source (ca65):\n```\n%s\n```' % asm)
    parts.append('Mechanical C of the same code from our recompiler (exact cycle-level semantics, ugly; n_cyc and '
                 'the bus/pc lines are bookkeeping you must NOT copy; it only shows the meaning of each '
                 'instruction):\n```c\n%s\n```' % mech)
    parts.append('Outputs the test compares besides RAM and hardware accesses: %s.' %
                 (', '.join('m->' + o for o in outs) if outs else 'none (registers are not compared)'))
    parts.append('Write `void routine(M6 *m)` now.')
    return [{'role': 'system', 'content': SYSTEM}, {'role': 'user', 'content': '\n\n'.join(parts)}], outs, r


if __name__ == '__main__':
    b, a = sys.argv[1].split(':')
    msgs, outs, r = build(int(b), int(a, 16))
    for m in msgs:
        print('=== %s ===\n%s\n' % (m['role'], m['content']))
