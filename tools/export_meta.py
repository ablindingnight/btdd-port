"""export_meta.py: build meta/btdd.meta from the private decomp (read-only snapshot) + the player's ROM + CDL logs.

Inputs (none of them is copied into the output; the output holds addresses, names and classifications only):
  - ..\\btdd-decomp at a commit (default HEAD), read with `git show <commit>:<path>` so a running loop that is
    editing the working tree cannot race us: src/bank0..7.s, src/ram.inc, docs/NAME_PROPOSALS.md,
    docs/RAM_PROPOSALS.md, docs/SOUND_FACTS.md
  - the ROM (..\\Gamefiles\\Battletoads-Double Dragon (USA).nes), used to locate and validate instructions
  - Mesen/FCEUX code-data logs ..\\Gamefiles\\Debugger\\*.cdl (OR-merged), for "seen executed" provenance
  - meta/overrides.meta: hand-written records (our own findings) applied on top

Validation (exit 1 if any ERROR): source addresses agree with the decomp's range headers and the ROM opcodes;
every code range disassembles to whole instructions and ends in a terminator or falls into the next code range;
no overlapping ranges; names unique per bank. Prints coverage statistics (bytes code / data / unknown).

  python tools/export_meta.py [--decomp PATH] [--commit REV] [--rom PATH] [--out meta/btdd.meta] [--check-only]
Format: meta/FORMAT.md.
"""
import argparse
import collections
import glob
import hashlib
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
sys.path.insert(0, HERE)
from m6502 import decode, BRANCHES  # noqa: E402

FORMAT_VERSION = 1
EXPECTED_SHA1 = '0218a66c6ee9887ec9115213a2f5b64055d79599'
DEF_DECOMP = os.path.join(REPO, '..', 'btdd-decomp')
DEF_ROM = os.path.join(REPO, '..', 'Gamefiles', 'Battletoads-Double Dragon (USA).nes')
DEF_CDL = os.path.join(REPO, '..', 'Gamefiles', 'Debugger')
DEF_OUT = os.path.join(REPO, 'meta', 'btdd.meta')
OVERRIDES = os.path.join(REPO, 'meta', 'overrides.meta')

TRAMPOLINE = 0xFFCB      # ldx #lo / ldy #i / jsr $FFCB  ->  $80lo in bank byte[$FFB3+i]
BANK_TABLE = 0xFFB3
DISPATCH_PTR = 0x13
XYCALL = (0, 0x85CF)     # bank 0: ldx #lo / ldy #hi / jsr $85CF  ->  jump to $hilo
N_SLOTS = 15             # entity arrays: one byte per slot, 15 slots (slot 0/1 = players)
CDL_CODE, CDL_DATA = 1, 2

AUTO_NAME = re.compile(r'^(sub|loc|data|raw_b\d)_[0-9a-fA-F]{4}$')
HEADER = re.compile(r'^; \$([0-9A-F]{4})-\$([0-9A-F]{4}): (\S+) \((.*)\)\s*$')
LABEL = re.compile(r'^([A-Za-z_][A-Za-z0-9_]*):(.*)$')
LOCAL = re.compile(r'^(@[A-Za-z0-9_]+):(.*)$')
EQU = re.compile(r'^\s*@?[A-Za-z_][A-Za-z0-9_]*\s*:?=\s*')
INCBIN = re.compile(r'^\.incbin\s+"[^"]*",\s*\$([0-9A-Fa-f]+),\s*\$([0-9A-Fa-f]+)')
INSN = re.compile(r'^([a-z]{3})\b\s*(.*)$')

errors, warnings = [], []


def err(msg):
    errors.append(msg)


def warn(msg):
    warnings.append(msg)


def h4(a):
    return '$%04X' % a


# ---------------------------------------------------------------- inputs
def git_show(decomp, commit, path):
    r = subprocess.run(['git', '-C', decomp, 'show', '%s:%s' % (commit, path)], capture_output=True)
    if r.returncode:
        raise SystemExit('git show %s:%s failed: %s' % (commit, path, r.stderr.decode(errors='replace')))
    return r.stdout.decode('utf-8', errors='replace').replace('\r\n', '\n')


def load_rom(path):
    raw = open(path, 'rb').read()
    sha = hashlib.sha1(raw).hexdigest()
    if sha != EXPECTED_SHA1:
        raise SystemExit('ROM SHA-1 %s is not the USA release %s' % (sha, EXPECTED_SHA1))
    prg = raw[16:16 + 8 * 0x8000]
    return sha, [prg[b * 0x8000:(b + 1) * 0x8000] for b in range(8)]


def load_cdl(folder):
    # FCEUX logs: bit 0 code, bit 1 data, bits 2-3 = $8000 quarter, bit 4 indirect code, bit 5 indirect data
    fceux = bytes((CDL_CODE if v & 0x11 else 0) | (CDL_DATA if v & 0x22 else 0) for v in range(256))
    mesen = bytes(v & (CDL_CODE | CDL_DATA) for v in range(256))
    acc = 0
    used = 0
    for path in sorted(glob.glob(os.path.join(folder, '*.cdl'))):
        raw = open(path, 'rb').read()
        if len(raw) < 0x40000:
            continue
        if len(raw) == 0x40000 and not raw.startswith(b'CDLv'):
            body = raw.translate(fceux)
        else:
            body = raw[len(raw) - 0x40000:].translate(mesen)
        acc |= int.from_bytes(body, 'big')
        used += 1
    merged = acc.to_bytes(0x40000, 'big')
    return [merged[b * 0x8000:(b + 1) * 0x8000] for b in range(8)], used


