"""m6_pick.py: choose routines for the M6 decompile pipeline.

Leaf routines first: carved code with conf=verified that ends in RTS and contains no JSR, no jump out of its
range and no indirect jump. Ranked by how often the 2P TAS calls them (decomp docs/STAGE_MAP.tsv, read through
`git show`, never the working tree). Prints "bank addr name size calls flags" lines; --list writes the
"bank addr" file m6_capture reads.

  python tools/m6/m6_pick.py [--max-size 96] [--top 40] [--list out.txt]
"""
import argparse
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, os.path.join(REPO, 'tools'))
from m6502 import decode, BRANCHES  # noqa: E402
from meta_check import parse  # noqa: E402

ROM = os.path.join(REPO, '..', 'Gamefiles', 'Battletoads-Double Dragon (USA).nes')
DECOMP = os.path.join(REPO, '..', 'btdd-decomp')


def routines(meta=os.path.join(REPO, 'meta', 'btdd.meta')):
    _, recs = parse(meta)
    out = []
    for t, pos, kv, n in recs:
        if t == 'code':
            out.append({'bank': int(pos[0]), 'start': int(pos[1][1:], 16), 'end': int(pos[2][1:], 16),
                        'name': pos[3], 'conf': kv.get('conf'), 'endkind': kv.get('end'),
                        'proposed': kv.get('proposed')})
    return out


def call_counts():
    try:
        txt = subprocess.run(['git', '-C', DECOMP, 'show', 'HEAD:docs/STAGE_MAP.tsv'], capture_output=True,
                             text=True, check=True).stdout
    except (subprocess.CalledProcessError, FileNotFoundError):
        return {}
    out = {}
    for line in txt.splitlines()[1:]:
        f = line.split('\t')
        if len(f) >= 4 and f[3].isdigit():
            out[(int(f[0]), int(f[1].lstrip('$'), 16))] = int(f[3])
    return out


def analyse(mem, r):
    """-> (is_leaf, flags, instructions)"""
    a, ins, flags = r['start'], [], set()
    while a <= r['end']:
        d = decode(mem, a)
        if d is None:
            return False, {'undecodable'}, ins
        ins.append((a, d))
        m, mode, n, v = d
        if m in ('jsr', 'brk', 'rti') or (m == 'jmp' and mode == 'ind'):
            return False, {m}, ins
        if (m == 'jmp' or m in BRANCHES) and not (r['start'] <= v <= r['end']):
            return False, {'jumps out'}, ins
        if mode in ('indx', 'indy'):
            flags.add('ptr')
        elif mode.startswith('abs') and m != 'jmp':
            if 0x2000 <= v < 0x8000:
                flags.add('hw')
            elif v >= 0x8000:
                flags.add('rom')
        a += n
    return ins[-1][1][0] == 'rts', flags, ins


def callees_of(mem, r, starts):
    """-> (ok, [callee start addresses]) for a routine whose only way out besides RTS is JSR to routine starts of
    the same bank (no far calls, indirect jumps, jumps out of its range)."""
    a, out = r['start'], []
    last = None
    while a <= r['end']:
        d = decode(mem, a)
        if d is None:
            return False, []
        m, mode, n, v = d
        if m in ('brk', 'rti') or (m == 'jmp' and mode == 'ind'):
            return False, []
        if m == 'jsr':
            if v not in starts:
                return False, []
            if v not in out:
                out.append(v)
        elif (m == 'jmp' or m in BRANCHES) and not (r['start'] <= v <= r['end']):
            return False, []
        last = m
        a += n
    return last == 'rts', out


FARCALL = 0xFFCB  # conv farcall stub (ldx #slot / ldy #bank_index / jsr $FFCB)
TRAMPOLINES = {0x8000, 0xFFCB}  # the jlist dispatch and the far-call stub: runtime, never M6 targets
KIND_ORDER = ('leaf', 'calls', 'tail', 'alt', 'far', 'dispatch')


def entries(meta=os.path.join(REPO, 'meta', 'btdd.meta')):
    """(bank, addr) -> entry name, for addresses inside a routine that are entered from elsewhere."""
    _, recs = parse(meta)
    return {(int(pos[0]), int(pos[1][1:], 16)): pos[2] for t, pos, kv, n in recs if t == 'entry'}


_CACHE = {}


def _bank_index():
    if 'idx' not in _CACHE:
        rs = routines()
        ent = entries()
        idx = {}
        for r in rs:
            idx.setdefault(r['bank'], []).append(r)
        _CACHE['idx'] = (rs, ent, idx)
    return _CACHE['idx']


def target_name(bank, addr):
    """Routine name at a routine start, the meta entry name inside a routine, else entry_XXXX."""
    rs, ent, idx = _bank_index()
    for r in idx.get(bank, []):
        if r['start'] == addr:
            return r['name']
    return ent.get((bank, addr), 'entry_%04x' % addr)


def _inside(bank, addr):
    rs, ent, idx = _bank_index()
    return any(r['start'] <= addr <= r['end'] for r in idx.get(bank, []))


