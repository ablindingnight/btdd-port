"""meta_check.py: load meta/btdd.meta + the player's ROM and prove the format round-trips.

What it does (no decomp needed; this is what the M5 load-time recompiler will see):
  1. parses every record with the grammar in meta/FORMAT.md (unknown record types are skipped and counted)
  2. checks the ROM SHA-1 against the `rom` record
  3. re-derives facts from the ROM and compares: every code range disassembles to whole documented
     instructions ending on its end address; jlist / jtarget / vec addresses match the ROM's words;
     ranges tile each bank with no overlap
  4. prints a summary per bank: routines, entries, bytes code / data / unknown

  python tools/meta_check.py [--meta meta/btdd.meta] [--rom PATH]
Exit 0 = consistent, 1 = mismatch, 2 = setup problem.
"""
import argparse
import collections
import hashlib
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
sys.path.insert(0, HERE)
from m6502 import decode  # noqa: E402

DEF_ROM = os.path.join(REPO, '..', 'Gamefiles', 'Battletoads-Double Dragon (USA).nes')
DEF_META = os.path.join(REPO, 'meta', 'btdd.meta')
SUPPORTED = 1
RANGE_TYPES = ('code', 'data', 'unknown')


def addr(tok):
    if not tok.startswith('$'):
        raise ValueError('address %r must be $hex' % tok)
    return int(tok[1:], 16)