# ---------------------------------------------------------------- bank source walker
class Seg:
    def __init__(self, kind, start, name, end=None, how=''):
        self.kind = kind          # code / data / unknown
        self.start = start
        self.end = end            # inclusive; None until known
        self.explicit = end is not None
        self.name = name
        self.how = how            # carve / datacarve / incbin / label / port
        self.items = []           # (addr, size, 'i' | 'b')
        self.labels = []          # (addr, name) non-local labels inside (alternate entries)
        self.dkind = None
        self.notes = []


def count_items(arg):
    return len([x for x in arg.split(',') if x.strip()])


def walk_bank(b, text, mem):
    segs, cur, pend = [], None, None
    addr = 0x8000
    for ln, raw in enumerate(text.split('\n'), 1):
        m = HEADER.match(raw)
        if m:
            s, e, name, how = int(m.group(1), 16), int(m.group(2), 16), m.group(3), m.group(4)
            if s != addr:
                err('bank %d line %d: header %s says $%04X but the source is at $%04X' % (b, ln, name, s, addr))
            pend = (s, e, name, 'datacarve' if 'datacarve' in how else 'carve')
            continue
        line = raw.split(';', 1)[0].rstrip()
        if not line.strip():
            continue
        if EQU.match(line) and not LABEL.match(line.strip()):
            continue
        st = line.strip()
        if st.startswith('.segment') or st.startswith('.include'):
            continue
        m = INCBIN.match(st)
        if m:
            off, n = int(m.group(1), 16), int(m.group(2), 16)
            if off != addr - 0x8000:
                err('bank %d line %d: .incbin offset $%04X but the source is at $%04X' % (b, ln, off, addr))
            seg = Seg('unknown', addr, 'unk_%04x' % addr, addr + n - 1, 'incbin')
            segs.append(seg)
            cur = None
            addr += n
            continue
        m = LABEL.match(line) if not line[0].isspace() else None
        if m:
            name, rest = m.group(1), m.group(2).strip()
            if pend and pend[2] == name:
                kind = 'data' if pend[3] == 'datacarve' else 'code'
                cur = Seg(kind, addr, name, pend[1], pend[3])
                segs.append(cur)
                pend = None
            elif cur is not None and cur.explicit and addr <= cur.end:
                cur.labels.append((addr, name))
            else:
                cur = Seg(None, addr, name, None, 'label')
                segs.append(cur)
            line = rest
            if not line:
                continue
            st = line.strip()
        else:
            m = LOCAL.match(line) if not line[0].isspace() else None
            if m:
                st = m.group(2).strip()
                if not st:
                    continue
        if pend:
            err('bank %d line %d: header %s not followed by its label' % (b, ln, pend[2]))
            pend = None
        if cur is None or (cur.explicit and addr > cur.end):
            cur = Seg(None, addr, 'orphan_%04x' % addr, None, 'orphan')
            segs.append(cur)
            warn('bank %d $%04X: source with no label/header (line %d)' % (b, addr, ln))
        if st.startswith('.byte'):
            n = count_items(st[5:])
            cur.items.append((addr, n, 'b'))
            addr += n
        elif st.startswith('.addr') or st.startswith('.word'):
            n = 2 * count_items(st[5:])
            if addr == 0xFFFA and cur.name != 'vectors':
                cur = Seg('data', addr, 'vectors', 0xFFFF, 'vectors')
                cur.dkind = 'vectors'
                segs.append(cur)
            cur.items.append((addr, n, 'b'))
            addr += n
        else:
            m = INSN.match(st)
            if not m:
                err('bank %d line %d: cannot parse %r' % (b, ln, st))
                continue
            d = decode(mem, addr)
            if d is None or d[0] != m.group(1):
                err('bank %d line %d $%04X: source says %s, ROM opcode is %s' %
                    (b, ln, addr, m.group(1), d[0] if d else 'undocumented'))
                break
            cur.items.append((addr, d[2], 'i'))
            addr += d[2]
    if addr != 0x10000:
        err('bank %d: source covers $8000-$%04X, not the whole bank' % (b, addr - 1))
    # close implicit ends, infer kinds
    for i, s in enumerate(segs):
        nxt = segs[i + 1].start if i + 1 < len(segs) else 0x10000
        if s.end is None:
            s.end = nxt - 1
        if s.kind is None:
            ins = sum(1 for it in s.items if it[2] == 'i')
            s.kind = 'code' if ins else 'data'
        if s.kind == 'code' and any(it[2] == 'b' for it in s.items):
            s.notes.append('embedded_data')
    return segs


# ---------------------------------------------------------------- analysis helpers
FLAG_SET = {'clc': ('bcc', True), 'sec': ('bcs', True), 'clv': ('bvc', True)}
PAIRS = {('beq', 'bne'), ('bne', 'beq'), ('bcc', 'bcs'), ('bcs', 'bcc'), ('bpl', 'bmi'), ('bmi', 'bpl'),
         ('bvc', 'bvs'), ('bvs', 'bvc')}


