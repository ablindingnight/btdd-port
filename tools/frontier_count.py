"""frontier_count.py: how many Carve / data-carve tasks a set of code/data logs would unlock in btdd-decomp.

Read-only: it only reads btdd-decomp's src/bank*.s (to find the still-raw .incbin blocks) and the .cdl files of
a folder, and applies the same rules as btdd-decomp's tools/cdlfrontier.py (runs of executed bytes inside raw
blocks, >= 4 bytes; without its carve.py dry-run check) and tools/datacarve.py (runs of data-only bytes inside
raw blocks, >= 8 bytes). Nothing is written.
  python tools/frontier_count.py CDL_DIR [CDL_DIR2 ...]
"""
import os, re, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import covreport

DECOMP = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', 'btdd-decomp'))
INC = re.compile(r'\.incbin\s+"orig/bank(\d)\.bin",\s*\$?([0-9A-Fa-f]+),\s*\$?([0-9A-Fa-f]+)')


def raw_blocks():
    out = []
    for b in range(8):
        s = open(os.path.join(DECOMP, 'src', 'bank%d.s' % b), encoding='utf-8').read()
        for bb, o, n in INC.findall(s):
            if int(bb) == b:
                out.append((b, int(o, 16), int(n, 16)))
    return out


def runs(m, blocks, pred, minlen):
    res = []
    for b, o, n in blocks:
        i = o
        while i < o + n:
            if pred(m[b * 0x8000 + i]):
                j = i
                while j < o + n and pred(m[b * 0x8000 + j]):
                    j += 1
                if j - i >= minlen:
                    res.append((b, i, j - i))
                i = j
            else:
                i += 1
    return res


def main():
    import glob
    blocks = raw_blocks()
    print('btdd-decomp raw .incbin blocks: %d, %d bytes' % (len(blocks), sum(n for _, _, n in blocks)))
    for d in sys.argv[1:]:
        m = covreport.merge(covreport.load(f) for f in glob.glob(os.path.join(d, '*.cdl')))
        code = runs(m, blocks, lambda f: f & 1, 4)
        data = runs(m, blocks, lambda f: (f & 3) == 2, 8)
        unseen = runs(m, blocks, lambda f: f == 0, 1)
        print('%s:\n  executed runs in raw blocks (cdlfrontier, >= 4 bytes): %d runs, %d bytes'
              '\n  data-only runs in raw blocks (datacarve, >= 8 bytes): %d tables, %d bytes'
              '\n  raw bytes no log has seen: %d' % (d, len(code), sum(r[2] for r in code), len(data),
                                                     sum(r[2] for r in data), sum(r[2] for r in unseen)))


if __name__ == '__main__':
    main()
