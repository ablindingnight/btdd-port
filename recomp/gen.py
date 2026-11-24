"""recomp/gen.py: M5 prototype generator. Player's ROM + meta/btdd.meta -> C, one function per routine.

The output is derived from the ROM, so it is written OUTSIDE the repo (default ..\\Gamefiles\\Movies\\m5\\generated)
and must never be committed.

  python recomp/gen.py [--rom PATH] [--meta meta/btdd.meta] [--out DIR] [--conf verified,static]
                       [--exclude-timing] [--exclude b:addr,...]

Each routine becomes `void r<bank>_<addr>(Cpu *c)`: a switch on c->pc to the instruction it should start at,
then straight-line C with gotos for branches/jumps inside the routine. Every instruction is an entry point
(the run loop may stop at any instruction boundary for the frame budget or an interrupt and come back later).
Semantics mirror src/runtime/cpu.c execute() exactly: cycles charged at the same points, same bus accesses
in the same order (including dummy reads and the RMW double write), same open-bus value before data accesses.
JSR/RTS/RTI/indirect jumps/jumps out of the routine return to the run loop (recomp/native_cpu.c), which looks up
the next native function by (bank, PC). BRK and anything undecodable are left to the interpreter.
"""
import argparse
import collections
import hashlib
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
sys.path.insert(0, os.path.join(REPO, 'tools'))
from m6502 import decode, BRANCHES  # noqa: E402
from meta_check import parse  # noqa: E402

DEF_ROM = os.path.join(REPO, '..', 'Gamefiles', 'Battletoads-Double Dragon (USA).nes')
DEF_META = os.path.join(REPO, 'meta', 'btdd.meta')
DEF_OUT = os.path.join(REPO, '..', 'Gamefiles', 'Movies', 'm5', 'generated')

BASE_CYCLES = [
    7, 6, 2, 8, 3, 3, 5, 5, 3, 2, 2, 2, 4, 4, 6, 6, 2, 5, 2, 8, 4, 4, 6, 6, 2, 4, 2, 7, 4, 4, 7, 7,
    6, 6, 2, 8, 3, 3, 5, 5, 4, 2, 2, 2, 4, 4, 6, 6, 2, 5, 2, 8, 4, 4, 6, 6, 2, 4, 2, 7, 4, 4, 7, 7,
    6, 6, 2, 8, 3, 3, 5, 5, 3, 2, 2, 2, 3, 4, 6, 6, 2, 5, 2, 8, 4, 4, 6, 6, 2, 4, 2, 7, 4, 4, 7, 7,
    6, 6, 2, 8, 3, 3, 5, 5, 4, 2, 2, 2, 5, 4, 6, 6, 2, 5, 2, 8, 4, 4, 6, 6, 2, 4, 2, 7, 4, 4, 7, 7,
    2, 6, 2, 6, 3, 3, 3, 3, 2, 2, 2, 2, 4, 4, 4, 4, 2, 6, 2, 6, 4, 4, 4, 4, 2, 5, 2, 5, 5, 5, 5, 5,
    2, 6, 2, 6, 3, 3, 3, 3, 2, 2, 2, 2, 4, 4, 4, 4, 2, 5, 2, 5, 4, 4, 4, 4, 2, 4, 2, 4, 4, 4, 4, 4,
    2, 6, 2, 8, 3, 3, 5, 5, 2, 2, 2, 2, 4, 4, 6, 6, 2, 5, 2, 8, 4, 4, 6, 6, 2, 4, 2, 7, 4, 4, 7, 7,
    2, 6, 3, 8, 3, 3, 5, 5, 2, 2, 2, 2, 4, 4, 6, 6, 2, 5, 2, 8, 4, 4, 6, 6, 2, 4, 2, 7, 4, 4, 7, 7,
]

LOADS = {'lda': 'a', 'ldx': 'x', 'ldy': 'y'}
STORES = {'sta': 'a', 'stx': 'x', 'sty': 'y'}
ALU = {'ora': 'c->a |= v; n_zn(c, c->a);', 'and': 'c->a &= v; n_zn(c, c->a);', 'eor': 'c->a ^= v; n_zn(c, c->a);',
       'adc': 'n_adc(c, v);', 'sbc': 'n_sbc(c, v);', 'cmp': 'n_cmp(c, c->a, v);', 'cpx': 'n_cmp(c, c->x, v);',
       'cpy': 'n_cmp(c, c->y, v);', 'bit': 'n_bit(c, v);'}
