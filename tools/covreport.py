"""covreport.py: how much of PRG the code/data logs have seen, per bank and per source.

Reads .cdl files the way btdd-decomp's tools/cdl.py does (FCEUX raw 256 KB: bits 0|4 code, 1|5 data;
Mesen "CDLv..." header + 256 KB: bit 0 code, bit 1 data) and prints counts only (no ROM bytes).

  python tools/covreport.py                       all logs in ..\\Gamefiles\\Debugger, merged + per file
  python tools/covreport.py --base "*" --new "auto-*"   gain of the auto-* logs over all the others
  python tools/covreport.py --regions [--min 16]  list still-unseen PRG runs (bank, start, length)
  python tools/covreport.py --dir D               use another folder
"""
import argparse, fnmatch, glob, os, sys

HERE = os.path.dirname(os.path.abspath(__file__))
DEFAULT_DIR = os.path.normpath(os.path.join(HERE, '..', '..', 'Gamefiles', 'Debugger'))
SIZE = 0x40000
CODE, DATA = 1, 2


def load(path):
    raw = open(path, 'rb').read()
    if len(raw) < SIZE:
        return None
    if len(raw) == SIZE and not raw.startswith(b'CDLv'):
        return bytes((CODE if b & 0x11 else 0) | (DATA if b & 0x22 else 0) for b in raw)
    return bytes(b & 3 for b in raw[len(raw) - SIZE:])


def merge(maps):
    out = bytearray(SIZE)
    for m in maps:
        for i, b in enumerate(m):
            if b:
                out[i] |= b
    return bytes(out)


def stats(m, bank=None):
    rng = range(SIZE) if bank is None else range(bank * 0x8000, (bank + 1) * 0x8000)
    c = d = s = 0
    for i in rng:
        b = m[i]
        c += b & CODE != 0
        d += (b & DATA != 0) and not b & CODE
        s += b != 0
    return c, d, s


def line(name, m, total):
    c, d, s = stats(m)
    return '%-42s code %6d (%4.1f%%)  data-only %6d (%4.1f%%)  seen %6d (%4.1f%%)' % (
        name, c, 100.0 * c / total, d, 100.0 * d / total, s, 100.0 * s / total)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--dir', default=DEFAULT_DIR)
    ap.add_argument('--base', default=None, help='glob of the baseline logs (default: all not matching --new)')
    ap.add_argument('--new', default=None, help='glob of the new logs')
    ap.add_argument('--regions', action='store_true')
    ap.add_argument('--min', type=int, default=16)
    a = ap.parse_args()
    files = sorted(glob.glob(os.path.join(a.dir, '*.cdl')))
    maps = {}
    for f in files:
        m = load(f)
        if m is not None:
            maps[os.path.basename(f)] = m
    names = list(maps)
    if a.new:
        new = [n for n in names if fnmatch.fnmatch(n, a.new)]
        base = [n for n in names if n not in new and fnmatch.fnmatch(n, a.base or '*')]
    else:
        new, base = [], names
    mb = merge(maps[n] for n in base)
    print('per source:')
    for n in names:
        print('  ' + line(n, maps[n], SIZE))
    print(line('BASE (%d logs merged)' % len(base), mb, SIZE))
    if new:
        mn = merge(maps[n] for n in new)
        ma = merge([mb, mn])
        print(line('NEW (%d logs merged)' % len(new), mn, SIZE))
        print(line('ALL', ma, SIZE))
        nc = sum(1 for i in range(SIZE) if ma[i] & CODE and not mb[i] & CODE)
        nd = sum(1 for i in range(SIZE) if ma[i] & DATA and not mb[i] & 3)
        print('new over base: %d code bytes, %d data-only bytes never seen before' % (nc, nd))
    else:
        ma = mb
    print('\nper bank (merged %s):' % ('base + new' if new else 'all'))
    print('  bank   code   data-only   seen     unseen' + ('   | base code  base seen' if new else ''))
    for b in range(8):
        c, d, s = stats(ma, b)
        extra = ''
        if new:
            bc, bd, bs = stats(mb, b)
            extra = '   | %6d    %6d' % (bc, bs)
        print('  %d    %6d  %6d   %6d (%4.1f%%) %6d%s' % (b, c, d, s, 100.0 * s / 0x8000, 0x8000 - s, extra))
    if a.regions:
        print('\nunseen runs >= %d bytes (bank $start len):' % a.min)
        for b in range(8):
            i = b * 0x8000
            end = i + 0x8000
            while i < end:
                if ma[i] == 0:
                    j = i
                    while j < end and ma[j] == 0:
                        j += 1
                    if j - i >= a.min:
                        print('  %d $%04X %d' % (b, 0x8000 + (i & 0x7FFF), j - i))
                    i = j
                else:
                    i += 1


if __name__ == '__main__':
    sys.exit(main())