def branch_always(prev, last):
    """prev, last: decoded (m, mode, n, v). True if `last` is a branch that is always taken."""
    pm, pmode, _, pv = prev
    lm = last[0]
    if (pm, lm) in PAIRS:
        return True
    if pm in FLAG_SET and FLAG_SET[pm][0] == lm:
        return True
    if pm in ('lda', 'ldx', 'ldy') and pmode == 'imm':
        if lm == 'beq':
            return pv == 0
        if lm == 'bne':
            return pv != 0
        if lm == 'bpl':
            return pv < 0x80
        if lm == 'bmi':
            return pv >= 0x80
    return False


def word(mem, a):
    o = a - 0x8000
    if 0 <= o < len(mem) - 1:
        return mem[o] | (mem[o + 1] << 8)
    return None


class Bank:
    def __init__(self, b, segs, mem, cdl):
        self.b, self.segs, self.mem, self.cdl = b, segs, mem, cdl
        self.starts = {s.start: s for s in segs}
        self.insn = {}        # addr -> (decoded, seg)
        for s in segs:
            if s.kind != 'code':
                continue
            # Prefer a straight disassembly of the ROM over the source: the decomp writes an instruction as
            # .byte when its bytes are also read as data (e.g. a trailing `jmp` used as a 3-byte table).
            # Only when the straight run fails (real embedded data) do we use the source's instruction list.
            lin, a = [], s.start
            while a <= s.end:
                d = decode(mem, a)
                if d is None:
                    break
                lin.append((a, d))
                a += d[2]
            if a == s.end + 1:
                s.code = lin
                if 'embedded_data' in s.notes:
                    s.notes.remove('embedded_data')
                    s.notes.append('code_read_as_data')
            else:
                s.code = [(x, decode(mem, x)) for x, n, t in s.items if t == 'i']
                s.linear_fail = a
            for x, d in s.code:
                self.insn[x] = (d, s)

    def seg_at(self, a):
        lo, hi = 0, len(self.segs) - 1
        while lo <= hi:
            mid = (lo + hi) // 2
            s = self.segs[mid]
            if a < s.start:
                hi = mid - 1
            elif a > s.end:
                lo = mid + 1
            else:
                return s
        return None

    def code_seq(self, s):
        return s.code


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--decomp', default=DEF_DECOMP)
    ap.add_argument('--commit', default='HEAD')
    ap.add_argument('--rom', default=DEF_ROM)
    ap.add_argument('--cdl', default=DEF_CDL)
    ap.add_argument('--out', default=DEF_OUT)
    ap.add_argument('--check-only', action='store_true', help='validate and print stats, do not write')
    ap.add_argument('-v', '--verbose', action='store_true', help='print every warning')
    a = ap.parse_args()

    commit = subprocess.run(['git', '-C', a.decomp, 'rev-parse', a.commit], capture_output=True,
                            text=True).stdout.strip()
    if not commit:
        raise SystemExit('cannot resolve %s in %s' % (a.commit, a.decomp))
    sha, mems = load_rom(a.rom)
    cdls, ncdl = load_cdl(a.cdl)

    banks = []
    for b in range(8):
        segs = walk_bank(b, git_show(a.decomp, commit, 'src/bank%d.s' % b), mems[b])
        banks.append(Bank(b, segs, mems[b], cdls[b]))
    if errors:
        for e in errors:
            print('ERROR: ' + e)
        return 1

    recs = []      # (bank, addr, order, text)
    overrides = load_overrides(banks)
    names_proposed = load_name_proposals(git_show(a.decomp, commit, 'docs/NAME_PROPOSALS.md'))
    music = load_music_names(git_show(a.decomp, commit, 'docs/SOUND_FACTS.md'))

    stats = collections.Counter()
    for bk in banks:
        bk.jlist = read_jlist(bk.mem)
    for bk in banks:
        analyse_bank(bk, banks, recs, names_proposed, music, stats)
    recs.extend(overrides)
    ram_recs = build_ram(git_show(a.decomp, commit, 'src/ram.inc'),
                         git_show(a.decomp, commit, 'docs/RAM_PROPOSALS.md'))

    # global checks: overlaps, unique names
    for bk in banks:
        prev = None
        for s in bk.segs:
            if prev and s.start <= prev.end:
                err('bank %d: %s %s-%s overlaps %s' % (bk.b, s.name, h4(s.start), h4(s.end), prev.name))
            prev = s
        seen = {}
        for s in bk.segs:
            for nm, ad in [(s.name, s.start)] + [(n, x) for x, n in s.labels]:
                if nm in seen and seen[nm] != ad:
                    err('bank %d: name %s used at %s and %s' % (bk.b, nm, h4(seen[nm]), h4(ad)))
                seen[nm] = ad

    print_stats(banks, stats, ncdl)
    nw = collections.Counter(w.split(':', 1)[0] for w in warnings)
    if a.verbose:
        for w in warnings:
            print('warning: ' + w)
    for e in errors:
        print('ERROR: ' + e)
    if errors:
        return 1
    if a.check_only:
        return 0

    out = [
        'btdd.meta %d' % FORMAT_VERSION,
        'rom sha1=%s prg=%d mapper=7 banks=8' % (sha, 8 * 0x8000),
        'source decomp=%s exporter=tools/export_meta.py cdl_logs=%d' % (commit[:12], ncdl),
        'conv farcall stub=%s x=slot_lo y=bank_index bank_table=%s ptr=%s slot=$8000+x' % (
            h4(TRAMPOLINE), h4(BANK_TABLE), '$%02X' % DISPATCH_PTR),
        'conv xycall bank=0 stub=%s x=lo y=hi' % h4(XYCALL[1]),
        'conv entity_slots count=%d slot0=player1 slot1=player2' % N_SLOTS,
    ]
    ORDER = {'vec': 0, 'code': 1, 'data': 1, 'unknown': 1, 'entry': 2, 'jlist': 3, 'jtable': 3, 'jtarget': 4,
             'indirect': 5, 'xcode': 2, 'farcall': 6, 'xycall': 6, 'timing': 7, 'xref': 8}
    recs.sort(key=lambda r: (r[0], r[1], ORDER.get(r[3].split(' ', 1)[0], 9), r[3]))
    out += [r[3] for r in recs]
    out += ram_recs
    os.makedirs(os.path.dirname(os.path.abspath(a.out)), exist_ok=True)
    with open(a.out, 'w', newline='\n') as f:
        f.write('\n'.join(out) + '\n')
    print('wrote %s: %d records' % (os.path.relpath(a.out, REPO), len(out)))
    return 0