RMW = {'asl': 'n_asl', 'lsr': 'n_lsr', 'rol': 'n_rol', 'ror': 'n_ror', 'inc': 'n_inc', 'dec': 'n_dec'}
COND = {'bpl': '!(c->p & CPU_N)', 'bmi': '(c->p & CPU_N)', 'bvc': '!(c->p & CPU_V)', 'bvs': '(c->p & CPU_V)',
        'bcc': '!(c->p & CPU_C)', 'bcs': '(c->p & CPU_C)', 'bne': '!(c->p & CPU_Z)', 'beq': '(c->p & CPU_Z)'}
IMPLIED = {
    'tax': 'c->x = c->a; n_zn(c, c->x);', 'txa': 'c->a = c->x; n_zn(c, c->a);',
    'tay': 'c->y = c->a; n_zn(c, c->y);', 'tya': 'c->a = c->y; n_zn(c, c->a);',
    'tsx': 'c->x = c->s; n_zn(c, c->x);', 'txs': 'c->s = c->x;',
    'inx': 'c->x++; n_zn(c, c->x);', 'dex': 'c->x--; n_zn(c, c->x);',
    'iny': 'c->y++; n_zn(c, c->y);', 'dey': 'c->y--; n_zn(c, c->y);',
    'clc': 'c->p &= (uint8_t)~CPU_C;', 'sec': 'c->p |= CPU_C;', 'cli': 'c->p &= (uint8_t)~CPU_I;',
    'sei': 'c->p |= CPU_I;', 'clv': 'c->p &= (uint8_t)~CPU_V;', 'cld': 'c->p &= (uint8_t)~CPU_D;',
    'sed': 'c->p |= CPU_D;', 'nop': '',
    'pha': 'n_push(c, c->a);', 'php': 'n_push(c, (uint8_t)(c->p | CPU_U | CPU_B));',
    'pla': 'c->a = n_pop(c); n_zn(c, c->a);', 'plp': 'c->p = n_pop(c);',
}