def classify(mem, r):
    """Every way out of a routine besides its own RTS.
    -> (ok, kind, targets, why). targets: [(name, addr, how)] with how in
       call  (jsr to a routine start of the same bank)
       alt   (jsr/jmp/branch to an address inside another routine: a meta `entry`)
       tail  (jmp/branch out of the range to another routine's start: a call followed by return)
       far   (jsr $FFCB: the far-call trampoline; far_call(m, bank_index, slot) in C)
       dispatch (jmp (ptr): jump_to(m, target) in C)
    kind: the hardest feature it needs, in KIND_ORDER. ok False (why says) for BRK/RTI, TXS, code in RAM,
    undecodable bytes, a last instruction that is not RTS/JMP, or a trampoline."""
    b = r['bank']
    if r['start'] in TRAMPOLINES:
        return False, None, [], 'trampoline'
    a, out, last, rank = r['start'], [], None, 0

    def add(addr, how):
        nm = target_name(b, addr)
        if (nm, addr, how) not in out:
            out.append((nm, addr, how))

    while a <= r['end']:
        d = decode(mem, a)
        if d is None:
            return False, None, [], 'undecodable'
        m, mode, n, v = d
        if m in ('brk', 'rti', 'txs'):
            return False, None, [], m
        if m == 'jmp' and mode == 'ind':
            out.append(('jump_to', v, 'dispatch'))
            rank = max(rank, 5)
        elif m == 'jsr':
            if v == FARCALL:
                if ('far_call', v, 'far') not in out:
                    out.append(('far_call', v, 'far'))
                rank = max(rank, 4)
            elif v < 0x8000:
                return False, None, [], 'code in RAM'
            elif any(x['start'] == v for x in _bank_index()[2].get(b, [])):
                add(v, 'call')
                rank = max(rank, 1)
            elif _inside(b, v):
                add(v, 'alt')
                rank = max(rank, 3)
            else:
                return False, None, [], 'jsr outside known code'
        elif (m == 'jmp' or m in BRANCHES) and not (r['start'] <= v <= r['end']):
            if v < 0x8000:
                return False, None, [], 'code in RAM'
            if any(x['start'] == v for x in _bank_index()[2].get(b, [])):
                add(v, 'tail')
                rank = max(rank, 2)
            elif _inside(b, v):
                add(v, 'alt')
                rank = max(rank, 3)
            else:
                return False, None, [], 'jump outside known code'
        last = (m, mode)
        a += n
    if last is None:
        return False, None, [], 'empty'
    if last[0] not in ('rts', 'jmp'):  # runs on into the next code: a tail jump to the address after the range
        nxt = r['end'] + 1
        if any(x['start'] == nxt for x in _bank_index()[2].get(b, [])):
            add(nxt, 'tail')
            rank = max(rank, 2)
        elif _inside(b, nxt):
            add(nxt, 'alt')
            rank = max(rank, 3)
        else:
            return False, None, [], 'falls through its end'
    return True, KIND_ORDER[rank], out, ''


def callgraph_ready(passing, max_size=128, stubs=False):
    """Non-leaf routines (conf=verified) all of whose callees have passing C, ranked by call count.
    passing: set of routine names with passing C. -> [(calls, routine, [callee names])]
    stubs=True: every non-leaf routine with same-bank callees qualifies (callees without passing C run as the
    original code through the interpreter stub); ranked by the share of callees with passing C, then calls."""
    raw = open(ROM, 'rb').read()[16:]
    mems = [raw[b * 0x8000:(b + 1) * 0x8000] for b in range(8)]
    rs = routines()
    by_start = {(r['bank'], r['start']): r for r in rs}
    calls = call_counts()
    out = []
    for r in rs:
        size = r['end'] - r['start'] + 1
        if r['conf'] != 'verified' or size > max_size or r['name'] in passing:
            continue
        starts = set(a for (b, a) in by_start if b == r['bank'])
        ok, cs = callees_of(mems[r['bank']], r, starts)
        if not ok or not cs:
            continue
        names = [by_start[(r['bank'], c)]['name'] for c in cs]
        share = sum(1 for n in names if n in passing) / len(names)
        if stubs or share == 1:
            out.append((calls.get((r['bank'], r['start']), 0), r, names, share))
    out.sort(key=lambda x: (-x[3], -x[0]))
    return [x[:3] for x in out]


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--max-size', type=int, default=96)
    ap.add_argument('--min-size', type=int, default=6)
    ap.add_argument('--top', type=int, default=40)
    ap.add_argument('--list')
    a = ap.parse_args()
    raw = open(ROM, 'rb').read()[16:]
    mems = [raw[b * 0x8000:(b + 1) * 0x8000] for b in range(8)]
    calls = call_counts()
    rows = []
    for r in routines():
        size = r['end'] - r['start'] + 1
        if r['conf'] != 'verified' or not (a.min_size <= size <= a.max_size):
            continue
        leaf, flags, ins = analyse(mems[r['bank']], r)
        if not leaf:
            continue
        rows.append((calls.get((r['bank'], r['start']), 0), r, size, flags, len(ins)))
    rows.sort(key=lambda x: -x[0])
    rows = rows[:a.top]
    for c, r, size, flags, n in rows:
        print('%d %04x %-28s %4d bytes %3d insns %8d calls  %s' % (r['bank'], r['start'], r['name'], size, n, c,
                                                                  ','.join(sorted(flags)) or 'ram-only'))
    if a.list:
        with open(a.list, 'w') as f:
            for c, r, size, flags, n in rows:
                f.write('%d %04x\n' % (r['bank'], r['start']))


if __name__ == '__main__':
    main()