# ---------------------------------------------------------------- per-bank analysis
def name_prov(name):
    return 'auto' if AUTO_NAME.match(name) else 'reviewed'


def analyse_bank(bk, banks, recs, proposed, music, stats):
    b, mem, cdl = bk.b, bk.mem, bk.cdl

    def add(addr, text):
        recs.append((b, addr, 0, text))

    # vectors
    for vec, at in (('nmi', 0xFFFA), ('reset', 0xFFFC), ('irq', 0xFFFE)):
        t = word(mem, at)
        s = bk.seg_at(t) if t is not None and t >= 0x8000 else None
        nm = s.name if s and s.start == t else next((n for x, n in (s.labels if s else []) if x == t), '-')
        add(0xFFFA, 'vec %d %s %s %s' % (b, vec, h4(t), nm))
        if s is None or s.kind != 'code' or (s.start != t and t not in bk.insn):
            warn('vector: bank %d %s %s is not the start of carved code' % (b, vec, h4(t)))

    # data kinds, filled by the passes below
    jtables = find_jump_tables(bk)
    for s in bk.segs:
        if s.kind == 'data' and s.dkind is None:
            s.dkind = data_kind(s, music, b)
    for t in jtables:
        s = bk.seg_at(t['table'])
        if s and s.kind == 'data':
            s.dkind = 'ptrtable'
    for a, (d, s) in bk.insn.items():
        if d[1] in ('abs', 'absx', 'absy') and d[3] >= 0x8000 and d[0] not in ('jmp', 'jsr'):
            t = bk.seg_at(d[3])
            if t is not None and t.kind == 'data' and t.dkind == 'unknown':
                t.dkind = 'table'

    # code range validation
    for i, s in enumerate(bk.segs):
        if s.kind != 'code':
            continue
        seq = bk.code_seq(s)
        end_ok = None
        # whole instructions: either the straight ROM disassembly lands on end+1 (s.code from it), or the
        # source's instruction/.byte items tile the range exactly (checked by walk_bank against the ROM)
        if hasattr(s, 'linear_fail'):
            pos = s.start
            for x, n, t in s.items:
                if x != pos:
                    err('bank %d %s: gap in items at %s' % (b, s.name, h4(pos)))
                    break
                pos += n
            if pos != s.end + 1:
                err('bank %d %s %s-%s: items run to %s (not whole instructions)' %
                    (b, s.name, h4(s.start), h4(s.end), h4(pos - 1)))
            stats['code_embedded'] += 1
        last_i = None
        if seq and seq[-1][0] + seq[-1][1][2] == s.end + 1:
            last_i = seq[-1][1]
        nxt = bk.segs[i + 1] if i + 1 < len(bk.segs) else None
        if last_i is not None:
            m = last_i[0]
            if m in ('rts', 'rti'):
                end_ok = m
            elif m == 'jmp':
                end_ok = 'jmp' if last_i[1] == 'abs' else 'jmpind'
            elif m in BRANCHES and len(seq) >= 2 and branch_always(seq[-2][1], last_i):
                end_ok = 'bra'
            elif m == 'brk':
                end_ok = 'brk'
        if end_ok is None and nxt is not None and nxt.start == s.end + 1 and nxt.kind == 'code':
            end_ok = 'fall'
        if end_ok is None:
            end_ok = 'open'
            if nxt is not None and nxt.start == s.end + 1:
                end_ok = 'open_into_' + nxt.kind
            stats['open'] += 1
            warn('openend: bank %d %s %s-%s ends without a terminator' % (b, s.name, h4(s.start), h4(s.end)))
        s.endkind = end_ok

    # cross references: jsr/jmp/branch targets that are not a range start
    entries = {}
    for a, (d, s) in sorted(bk.insn.items()):
        m, mode, n, v = d
        if not ((m in ('jsr', 'jmp') and mode == 'abs') or m in BRANCHES):
            continue
        if v < 0x8000:
            continue
        if m in BRANCHES and s.start <= v <= s.end:
            continue
        t = bk.seg_at(v)
        if t is None:
            continue
        if t.start == v and t.kind == 'code':
            continue
        if t.kind == 'code' and v in bk.insn:
            entries.setdefault(v, set()).add('%s@%s' % (m, h4(a)))
        else:
            kind = t.kind if t.kind != 'code' else 'midinsn'
            stats['xref_' + kind] += 1
            add(v, 'xref %d %s %s from=%s in=%s' % (b, h4(v), kind, h4(a), t.name))
    for s in bk.segs:
        for x, n in s.labels:
            if s.kind == 'code' and x in bk.insn:
                entries.setdefault(x, set()).add('label')
    label_at = {x: n for s in bk.segs for x, n in s.labels}

    # dispatch list (jmp ($13) at $8000 + jmp table)
    jlist = bk.jlist
    if mem[0] == 0x6C:
        ptr = word(mem, 0x8001)
        for a, t in sorted(jlist.items()):
            ts = bk.seg_at(t)
            add(a, 'jlist %d %s %s %s' % (b, h4(a), h4(t), target_name(bk, t, label_at)))
            if not (ts and ts.kind == 'code' and (ts.start == t or t in bk.insn)):
                warn('jlist: bank %d %s -> %s is not carved code' % (b, h4(a), h4(t)))
        add(0x8000, 'indirect %d %s ptr=$%02X kind=dispatch targets=jlist' % (b, h4(0x8000), ptr))

    # jump tables
    for t in jtables:
        add(t['table'], 'jtable %d %s count=%d ptr=%s site=%s jmp=%s conf=%s' % (
            b, h4(t['table']), len(t['targets']), h4(t['ptr']) if t['ptr'] > 0xFF else '$%02X' % t['ptr'],
            h4(t['site']), h4(t['jmp']), t['conf']))
        for k, tg in enumerate(t['targets']):
            add(t['table'] + 2 * k, 'jtarget %d %s %d %s %s' % (b, h4(t['table']), k, h4(tg),
                                                                target_name(bk, tg, label_at)))
            if tg in bk.insn and bk.insn[tg][1].start != tg:
                entries.setdefault(tg, set()).add('jtable')
    # other jmp (ptr) sites
    by_ptr = collections.defaultdict(list)
    for t in jtables:
        by_ptr[t['ptr']].append(t['table'])
    for a, (d, s) in sorted(bk.insn.items()):
        if d[0] == 'jmp' and d[1] == 'ind' and a != 0x8000:
            p = d[3]
            tabs = by_ptr.get(p)
            tg = ('tables=' + ','.join(h4(x) for x in sorted(tabs))) if tabs else 'targets=unknown'
            if not tabs:
                stats['indirect_unknown'] += 1
            add(a, 'indirect %d %s ptr=%s %s' % (b, h4(a), h4(p) if p > 0xFF else '$%02X' % p, tg))

    # far calls and xycalls
    for s in bk.segs:
        if s.kind != 'code':
            continue
        seq = bk.code_seq(s)
        for i, (a, d) in enumerate(seq):
            if d[0] not in ('jsr', 'jmp') or d[1] != 'abs':
                continue
            tail = ' tail' if d[0] == 'jmp' else ''
            imm = {}
            for k in range(max(0, i - 4), i):
                dd = seq[k][1]
                if dd[0] in ('ldx', 'ldy') and dd[1] == 'imm':
                    imm[dd[0][2]] = dd[3]
            if d[3] == TRAMPOLINE:
                if 'x' in imm and 'y' in imm:
                    sel = mem[BANK_TABLE - 0x8000 + imm['y']] & 7
                    slot = 0x8000 + imm['x']
                    tb = banks[sel]
                    tgt = tb.jlist.get(slot)
                    add(a, 'farcall %d %s bank=%d slot=%s target=%s %s%s' % (
                        b, h4(a), sel, h4(slot), h4(tgt) if tgt else '?',
                        target_name(tb, tgt, {}) if tgt else '-', tail))
                    if tgt is None:
                        warn('farcall: bank %d %s -> bank %d slot %s not in its jlist' % (b, h4(a), sel, h4(slot)))
                else:
                    known = ''.join('%s=$%02X ' % (r, imm[r]) for r in sorted(imm))
                    add(a, 'farcall %d %s bank=? slot=? target=? dynamic %s%s' % (b, h4(a), known, tail.strip()))
                    stats['farcall_dynamic'] += 1
            if (b, d[3]) == XYCALL:
                if 'x' in imm and 'y' in imm:
                    t = imm['y'] << 8 | imm['x']
                    add(a, 'xycall %d %s target=%s %s' % (b, h4(a), h4(t), target_name(bk, t, label_at)))
                    if t in bk.insn and bk.insn[t][1].start != t:
                        entries.setdefault(t, set()).add('xycall')
                else:
                    add(a, 'xycall %d %s target=? dynamic' % (b, h4(a)))

    for v, why in sorted(entries.items()):
        s = bk.insn[v][1]
        nm = label_at.get(v, 'e_%04x' % v)
        add(v, 'entry %d %s %s in=%s via=%s' % (b, h4(v), nm, s.name, ','.join(sorted(why))))
        stats['entries'] += 1

    # timing-sensitive sites
    timing_sites(bk, add, stats)

    # ranges
    for s in bk.segs:
        n = s.end - s.start + 1
        seen_code = sum(1 for x in range(s.start, s.end + 1) if cdl[x - 0x8000] & CDL_CODE)
        seen_data = sum(1 for x in range(s.start, s.end + 1) if cdl[x - 0x8000] & CDL_DATA)
        stats[s.kind] += n
        stats['%s_b%d' % (s.kind, b)] += n
        if s.kind == 'code':
            entry_seen = bool(cdl[s.start - 0x8000] & CDL_CODE)
            is_open = s.endkind.startswith('open')
            conf = 'suspect' if is_open else ('verified' if entry_seen else 'static')
            stats['code_' + conf] += 1
            stats['routines'] += 1
            np = name_prov(s.name)
            extra = ''
            if np == 'auto' and s.name in proposed.get(b, {}):
                extra = ' proposed=%s' % proposed[b][s.name]
            notes = (' notes=' + ','.join(s.notes)) if s.notes else ''
            add(s.start, 'code %d %s %s %s naming=%s end=%s src=%s cdl=%d/%d conf=%s%s%s' % (
                b, h4(s.start), h4(s.end), s.name, np, s.endkind, s.how, seen_code, n, conf, extra, notes))
        elif s.kind == 'data':
            np = name_prov(s.name)
            extra = ''
            if np == 'auto' and s.name in proposed.get(b, {}):
                extra = ' proposed=%s' % proposed[b][s.name]
            add(s.start, 'data %d %s %s %s kind=%s naming=%s src=%s cdl_read=%d/%d cdl_exec=%d%s' % (
                b, h4(s.start), h4(s.end), s.name, s.dkind, np, s.how, seen_data, n, seen_code, extra))
            if seen_code:
                stats['data_executed'] += 1
                warn('dataexec: bank %d %s %s-%s has %d byte(s) the CDL saw executed' %
                     (b, s.name, h4(s.start), h4(s.end), seen_code))
        else:
            add(s.start, 'unknown %d %s %s cdl_exec=%d cdl_read=%d' % (b, h4(s.start), h4(s.end), seen_code,
                                                                     seen_data))
        if s.kind != 'code' and seen_code:
            for xs, xe, ok in cdl_code_runs(bk, s):
                add(xs, 'xcode %d %s %s in=%s whole=%s' % (b, h4(xs), h4(xe), s.name, 'yes' if ok else 'no'))
                stats['xcode'] += 1
                stats['xcode_bytes'] += xe - xs + 1
            stats['unknown_exec'] += seen_code
            stats['unknown_read'] += seen_data