class Gen:
    def __init__(self, bank, mem):
        self.b, self.mem = bank, mem
        self.unsupported = collections.Counter()

    def byte(self, a):
        return self.mem[a - 0x8000]

    # ---- effective address / operand access. Each returns (setup lines, read expr builder, write builder)
    def operand(self, a, d, lb):
        """Lines that compute the operand value into v for a read instruction."""
        m, mode, n, v = d
        if mode == 'imm':
            return ['v = 0x%02X;' % v]
        if mode == 'zp':
            return ['c->bus = 0x%02X;' % lb, 'v = n_rdram(c, 0x%02X);' % v]
        if mode in ('zpx', 'zpy'):
            r = 'x' if mode == 'zpx' else 'y'
            return ['c->bus = 0x%02X;' % lb, 'v = n_rdram(c, (uint8_t)(0x%02X + c->%s));' % (v, r)]
        if mode == 'abs':
            fn = 'n_rdram' if v < 0x2000 else 'n_rd'
            return ['c->bus = 0x%02X;' % lb, 'v = %s(c, 0x%04X);' % (fn, v)]
        if mode in ('absx', 'absy'):
            r = 'x' if mode == 'absx' else 'y'
            ram = v + 0xFF < 0x2000
            out = ['c->bus = 0x%02X;' % lb, 'ea = (uint16_t)(0x%04X + c->%s);' % (v, r),
                   'if ((ea ^ 0x%04X) & 0x100) { %sn_cyc(c, 1); }' % (v, '' if ram else 'n_rd(c, (uint16_t)(ea ^ 0x100)); ')]
            out.append('v = %s(c, ea);' % ('n_rdram' if ram else 'n_rdx'))
            return out
        if mode == 'indx':
            return ['c->bus = 0x%02X;' % lb, 'zp = (uint8_t)(0x%02X + c->x);' % v,
                    'ea = n_rdram(c, zp); ea |= (uint16_t)(n_rdram(c, (uint8_t)(zp + 1)) << 8);',
                    'v = n_rdx(c, ea);']
        if mode == 'indy':
            return ['c->bus = 0x%02X;' % lb,
                    'base = n_rdram(c, 0x%02X); base |= (uint16_t)(n_rdram(c, 0x%02X) << 8);' % (v, (v + 1) & 0xFF),
                    'ea = (uint16_t)(base + c->y);',
                    'if ((ea ^ base) & 0x100) { n_rdx(c, (uint16_t)(ea ^ 0x100)); n_cyc(c, 1); }',
                    'v = n_rdx(c, ea);']
        raise ValueError(mode)

    def store_addr(self, d, lb):
        """Lines computing ea for a store/RMW (with the interpreter's dummy reads); returns (lines, static ea or
        None, ram-only flag)."""
        m, mode, n, v = d
        if mode == 'zp':
            return ['c->bus = 0x%02X;' % lb], v, True
        if mode in ('zpx', 'zpy'):
            r = 'x' if mode == 'zpx' else 'y'
            return ['c->bus = 0x%02X;' % lb, 'ea = (uint8_t)(0x%02X + c->%s);' % (v, r)], None, True
        if mode == 'abs':
            return ['c->bus = 0x%02X;' % lb], v, v < 0x2000
        if mode in ('absx', 'absy'):
            r = 'x' if mode == 'absx' else 'y'
            ram = v + 0xFF < 0x2000
            out = ['c->bus = 0x%02X;' % lb, 'ea = (uint16_t)(0x%04X + c->%s);' % (v, r)]
            if not ram:
                out.append('n_rd(c, (uint16_t)((ea & 0x00FF) | 0x%04X));' % (v & 0xFF00))
            return out, None, ram
        if mode == 'indx':
            return ['c->bus = 0x%02X;' % lb, 'zp = (uint8_t)(0x%02X + c->x);' % v,
                    'ea = n_rdram(c, zp); ea |= (uint16_t)(n_rdram(c, (uint8_t)(zp + 1)) << 8);'], None, False
        if mode == 'indy':
            return ['c->bus = 0x%02X;' % lb,
                    'base = n_rdram(c, 0x%02X); base |= (uint16_t)(n_rdram(c, 0x%02X) << 8);' % (v, (v + 1) & 0xFF),
                    'ea = (uint16_t)(base + c->y);',
                    'n_rdx(c, (uint16_t)((ea & 0x00FF) | (base & 0xFF00)));'], None, False
        raise ValueError(mode)

    def insn(self, a, d, inside, nxt):
        """C lines for one instruction (after its entry label and base-cycle charge). inside(t) -> label or None.
        nxt = address of the next instruction."""
        m, mode, n, v = d
        lb = self.byte(a + n - 1)
        L = []

        def leave(t):
            lab = inside(t)
            return 'goto L%04X;' % t if lab else 'c->pc = 0x%04X; return;' % t

        if m in LOADS:
            L += self.operand(a, d, lb)
            L.append('c->%s = v; n_zn(c, v);' % LOADS[m])
        elif m in ALU:
            L += self.operand(a, d, lb)
            L.append(ALU[m])
        elif m in STORES:
            lines, sea, ram = self.store_addr(d, lb)
            L += lines
            val = 'c->' + STORES[m]
            if sea is not None:
                L.append('%s(c, 0x%04X, %s);' % ('n_wrram' if ram else 'n_wr', sea, val))
                if sea >= 0x4020:
                    L.append('c->pc = 0x%04X; return; /* mapper write: bank may change */' % nxt)
            else:
                L.append('%s(c, ea, %s);' % ('n_wrram' if ram else 'n_wrx', val))
                if not ram:
                    L.append('if (NES(c)->prg_bank != %d) { c->pc = 0x%04X; return; }' % (self.b, nxt))
        elif m in RMW and mode == 'acc':
            L.append('c->a = %s(c, c->a);' % RMW[m])
        elif m in RMW:
            lines, sea, ram = self.store_addr(d, lb)
            L += lines
            f = RMW[m]
            e = '0x%04X' % sea if sea is not None else 'ea'
            if mode in ('zp', 'zpx'):  # zero-page RMW: no dummy write
                L.append('v = n_rdram(c, %s); n_wrram(c, %s, %s(c, v));' % (e, e, f))
            elif ram:
                L.append('v = n_rdram(c, %s); n_wrram(c, %s, v); n_wrram(c, %s, %s(c, v));' % (e, e, e, f))
            else:
                rd_, wr_ = ('n_rd', 'n_wr') if sea is not None else ('n_rdx', 'n_wrx')
                L.append('v = %s(c, %s); %s(c, %s, v); %s(c, %s, %s(c, v));' % (rd_, e, wr_, e, wr_, e, f))
                if sea is None or sea >= 0x4020:
                    L.append('if (NES(c)->prg_bank != %d) { c->pc = 0x%04X; return; }' % (self.b, nxt))
        elif m in IMPLIED:
            if m in ('pha', 'php', 'pla', 'plp'):
                L.append('c->bus = 0x%02X;' % lb)
            L.append(IMPLIED[m])
        elif m in BRANCHES:
            t = v
            extra = 2 if ((nxt ^ t) & 0x100) else 1
            L.append('if (%s) { n_cyc(c, %d); %s }' % (COND[m], extra, leave(t)))
        elif m == 'jmp' and mode == 'abs':
            L.append(leave(v))
        elif m == 'jmp' and mode == 'ind':
            hi = ((v + 1) & 0x00FF) | (v & 0xFF00)
            rdf = 'n_rdram' if v < 0x2000 else 'n_rd'
            L.append('c->bus = 0x%02X;' % lb)
            L.append('ea = %s(c, 0x%04X); ea |= (uint16_t)(%s(c, 0x%04X) << 8);' % (rdf, v, rdf, hi))
            L.append('c->pc = ea; return;')
        elif m == 'jsr':
            ret = (a + 2) & 0xFFFF
            L.append('c->bus = 0x%02X;' % self.byte(a + 1))
            L.append('n_push(c, 0x%02X); n_push(c, 0x%02X);' % (ret >> 8, ret & 0xFF))
            L.append('c->bus = 0x%02X;' % (v >> 8))
            L.append('c->pc = 0x%04X; return;' % v)
        elif m == 'rts':
            L.append('c->bus = 0x%02X;' % lb)
            L.append('ea = n_pop(c); ea |= (uint16_t)(n_pop(c) << 8); c->pc = (uint16_t)(ea + 1); return;')
        elif m == 'rti':
            L.append('c->bus = 0x%02X;' % lb)
            L.append('c->p = n_pop(c); c->p_before = c->p;')
            L.append('ea = n_pop(c); ea |= (uint16_t)(n_pop(c) << 8); c->pc = ea; return;')
        else:
            return None
        return L

    def routine(self, name, start, end):
        """C text of one routine, and the list of registered entry addresses."""
        code, a = [], start
        while a <= end:
            d = decode(self.mem, a)
            if d is None or a + d[2] - 1 > end:
                break
            code.append((a, d))
            a += d[2]
        addrs = set(x for x, _ in code)
        body, entries = [], []
        for i, (a, d) in enumerate(code):
            nxt = (a + d[2]) & 0xFFFF
            lines = self.insn(a, d, lambda t: t in addrs, nxt)
            if lines is None:
                self.unsupported[d[0]] += 1
                body.append('L%04X: c->pc = 0x%04X; return; /* %s: interpreter */' % (a, a, d[0]))
                continue
            lines = [x.replace('return;', 'N_EXIT;') for x in lines]
            entries.append(a)
            op = self.byte(a)
            body.append('L%04X: if (c->budget <= 0 || c->irq) { c->pc = 0x%04X; return; }' % (a, a))
            body.append('E%04X: c->p_before = c->p; n_cyc(c, %d); c->instructions++; c->last_op = 0x%02X; '
                        'native_insns++;' % (a, BASE_CYCLES[op], op))
            body += ['    ' + x for x in lines]
        if code:
            last_a, last_d = code[-1]
            fall = (last_a + last_d[2]) & 0xFFFF
            body.append('    c->pc = 0x%04X; N_EXIT; /* falls out of the routine */' % fall)
        fn = 'r%d_%04x' % (self.b, start)
        out = ['/* %s $%04X-$%04X */' % (name, start, end),
               'static void %s(Cpu *c)' % fn, '{',
               '    uint8_t v; uint16_t ea, base; uint8_t zp;',
               '    (void)v; (void)ea; (void)base; (void)zp;',
               '    switch (c->pc) {']
        out += ['    case 0x%04X: goto E%04X;' % (x, x) for x in entries]
        out += ['    default: return;', '    }']
        out += body
        out.append('}')
        return fn, entries, '\n'.join(out)




