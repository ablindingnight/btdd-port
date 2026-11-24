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


def decomp_facts(bank, label, max_lines=40):
    """The decomp's read-only fact tool for one routine, run as `python ../btdd-decomp/tools/facts.py BANK LABEL`
    from this checkout (a sibling of btdd-decomp; the form the user's permission rule allows). None if it fails."""
    try:
        r = subprocess.run(['python', '../btdd-decomp/tools/facts.py', str(bank), label], cwd=REPO,
                           capture_output=True, text=True, timeout=60)
    except (OSError, subprocess.TimeoutExpired):
        return None
    if r.returncode != 0 or not r.stdout.strip():
        return None
    lines = r.stdout.strip().splitlines()
    if len(lines) > max_lines:
        lines = lines[:max_lines] + ['(... %d more lines)' % (len(lines) - max_lines)]
    return '\n'.join(lines)

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
Keep the reasoning short: translate instruction by instruction, then simplify; do not trace example states by
hand. Declare every local variable. Answer with exactly one ```c block that starts with `void routine(M6 *m)`."""


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
    """recomp/gen.py's C for the routine with the run-loop bookkeeping stripped: one '/* $addr */' marker per
    instruction, labels kept for branches, n_* helpers as in recomp/native_rt.h."""
    import gen  # recomp/gen.py
    g = gen.Gen(bank, mem)
    out = []
    for line in g.routine('', start, end)[2].splitlines():
        st = line.strip()
        m = re.match(r'^L([0-9A-F]{4}): if \(c->budget', line)
        if m:
            out.append('L%s:' % m.group(1))
            continue
        m = re.match(r'^E([0-9A-F]{4}): ', line)
        if m:
            out.append('    /* $%s */' % m.group(1))
            continue
        if (st.startswith(('static void', 'uint8_t v;', '(void)v', 'switch (c->pc)', 'case 0x', 'default: return',
                           'c->bus = ', '/*  $')) or st in ('{', '}')):
            continue
        out.append(line.replace('N_EXIT;', 'return;'))
    return '\n'.join(out)


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


# what each instruction reads / overwrites among A X Y C Z N V (for caller liveness)
def _uses(m, mode):
    reads, writes = set(), set()
    if m in ('sta', 'pha', 'tax', 'tay', 'cmp', 'and', 'ora', 'eor', 'adc', 'sbc', 'bit') or \
            (m in ('asl', 'lsr', 'rol', 'ror') and mode == 'acc'):
        reads.add('a')
    if m in ('stx', 'txa', 'txs', 'cpx', 'inx', 'dex') or mode in ('zpx', 'absx', 'indx'):
        reads.add('x')
    if m in ('sty', 'tya', 'cpy', 'iny', 'dey') or mode in ('zpy', 'absy', 'indy'):
        reads.add('y')
    if m in ('adc', 'sbc', 'rol', 'ror', 'bcc', 'bcs', 'php'):
        reads.add('c')
    if m in ('beq', 'bne', 'php'):
        reads.add('z')
    if m in ('bmi', 'bpl', 'php'):
        reads.add('n')
    if m in ('bvc', 'bvs', 'php'):
        reads.add('v')
    if m in ('lda', 'pla', 'txa', 'tya') or (m in ('and', 'ora', 'eor', 'adc', 'sbc')):
        writes.add('a')
    if m in ('ldx', 'tax', 'tsx'):
        writes.add('x')
    if m in ('ldy', 'tay'):
        writes.add('y')
    if m in ('adc', 'sbc', 'cmp', 'cpx', 'cpy', 'asl', 'lsr', 'rol', 'ror', 'clc', 'sec', 'plp'):
        writes.add('c')
    if m in ('lda', 'ldx', 'ldy', 'tax', 'tay', 'txa', 'tya', 'tsx', 'pla', 'inx', 'dex', 'iny', 'dey', 'inc', 'dec',
             'and', 'ora', 'eor', 'adc', 'sbc', 'cmp', 'cpx', 'cpy', 'asl', 'lsr', 'rol', 'ror', 'bit', 'plp'):
        writes.update(('z', 'n'))
    if m in ('adc', 'sbc', 'bit', 'clv', 'plp'):
        writes.add('v')
    return reads, writes


def caller_outputs(bank, addr, mem):
    """Registers/flags some caller reads after `jsr addr` before overwriting them (straight-line scan of up to 12
    instructions per call site in the same bank; anything that leaves the straight line counts as a read of
    everything still undecided). Returns (outputs string, number of call sites)."""
    live, sites = set(), 0
    for r in m6_pick.routines():
        if r['bank'] != bank:
            continue
        a = r['start']
        while a <= r['end']:
            d = decode(mem, a)
            if d is None:
                break
            if d[0] == 'jsr' and d[3] == addr:
                sites += 1
                live |= _live_after(mem, a + 3)
            a += d[2]
    return ''.join(r for r in 'axyczvn' if r in live), sites


REGS = frozenset('axy')


def _live_after(mem, start, budget=400):
    """Registers/flags read before being overwritten on some path from `start`: depth-first over branches (both
    ways), `jmp abs`, and into `jsr` callees (back to the return address at their RTS). Leaving the known code
    (an RTS with no pending return, an indirect jump, BRK/RTI, an undecodable byte, the step budget) counts as a
    read of the registers still pending, but not of the flags: a flag carried across a second return is rare,
    and counting it would make nearly every flag an output."""
    live = set()
    work = [(start, frozenset('axyczvn'), ())]
    seen = set()
    steps = 0
    while work:
        pc, pending, rets = work.pop()
        while pending:
            steps += 1
            if steps > budget or (pc, pending, rets) in seen:
                if steps > budget:
                    live |= pending & REGS
                break
            seen.add((pc, pending, rets))
            e = decode(mem, pc)
            if e is None:
                live |= pending & REGS
                break
            m, mode, n, v = e
            rd, wr = _uses(m, mode)
            live |= rd & pending
            pending = pending - rd - wr
            if m in ('rti', 'brk') or (m == 'jmp' and mode == 'ind'):
                live |= pending & REGS
                break
            if m == 'rts':
                if not rets:
                    live |= pending & REGS
                    break
                pc, rets = rets[-1], rets[:-1]
                continue
            if m == 'jsr':
                if v < 0x8000 or len(rets) > 4:
                    live |= pending & REGS
                    break
                rets, pc = rets + ((pc + 3) & 0xFFFF,), v
                continue
            if m == 'jmp':
                pc = v
                continue
            if mode == 'rel':
                work.append((v, pending, rets))
            pc = (pc + n) & 0xFFFF
    return live


def ram_reads(bank, addr):
    """RAM addresses (and pointer pairs) the routine names directly: shown with their entry values in FAIL lines."""
    raw = open(m6_test.ROM, 'rb').read()[16:]
    mem = raw[bank * 0x8000:(bank + 1) * 0x8000]
    r = next(x for x in m6_pick.routines() if x['bank'] == bank and x['start'] == addr)
    out = []
    for a, (m, mode, n, v) in m6_pick.analyse(mem, r)[2]:
        if v is not None and mode not in ('imm', 'rel') and v < 0x0800:
            for x in ([v, (v + 1) & 0xFF] if mode in ('indx', 'indy') else [v]):
                if x not in out:
                    out.append(x)
    return out[:16]


def build(bank, addr):
    raw = open(m6_test.ROM, 'rb').read()[16:]
    mem = raw[bank * 0x8000:(bank + 1) * 0x8000]
    r = next(x for x in m6_pick.routines() if x['bank'] == bank and x['start'] == addr)
    ins, a = [], r['start']
    while a <= r['end']:
        d = decode(mem, a)
        if d is None:
            break
        ins.append((a, d))
        a += d[2]
    starts = {x['start']: x for x in m6_pick.routines() if x['bank'] == bank}
    ok, callee_addrs = m6_pick.callees_of(mem, r, set(starts))
    r['callees'] = [(starts[c]['name'], c) for c in callee_addrs] if ok else []
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
    outs, sites = caller_outputs(bank, addr, mem)
    if not sites:  # only reached by jumps, dispatch slots or far calls: fall back to what it writes
        outs = outputs_of(ins)
    calls = m6_pick.call_counts().get((bank, addr), 0)
    asm = asm_listing(bank, r['name']) or '(decomp listing not found)'
    mech = mechanical_c(bank, r['start'], r['end'], mem)
    parts = ['Routine %s: bank %d, $%04X-$%04X, %d instructions, called %d times in the 2-player TAS.'
             % (r['name'], bank, r['start'], r['end'], len(ins), calls)]
    # off by default: the A/B (docs/M6_HARNESS.md) found no gain from it; M6_FACTS=1 turns it on
    facts = decomp_facts(bank, r['name']) if os.environ.get('M6_FACTS') and not os.environ.get('M6_NO_FACTS') else None
    if facts:
        parts.append('Facts about it from the decomp (tools/facts.py):\n```\n%s\n```' % facts)
    if r.get('proposed'):
        parts.append('Proposed name (unreviewed): %s' % r['proposed'])
    if used:
        parts.append('RAM names it uses (#defined addresses): ' +
                     ', '.join('%s=$%04X (%s)' % (n, a, k) for n, a, k in used))
    if tables:
        parts.append('It reads ROM at: ' + ', '.join('$%04X' % t for t in sorted(set(tables))) +
                     ' (use bus_read for these)')
    parts.append('6502 source (ca65):\n```\n%s\n```' % asm)
    if re.search(r'^\s*(@\w+:\s*)?\.byte', asm, flags=re.M):
        # the decomp writes instructions as .byte where their bytes double as table data: show the real ones
        fmt = {'imp': '', 'acc': ' a', 'imm': ' #$%02X', 'zp': ' $%02X', 'zpx': ' $%02X,x', 'zpy': ' $%02X,y',
               'abs': ' $%04X', 'absx': ' $%04X,x', 'absy': ' $%04X,y', 'ind': ' ($%04X)', 'indx': ' ($%02X,x)',
               'indy': ' ($%02X),y', 'rel': ' $%04X'}
        dis = '\n'.join('$%04X  %s%s' % (a, m, (fmt[mode] % v) if '%' in fmt[mode] else fmt[mode])
                        for a, (m, mode, n, v) in ins)
        parts.append('NOTE: the source above writes some instructions as .byte (their bytes are also read as table '
                     'data). The disassembly of the ROM is authoritative:\n```\n%s\n```' % dis)
    parts.append('The same code translated mechanically, one block per instruction (c->a/x/y/p are the registers, '
                 'n_adc/n_sbc/n_cmp/n_rol... do exactly what the 6502 instruction does including flags, n_rdram/'
                 'n_wrram are RAM, n_rd/n_wr are the bus, `goto L....` are branches, n_cyc is timing: ignore it). '
                 'Use it to check the semantics, then write clean C:\n```c\n%s\n```' % mech)
    if r['callees']:
        lines = ['It calls other routines. Call each as a C function instead of a JSR; they take and return the '
                 'registers and flags in m exactly like the 6502 subroutine (set m->a/x/y before the call if the '
                 'callee reads them, and use what it leaves in m afterwards):']
        for name, ca in r['callees']:
            co, _ = caller_outputs(bank, ca, mem)
            lines.append('- `void %s(M6 *m);` ($%04X), callers use its: %s' % (
                name, ca, ', '.join('m->' + o for o in co) or 'nothing (only its RAM/hardware effects)'))
            mine = os.path.join(m6_test.PRIVATE, name + '.c')
            if os.path.isfile(mine):
                body = [l for l in open(mine, encoding='utf-8').read().splitlines()[1:]]
                if len(body) <= 60:
                    lines.append('  its C (`routine` is `%s`):\n```c\n%s\n```' % (name, '\n'.join(body)))
            else:  # no C yet: show what it does (it runs as the original code; just call it)
                src = asm_listing(bank, name)
                if src and len(src.splitlines()) <= 50:
                    lines.append('  no C yet (calling it runs the original); its 6502 code, for what it reads and '
                                 'writes:\n```\n%s\n```' % src)
        parts.append('\n'.join(lines))
    parts.append('Outputs the test compares besides RAM and hardware accesses: %s.' %
                 (', '.join('m->' + o for o in outs) if outs else 'none (registers are not compared)'))
    parts.append('Write `void routine(M6 *m)` now.')
    return [{'role': 'system', 'content': SYSTEM}, {'role': 'user', 'content': '\n\n'.join(parts)}], outs, r


if __name__ == '__main__':
    b, a = sys.argv[1].split(':')
    msgs, outs, r = build(int(b), int(a, 16))
    for m in msgs:
        print('=== %s ===\n%s\n' % (m['role'], m['content']))
