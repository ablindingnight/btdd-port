"""m6_lint.py: readability score of passing M6 C (private/m6/*.c), to rank what the simplify pass should rewrite.

  python tools/m6/m6_lint.py [FILE ...]        default: every file in private/m6, worst first

Counts per routine (higher = less readable): raw RAM(0x...) addresses that have a name in the RAM map, gotos,
assignments to flags that are not outputs of the routine, comment lines that just repeat a 6502 mnemonic, and
lines per 6502 instruction. Score = 3*named_raw + 2*goto + 1*flag_noise + 1*asm_comments + 10*max(0, lines/insn-2).
"""
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import m6_pick  # noqa: E402
import m6_prompt  # noqa: E402
import m6_test  # noqa: E402

MNEMONICS = ('lda ldx ldy sta stx sty adc sbc and ora eor cmp cpx cpy bit inc dec inx iny dex dey asl lsr rol ror '
             'jmp jsr rts bne beq bcc bcs bmi bpl bvc bvs clc sec tax tay txa tya pha pla php plp').split()


def score(path, outputs=''):
    text = open(path, encoding='utf-8', errors='replace').read()
    body = '\n'.join(text.splitlines()[1:])  # skip the PASS header comment
    named = {a for n, a, size, kind, naming in m6_test.ram_names() if a < 0x800}
    raw = [int(x, 16) for x in re.findall(r'RAM\(\s*0x([0-9A-Fa-f]+)\s*\)', body)]
    named_raw = sum(1 for a in raw if a in named)
    gotos = len(re.findall(r'\bgoto\b', body))
    flag_noise = sum(1 for f in 'czvn' if f not in outputs
                     for _ in re.finditer(r'm->%s\s*=' % f, body))
    asm_comments = sum(1 for l in body.splitlines()
                       if re.match(r'\s*(//|/\*)\s*(%s)\b' % '|'.join(MNEMONICS), l, flags=re.I))
    lines = sum(1 for l in body.splitlines() if l.strip() and not l.strip().startswith(('//', '/*', '*')))
    return {'named_raw': named_raw, 'goto': gotos, 'flag_noise': flag_noise, 'asm_comments': asm_comments,
            'lines': lines}


def total(s, insns):
    return (3 * s['named_raw'] + 2 * s['goto'] + s['flag_noise'] + s['asm_comments'] +
            10 * max(0.0, s['lines'] / max(insns, 1) - 2))


def routine_info():
    """name -> (bank, addr, instruction count, outputs)"""
    raw = open(m6_test.ROM, 'rb').read()[16:]
    out = {}
    for r in m6_pick.routines():
        mem = raw[r['bank'] * 0x8000:(r['bank'] + 1) * 0x8000]
        n, a = 0, r['start']
        while a <= r['end']:
            d = m6_prompt.decode(mem, a)
            if d is None:
                break
            n += 1
            a += d[2]
        out[r['name']] = (r['bank'], r['start'], n, mem)
    return out


def main():
    files = sys.argv[1:] or sorted(os.path.join(m6_test.PRIVATE, f) for f in os.listdir(m6_test.PRIVATE)
                                   if f.endswith('.c'))
    info = routine_info()
    rows = []
    for f in files:
        name = os.path.splitext(os.path.basename(f))[0]
        if name not in info:
            continue
        bank, addr, insns, mem = info[name]
        outs, sites = m6_prompt.caller_outputs(bank, addr, mem)
        s = score(f, outs)
        rows.append((total(s, insns), name, insns, s))
    rows.sort(key=lambda x: -x[0])
    print('score  routine              insns  named_raw goto flag_noise asm_comments lines')
    for t, name, insns, s in rows:
        print('%5.1f  %-20s %5d  %9d %4d %10d %12d %5d' % (t, name, insns, s['named_raw'], s['goto'],
                                                         s['flag_noise'], s['asm_comments'], s['lines']))
    if rows:
        print('mean score %.1f over %d routines' % (sum(r[0] for r in rows) / len(rows), len(rows)))


if __name__ == '__main__':
    main()