def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--rom', default=DEF_ROM)
    ap.add_argument('--meta', default=DEF_META)
    ap.add_argument('--out', default=DEF_OUT)
    ap.add_argument('--conf', default='verified,static', help='code confidences to compile natively')
    ap.add_argument('--exclude-timing', action='store_true', help='leave routines with timing sites to the interpreter')
    ap.add_argument('--exclude', default='', help='comma list of bank:addr routine starts to leave out')
    a = ap.parse_args()

    raw = open(a.rom, 'rb').read()
    sha = hashlib.sha1(raw).hexdigest()
    version, recs = parse(a.meta)
    want_sha = next(r[2]['sha1'] for r in recs if r[0] == 'rom')
    if sha != want_sha:
        raise SystemExit('ROM SHA-1 %s does not match the meta (%s)' % (sha, want_sha))
    prg = raw[16:]
    mems = [prg[b * 0x8000:(b + 1) * 0x8000] for b in range(8)]
    confs = set(a.conf.split(','))
    excl = set()
    for x in filter(None, a.exclude.split(',')):
        b, ad = x.split(':')
        excl.add((int(b), int(ad, 16)))

    timing = collections.defaultdict(list)
    for t, pos, kv, n in recs:
        if t == 'timing':
            timing[int(pos[0])].append((int(pos[1][1:], 16), pos[2]))
    routines = [(int(p[0]), int(p[1][1:], 16), int(p[2][1:], 16), p[3], kv) for t, p, kv, n in recs if t == 'code']

    os.makedirs(a.out, exist_ok=True)
    stats = collections.Counter()
    skipped = []
    regs = []
    for b in range(8):
        g = Gen(b, mems[b])
        parts = ['/* GENERATED from the player\'s ROM by the btdd recompiler. ROM-derived: never commit or distribute. */',
                 '#include "native_rt.h"', '']
        for rb, s, e, name, kv in routines:
            if rb != b:
                continue
            why = None
            if kv.get('conf') not in confs:
                why = 'conf=' + kv.get('conf')
            elif (b, s) in excl:
                why = 'excluded'
            elif a.exclude_timing and any(s <= x <= e for x, k in timing[b] if k != 'bank_switch'):
                why = 'timing:' + ','.join(sorted(set(k for x, k in timing[b] if s <= x <= e)))
            if why:
                skipped.append((b, s, e, name, why))
                stats['skipped'] += 1
                continue
            fn, entries, text = g.routine(name, s, e)
            parts.append(text)
            parts.append('static const uint16_t %s_at[] = {%s};' % (fn, ','.join('0x%04X' % x for x in entries)))
            parts.append('')
            regs.append((b, fn, len(entries)))
            stats['routines'] += 1
            stats['entries'] += len(entries)
        parts.append('void native_register_b%d(void)' % b)
        parts.append('{')
        for rb, fn, k in regs:
            if rb == b and k:
                parts.append('    native_register(%d, %s, %s_at, %d);' % (b, fn, fn, k))
        parts.append('}')
        with open(os.path.join(a.out, 'native_b%d.c' % b), 'w', newline='\n') as f:
            f.write('\n'.join(parts) + '\n')
        for k, cnt in g.unsupported.items():
            stats['unsupported_' + k] += cnt
    with open(os.path.join(a.out, 'native_all.c'), 'w', newline='\n') as f:
        f.write('#include "native_rt.h"\n')
        for b in range(8):
            f.write('void native_register_b%d(void);\n' % b)
        f.write('void native_register_all(void)\n{\n')
        for b in range(8):
            f.write('    native_register_b%d();\n' % b)
        f.write('}\n')
    with open(os.path.join(a.out, 'skipped.txt'), 'w', newline='\n') as f:
        for b, s, e, name, why in skipped:
            f.write('%d $%04X $%04X %s %s\n' % (b, s, e, name, why))
    print('gen: %d routines, %d native entry points, %d left to the interpreter (%s); unsupported: %s' % (
        stats['routines'], stats['entries'], stats['skipped'], os.path.join(a.out, 'skipped.txt'),
        ', '.join('%s %d' % (k[12:], v) for k, v in stats.items() if k.startswith('unsupported_')) or 'none'))


if __name__ == '__main__':
    sys.exit(main())