def read_jlist(mem):
    jl = {}
    if mem[0] == 0x6C:
        a = 0x8003
        while a < 0x8100 and mem[a - 0x8000] == 0x4C:
            t = word(mem, a + 1)
            if t is None or t < 0x8000:
                break
            jl[a] = t
            a += 3
    return jl


def cdl_code_runs(bk, s):
    """Runs of bytes the CDL saw executed inside a range the decomp did not carve as code. Each run is
    disassembled from its first executed byte; whole=yes when it decodes to whole documented instructions."""
    out, a = [], s.start
    cdl, mem = bk.cdl, bk.mem
    while a <= s.end:
        if not cdl[a - 0x8000] & CDL_CODE:
            a += 1
            continue
        st, ok = a, True
        while a <= s.end and cdl[a - 0x8000] & CDL_CODE:
            d = decode(mem, a)
            if d is None:
                ok = False
                a += 1
                continue
            a += d[2]
        if a > s.end + 1:
            ok = False
        out.append((st, min(a, s.end + 1) - 1, ok))
    return out


def target_name(bk, t, label_at):
    if t is None:
        return '-'
    s = bk.starts.get(t)
    if s is not None:
        return s.name
    if t in label_at:
        return label_at[t]
    return 'e_%04x' % t if t in bk.insn else '?'