def parse(path):
    """Returns (version, records). A record is (type, positional list, {key: value}, line number)."""
    recs = []
    version = None
    with open(path, encoding='utf-8') as f:
        for n, line in enumerate(f, 1):
            line = line.rstrip('\n')
            if not line.strip() or line.lstrip().startswith('#'):
                continue
            toks = line.split()
            if version is None:
                if toks[0] != 'btdd.meta' or len(toks) != 2:
                    raise SystemExit('%s:%d: first record must be "btdd.meta <version>"' % (path, n))
                version = int(toks[1])
                if version > SUPPORTED:
                    raise SystemExit('meta version %d is newer than this reader (%d)' % (version, SUPPORTED))
                continue
            pos, kv = [], {}
            for t in toks[1:]:
                if '=' in t:
                    k, v = t.split('=', 1)
                    kv[k] = v
                else:
                    pos.append(t)
            recs.append((toks[0], pos, kv, n))
    return version, recs


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--meta', default=DEF_META)
    ap.add_argument('--rom', default=DEF_ROM)
    a = ap.parse_args()
    if not os.path.isfile(a.rom):
        print('ROM not found: %s' % a.rom)
        return 2
    version, recs = parse(a.meta)
    raw = open(a.rom, 'rb').read()
    sha = hashlib.sha1(raw).hexdigest()
    bad = []
    rom_rec = [r for r in recs if r[0] == 'rom']
    if not rom_rec or rom_rec[0][2].get('sha1') != sha:
        print('ROM SHA-1 %s does not match the meta (%s)' % (sha, rom_rec[0][2].get('sha1') if rom_rec else '-'))
        return 1
    prg = raw[16:]
    mem = [prg[b * 0x8000:(b + 1) * 0x8000] for b in range(8)]

    def word(b, x):
        o = x - 0x8000
        return mem[b][o] | (mem[b][o + 1] << 8)

    types = collections.Counter(r[0] for r in recs)
    known = {'rom', 'source', 'conv', 'vec', 'code', 'data', 'unknown', 'xcode', 'entry', 'jlist', 'indirect',
             'jtable', 'jtarget', 'farcall', 'xycall', 'timing', 'xref', 'ram'}
    ranges = collections.defaultdict(list)
    per = collections.defaultdict(collections.Counter)
    insn_starts = collections.defaultdict(set)
    names = collections.defaultdict(dict)

    for t, pos, kv, n in recs:
        try:
            if t in RANGE_TYPES:
                b, s, e = int(pos[0]), addr(pos[1]), addr(pos[2])
                ranges[b].append((s, e, t, n))
                per[b][t] += e - s + 1
                if t in ('code', 'data'):
                    nm = pos[3]
                    if nm in names[b] and names[b][nm] != s:
                        bad.append('line %d: bank %d name %s repeated' % (n, b, nm))
                    names[b][nm] = s
                if t == 'code':
                    per[b]['routines'] += 1
                    per[b]['conf_' + kv.get('conf', '?')] += 1
                    x = s
                    while x <= e:
                        d = decode(mem[b], x)
                        if d is None:
                            bad.append('line %d: bank %d %s: undocumented opcode at $%04X' % (n, b, pos[3], x))
                            break
                        insn_starts[b].add(x)
                        x += d[2]
                    if x != e + 1 and 'notes' not in kv:
                        bad.append('line %d: bank %d %s does not end on an instruction boundary' % (n, b, pos[3]))
            elif t == 'entry':
                per[int(pos[0])]['entries'] += 1
            elif t == 'jlist':
                b, slot, tg = int(pos[0]), addr(pos[1]), addr(pos[2])
                if mem[b][slot - 0x8000] != 0x4C or word(b, slot + 1) != tg:
                    bad.append('line %d: jlist bank %d $%04X does not match the ROM' % (n, b, slot))
            elif t == 'jtarget':
                b, tab, k, tg = int(pos[0]), addr(pos[1]), int(pos[2]), addr(pos[3])
                if word(b, tab + 2 * k) != tg:
                    bad.append('line %d: jtarget bank %d $%04X[%d] does not match the ROM' % (n, b, tab, k))
            elif t == 'vec':
                b, which, tg = int(pos[0]), pos[1], addr(pos[2])
                at = {'nmi': 0xFFFA, 'reset': 0xFFFC, 'irq': 0xFFFE}[which]
                if word(b, at) != tg:
                    bad.append('line %d: vector %s bank %d does not match the ROM' % (n, which, b))
        except (ValueError, IndexError, KeyError) as ex:
            bad.append('line %d: malformed %s record (%s)' % (n, t, ex))

    for b in range(8):
        rs = sorted(ranges[b])
        pos = 0x8000
        for s, e, t, n in rs:
            if s < pos:
                bad.append('line %d: bank %d range $%04X overlaps the previous one' % (n, b, s))
            elif s > pos:
                bad.append('bank %d: $%04X-$%04X not covered by any range' % (b, pos, s - 1))
            pos = max(pos, e + 1)
        if pos != 0x10000:
            bad.append('bank %d: ranges end at $%04X' % (b, pos - 1))

    print('btdd.meta v%d: %d records (%s)' % (version, len(recs),
                                             ', '.join('%s %d' % kv for kv in sorted(types.items()))))
    skipped = sorted(set(types) - known)
    if skipped:
        print('skipped unknown record types: %s' % ', '.join(skipped))
    print('bank  routines  verified  entries    code    data  unknown')
    tot = collections.Counter()
    for b in range(8):
        p = per[b]
        print('  %d  %8d  %8d  %7d  %6d  %6d  %7d' % (b, p['routines'], p['conf_verified'], p['entries'],
                                                      p['code'], p['data'], p['unknown']))
        tot.update(p)
    T = 8 * 0x8000
    print('all %8d  %8d  %7d  %6d  %6d  %7d   (code %.1f%%, data %.1f%%, unknown %.1f%%)' % (
        tot['routines'], tot['conf_verified'], tot['entries'], tot['code'], tot['data'], tot['unknown'],
        100.0 * tot['code'] / T, 100.0 * tot['data'] / T, 100.0 * tot['unknown'] / T))
    for x in bad[:40]:
        print('MISMATCH: ' + x)
    print('meta_check: %s' % ('OK, meta and ROM agree' if not bad else '%d mismatch(es)' % len(bad)))
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
