"""gap_context.py: for every still-unseen PRG run, who reads the bytes around it and in which stage/section.

  python tools/gap_context.py [--min 32] [--margin 48] [--dir DEBUGGER_DIR]
Uses every .cdl and auto-reads-*.tsv in the Debugger folder. Prints one line per gap: bank, start, length,
whether the bytes just before/after were code or data, and the reading routines and (level, section) pairs of
the neighbouring reads, with level IDs also given as the stage number of the TAS route (level 3 = stage 1, 5 = 2,
2 = 3, 4 = 4, 8 = 5, 9 = 6, 6 = 7; 00 = title/menus, FD/FE/FF = transitions). Addresses and counts only.
"""
import argparse, collections, glob, os, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import covreport

STAGE = {'03': '1', '05': '2', '02': '3', '04': '4', '08': '5', '09': '6', '06': '7', '00': 'title'}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--dir', default=covreport.DEFAULT_DIR)
    ap.add_argument('--min', type=int, default=32)
    ap.add_argument('--margin', type=int, default=48)
    a = ap.parse_args()
    m = covreport.merge(covreport.load(f) for f in glob.glob(os.path.join(a.dir, '*.cdl')))
    gaps = []
    for b in range(8):
        i, end = b * 0x8000, (b + 1) * 0x8000
        while i < end:
            if m[i] == 0:
                j = i
                while j < end and m[j] == 0:
                    j += 1
                if j - i >= a.min:
                    gaps.append((b, i, j - i))
                i = j
            else:
                i += 1
    area = {g: collections.Counter() for g in gaps}
    rout = {g: collections.Counter() for g in gaps}
    by_bank = collections.defaultdict(list)
    for g in gaps:
        by_bank[g[0]].append(g)
    for f in glob.glob(os.path.join(a.dir, 'auto-reads-*.tsv')):
        for line in open(f):
            c = line.split('\t')
            if c[0] == 'bank':
                continue
            b = int(c[0])
            off = b * 0x8000 + int(c[1][1:], 16) - 0x8000
            for g in by_bank.get(b, ()):
                if g[1] - a.margin <= off < g[1] + g[2] + a.margin:
                    n = int(c[6])
                    area[g]['L%s/S%s' % (c[4], c[5])] += n
                    rout[g][c[3] if c[3] != '-' else c[2]] += n
    def kind(off):
        if off < 0 or off >= covreport.SIZE:
            return '-'
        f = m[off]
        return 'code' if f & 1 else ('data' if f & 2 else '-')
    total = sum(g[2] for g in gaps)
    print('%d gaps >= %d bytes, %d bytes' % (len(gaps), a.min, total))
    for g in sorted(gaps, key=lambda g: -g[2]):
        b, off, n = g
        ar = ', '.join('%s(st %s)x%d' % (k, STAGE.get(k[1:3], '?'), v) for k, v in area[g].most_common(3)) or 'no reads nearby'
        rt = ', '.join('%s x%d' % kv for kv in rout[g].most_common(2))
        print('%d $%04X %4d  before:%-4s after:%-4s  %s  | readers %s' % (b, 0x8000 + (off & 0x7FFF), n, kind(off - 1),
                                                                         kind(off + n), ar, rt))


if __name__ == '__main__':
    main()