def find_jump_tables(bk):
    """lda T,x|y / sta P / lda T+1,x|y / sta P+1 (either order) with a jmp (P) in this bank."""
    mem = bk.mem
    jmp_ptrs = {}
    for a, (d, s) in bk.insn.items():
        if d[0] == 'jmp' and d[1] == 'ind':
            jmp_ptrs.setdefault(d[3], a)
    out = []
    for s in bk.segs:
        if s.kind != 'code':
            continue
        seq = bk.code_seq(s)
        for i in range(len(seq) - 3):
            (a0, d0), (_, d1), (_, d2), (_, d3) = seq[i:i + 4]
            if not (d0[0] == 'lda' and d0[1] in ('absx', 'absy') and d1[0] == 'sta' and
                    d2[0] == 'lda' and d2[1] == d0[1] and d3[0] == 'sta'):
                continue
            if d2[3] == d0[3] + 1 and d3[3] == d1[3] + 1:
                T, P = d0[3], d1[3]
            elif d2[3] == d0[3] - 1 and d3[3] == d1[3] - 1:
                T, P = d2[3], d3[3]
            else:
                continue
            if T < 0x8000:
                continue
            if P not in jmp_ptrs:
                ts = bk.seg_at(T)
                if ts is not None and ts.kind == 'data':
                    ts.dkind = 'ptrtable'
                continue
            ts = bk.seg_at(T)
            limit = ts.end if ts and ts.kind != 'code' else T + 63
            targets, misses = [], 0
            k = 0
            while T + 2 * k + 1 <= limit and k < 64:
                t = word(mem, T + 2 * k)
                if t is None or t < 0x8000 or T <= t <= T + 2 * k + 1:
                    break
                ok = t in bk.insn or (t in bk.starts and bk.starts[t].kind == 'code')
                if not ok:
                    misses += 1
                    if misses == 2 or not (ts and ts.kind != 'code'):
                        break
                else:
                    misses = 0
                targets.append(t)
                k += 1
            while targets and not (targets[-1] in bk.insn or targets[-1] in bk.starts):
                targets.pop()
            if targets:
                same = bk.insn[jmp_ptrs[P]][1] is s and 0 <= jmp_ptrs[P] - a0 <= 64
                out.append({'table': T, 'ptr': P, 'site': a0, 'jmp': jmp_ptrs[P], 'targets': targets,
                            'conf': 'strong' if same else 'weak'})
    uniq = {}
    for t in out:
        if t['table'] not in uniq or len(t['targets']) > len(uniq[t['table']]['targets']):
            uniq[t['table']] = t
    return sorted(uniq.values(), key=lambda t: t['table'])


def data_kind(s, music, b):
    n = s.name.lower()
    if (b, s.name) in music:
        return 'music'
    if n.endswith('_text') or '_text_' in n:
        return 'text'
    if 'tile' in n or 'chr' in n or 'gfx' in n:
        return 'graphics'
    if 'table' in n or 'defs' in n or 'metasprite' in n:
        return 'table'
    return 'unknown'


def timing_sites(bk, add, stats):
    b = bk.b
    for s in bk.segs:
        if s.kind != 'code':
            continue
        seq = bk.code_seq(s)
        for i, (a, d) in enumerate(seq):
            m, mode, n, v = d
            if m in ('sta', 'stx', 'sty') and mode == 'abs' and v == 0x4011:
                add(a, 'timing %d %s pcm_write reg=$4011' % (b, h4(a)))
                stats['t_pcm'] += 1
            elif m in ('sta', 'stx', 'sty') and v == 0x4014 and mode == 'abs':
                add(a, 'timing %d %s oam_dma reg=$4014 stall=513' % (b, h4(a)))
                stats['t_dma'] += 1
            elif m in ('sta', 'stx', 'sty', 'inc', 'dec') and mode.startswith('abs') and v >= 0x8000:
                add(a, 'timing %d %s bank_switch store=%s mode=%s' % (b, h4(a), h4(v), mode))
                stats['t_bank'] += 1
            if m == 'jmp' and mode == 'abs' and v == a:
                add(a, 'timing %d %s idle_loop' % (b, h4(a)))
                stats['t_idle'] += 1
            if m in BRANCHES and a - 6 <= v <= a and v in bk.insn:
                body, x = [], v
                while x <= a and x in bk.insn:
                    body.append(bk.insn[x][0][0])
                    x += bk.insn[x][0][2]
                if body and set(body[:-1]) <= {'dex', 'dey', 'nop', 'inx', 'iny'} and len(body) > 1:
                    add(v, 'timing %d %s delay_loop loop=%s' % (b, h4(v), h4(a)))
                    stats['t_delay'] += 1
            # polling loops: a read of $2002 (or a RAM byte) followed within 3 instructions by a branch back
            if m in ('lda', 'ldx', 'ldy', 'bit') and mode in ('abs', 'zp'):
                for k in range(i + 1, min(i + 4, len(seq))):
                    dk = seq[k][1]
                    if dk[0] in BRANCHES and dk[3] <= a and dk[3] >= seq[max(0, i - 2)][0]:
                        body = seq[i:k + 1]
                        writes = any(x[1][0] in ('sta', 'stx', 'sty', 'inc', 'dec', 'jsr') for x in body)
                        if v == 0x2002:
                            br = dk[0]
                            if br in ('bvc', 'bvs'):
                                bit = 'sprite0'
                            elif br in ('bpl', 'bmi') and not any(x[1][0] == 'and' for x in body):
                                bit = 'vblank'
                            else:
                                mask = None
                                for x in body:
                                    if x[1][0] == 'and' and x[1][1] == 'imm':
                                        mask = x[1][3]
                                if m == 'bit':   # Z = A & PPUSTATUS: look back for the lda #imm that set A
                                    for k2 in range(i - 1, max(-1, i - 4), -1):
                                        x = seq[k2][1]
                                        if x[0] == 'lda' and x[1] == 'imm':
                                            mask = x[3]
                                            break
                                bit = {0x40: 'sprite0', 0x80: 'vblank', 0x20: 'overflow'}.get(mask, 'mask_unknown')
                            add(a, 'timing %d %s ppustatus_poll bit=%s loop=%s' % (b, h4(a), bit, h4(seq[k][0])))
                            stats['t_poll'] += 1
                        elif v < 0x0800 and not writes and dk[3] == a:
                            add(a, 'timing %d %s ram_spin addr=%s loop=%s' % (
                                b, h4(a), h4(v) if v > 0xFF else '$%02X' % v, h4(seq[k][0])))
                            stats['t_spin'] += 1
                        break


# ---------------------------------------------------------------- names / RAM / overrides
def load_name_proposals(text):
    out = collections.defaultdict(dict)
    for line in text.split('\n'):
        m = re.match(r'^bank (\d) \$([0-9A-F]{4}) (\S+) -> ([A-Za-z_][A-Za-z0-9_]*)\b', line)
        if m:
            out[int(m.group(1))][m.group(3)] = m.group(4)
    return out


def load_music_names(text):
    out = set()
    sect = text.split('## Music data per stage', 1)
    if len(sect) == 2:
        body = sect[1].split('\n## ', 1)[0]
        for m in re.finditer(r'\b(data_[0-9a-f]{4})\b', body):
            out.add((3, m.group(1)))
    return out


HW = {'PPU', 'OAM', 'APU', 'JOY'}


def build_ram(inc, props):
    recs, names = {}, {}
    for line in inc.split('\n'):
        m = re.match(r'^([A-Za-z_][A-Za-z0-9_]*)\s*=\s*\$([0-9A-Fa-f]+)', line)
        if not m:
            continue
        nm, ad = m.group(1), int(m.group(2), 16)
        kind, size, extra = 'var', 1, ''
        if ad >= 0x2000:
            kind = 'io'
        elif nm.startswith('ent_'):
            kind, size, extra = 'array', N_SLOTS, ' stride=1 index=slot'
        elif nm.startswith('ptr_') and nm.endswith('_hi'):
            kind, extra = 'alias', ' of=%s part=hi' % nm[:-3]
        elif nm.startswith('ptr_'):
            kind, size = 'ptr', 2
        elif nm in ('player_lives', 'joy_held', 'joy_pressed'):
            kind, size, extra = 'array', 2, ' stride=1 index=player'
        recs[ad] = (nm, kind, size, extra, 'reviewed')
        names[nm] = ad
    for line in props.split('\n'):
        m = re.match(r'^\$([0-9A-F]{4}) -> ([A-Za-z_][A-Za-z0-9_]*)', line)
        if not m:
            continue
        ad, nm = int(m.group(1), 16), m.group(2)
        if ad in recs and recs[ad][4] == 'reviewed':
            continue
        kind, size, extra = 'var', 1, ''
        if 'ARRAY' in line or nm.startswith('ent_'):
            kind, size, extra = 'array', N_SLOTS, ' stride=1 index=slot'
        elif 'POINTER:' in line:
            kind, size = 'ptr', 2
        recs[ad] = (nm, kind, size, extra, 'proposed')
    for ad in sorted(recs):
        if recs[ad][1] == 'ptr' and ad + 1 in recs and recs[ad + 1][4] == 'proposed':
            recs[ad + 1] = (recs[ad + 1][0], 'alias', 1, ' of=%s part=hi' % recs[ad][0], 'proposed')
    out = []
    for ad in sorted(recs):
        nm, kind, size, extra, prov = recs[ad]
        a = '$%04X' % ad
        out.append('ram %s %d %s kind=%s naming=%s%s' % (a, size, nm, kind, prov, extra))
    # uniqueness of reviewed RAM names is checked here (proposed names may repeat; they are hints)
    seen = {}
    for ad in sorted(recs):
        nm, prov = recs[ad][0], recs[ad][4]
        if prov == 'reviewed':
            if nm in seen:
                err('ram: name %s at $%04X and $%04X' % (nm, seen[nm], ad))
            seen[nm] = ad
    return out


def load_overrides(banks):
    """meta/overrides.meta: records in the btdd.meta syntax written by hand (our own findings). Each is emitted
    as-is with prov=port. Range records here also replace the decomp's classification of those bytes."""
    out = []
    if not os.path.isfile(OVERRIDES):
        return out
    for line in open(OVERRIDES, encoding='utf-8'):
        line = line.split('#', 1)[0].strip()
        if not line:
            continue
        f = line.split()
        b = int(f[1])
        ad = int(f[2][1:], 16)
        out.append((b, ad, 0, line + ('' if 'prov=' in line else ' prov=port')))
    return out


def print_stats(banks, stats, ncdl):
    print('btdd.meta export: %d CDL log(s) merged' % ncdl)
    print('bank   code    data  unknown  routines')
    for bk in banks:
        print('  %d  %6d  %6d  %6d  %5d' % (bk.b, stats['code_b%d' % bk.b], stats['data_b%d' % bk.b],
                                           stats['unknown_b%d' % bk.b],
                                           sum(1 for s in bk.segs if s.kind == 'code')))
    tot = 8 * 0x8000
    print('total  code %d (%.1f%%)  data %d (%.1f%%)  unknown %d (%.1f%%)' % (
        stats['code'], 100.0 * stats['code'] / tot, stats['data'], 100.0 * stats['data'] / tot,
        stats['unknown'], 100.0 * stats['unknown'] / tot))
    print('routines %d: verified %d, static %d, suspect %d (open end %d); mid-routine entries %d' % (
        stats['routines'], stats['code_verified'], stats['code_static'], stats['code_suspect'], stats['open'],
        stats['entries']))
    print('unknown bytes the CDL saw executed: %d, read: %d; data ranges with executed bytes: %d' % (
        stats['unknown_exec'], stats['unknown_read'], stats['data_executed']))
    print('xrefs into unknown %d / data %d / mid-instruction %d; jmp (ptr) with unknown targets %d; '
          'dynamic farcalls %d' % (stats['xref_unknown'], stats['xref_data'], stats['xref_midinsn'],
                                   stats['indirect_unknown'], stats['farcall_dynamic']))
    print('CDL-executed runs outside carved code (xcode): %d runs, %d bytes; code ranges kept from source items '
          '(real embedded data): %d' % (stats['xcode'], stats['xcode_bytes'], stats['code_embedded']))
    print('timing sites: pcm %d, oam_dma %d, bank_switch %d, ppustatus_poll %d, ram_spin %d, idle_loop %d, '
          'delay_loop %d' % (stats['t_pcm'], stats['t_dma'], stats['t_bank'], stats['t_poll'], stats['t_spin'],
                             stats['t_idle'], stats['t_delay']))
    print('warnings: %d' % len(warnings))


if __name__ == '__main__':
    sys.exit(main())
