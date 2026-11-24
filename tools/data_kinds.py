"""data_kinds.py: classify PRG data ranges by format (docs/DATA_FORMATS.md) into meta/data_kinds.tsv.

  python tools/data_kinds.py [--rom ROM] [--reads DEBUGGER_DIR] [--out meta/data_kinds.tsv]

Two sources, both giving addresses only (the output passes tools/release_audit.py):
  1. Structure walked from the ROM's own pointer tables (level maps, block sets, section records, palettes,
     spawn lists, player starts): the table addresses below come from reading the loader code.
  2. Read logs (btdd_headless --reads, auto-reads-*.tsv): every byte read by a known loader instruction gets
     that loader's format (CHR tile streams, metasprites, music ... see READERS).
Output columns: bank, start, end (inclusive, CPU $8000-$FFFF), kind (btdd.meta data kind), fmt (format id in
docs/DATA_FORMATS.md), conf (high/medium/low), source. tools/export_meta.py applies it to `data` ranges.
"""
import argparse, collections, glob, os

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
GAME = os.path.normpath(os.path.join(REPO, '..', 'Gamefiles'))
ROM = os.path.join(GAME, 'Battletoads-Double Dragon (USA).nes')

# (bank, reader PC range lo, hi) -> (kind, fmt, conf). PCs are the instruction that reads the data byte.
READERS = [
    (1, 0x80DE, 0x80DE, 'graphics', 'chr_rle', 'high'),      # bank 1 sub_809c: escape-coded CHR stream
    (1, 0x8101, 0x8101, 'graphics', 'chr_rle', 'high'),
    (1, 0x8174, 0x8174, 'graphics', 'chr_raw', 'high'),      # bank 1 sub_8174: plain copy to PPUDATA
    (7, 0x81A6, 0x81A6, 'graphics', 'chr_rle', 'high'),      # bank 7 sub_8164: same stream format
    (7, 0x81C9, 0x81C9, 'graphics', 'chr_rle', 'high'),
    (4, 0x808C, 0x808C, 'graphics', 'chr_rle', 'medium'),    # bank 4 sub_802b: same escape coding
    (4, 0x80AF, 0x80AF, 'graphics', 'chr_rle', 'medium'),
    (2, 0x0782, 0x07FF, 'graphics', 'chr_raw', 'high'),      # RAM tile streamer (copied from bank 7 $805F)
    (1, 0x8041, 0x8052, 'table', 'chr_set', 'medium'),       # set entries: count, method, source pointer
    (7, 0x810E, 0x8125, 'table', 'chr_set', 'medium'),
]


def rom_banks(path):
    d = open(path, 'rb').read()[16:]
    return [d[i * 0x8000:(i + 1) * 0x8000] for i in range(8)]


def structure(P):
    """Rows walked from the pointer tables (addresses only)."""
    def b(k, a):
        return P[k][a - 0x8000]

    def w(k, a):
        return b(k, a) | b(k, a + 1) << 8
    rows = []

    def add(k, s, e, kind, fmt, conf, src):
        rows.append((k, s, e, kind, fmt, conf, src))
    # --- sections (bank 6 sub_a0fb) ---
    add(6, 0xA0E2, 0xA0EB, 'table', 'sec_first', 'high', 'level -> first section ($A0E2,x)')
    add(6, 0xA0ED, 0xA0FA, 'table', 'sec_recidx', 'high', 'section -> record offset ($A0EC,y)')
    add(6, 0xA19A, 0xA217, 'table', 'sec_record', 'high', '14 x 9-byte section records')
    # --- palettes (bank 6 sub_a3c2) ---
    n, low = 0, 0x10000
    while 0xA3FA + 2 * n < low:
        p = w(6, 0xA3FA + 2 * n)
        if p < 0x8000:
            break
        low, n = min(low, p), n + 1
    add(6, 0xA3FA, 0xA3FA + 2 * n - 1, 'ptrtable', 'pal_ptrs', 'high', '%d palette pointers' % n)
    pals = sorted({w(6, 0xA3FA + 2 * i) for i in range(n)})
    for p in pals:
        add(6, p, p + 15, 'table', 'pal_16', 'high', '16 colour indices -> PPU $3F00')
    # --- level maps / blocks / block attributes (bank 5 sub_a305, sub_a452) ---
    add(5, 0xA4A1, 0xA4EE, 'table', 'lvl_ptrs', 'high', 'per level 13 x (blocks lo/hi, attr lo/hi, map lo/hi)')
    lv = {}
    for x in range(13):
        lv[x] = (b(5, 0xA4D5 + x) | b(5, 0xA4E2 + x) << 8, b(5, 0xA4A1 + x) | b(5, 0xA4AE + x) << 8,
                 b(5, 0xA4BB + x) | b(5, 0xA4C8 + x) << 8)
    seen = set()
    for x in (2, 3, 4, 5, 6, 8, 9, 0xA):          # levels the game uses (A-C share one set)
        m, blk, att = lv[x]
        nb = (att - blk) // 16
        if (m, blk) in seen or nb <= 0:
            continue
        seen.add((m, blk))
        if m >= 0x8000:
            add(5, m, blk - 1, 'table', 'lvl_map', 'high', 'level %X block map, row-major' % x)
        add(5, blk, att - 1, 'table', 'lvl_blocks', 'high', 'level %X: %d blocks x 16 tile ids' % (x, nb))
        add(5, att, att + nb - 1, 'table', 'lvl_block_attr', 'high', 'level %X: 1 attribute byte per block' % x)
    add(5, 0xF1F5, 0xF218, 'table', 'lvl_map', 'medium', 'level 4 map strip, copied to RAM $0624-$0647')
    # --- player start positions (bank 5 sub_a3f0) ---
    add(5, 0xA38C, 0xA39A, 'table', 'start_idx', 'high', 'section -> player start offset (0 = keep)')
    add(5, 0xA39B, 0xA3EF, 'table', 'start_pos', 'medium', '6 bytes per player: X hi/lo, Y hi/lo, Z hi/lo')
    # --- spawn lists (bank 3 sub_d073 / sub_d0ec) ---
    add(3, 0xD0C1, 0xD0EB, 'table', 'spawn_sec_tables', 'high', 'per section: prespawn count, list lo, list hi')
    add(3, 0xD330, 0xD341, 'table', 'spawn_lvl_ptrs', 'high', 'per level list lo ($D330,x) / hi ($D338,x)')
    starts = {b(3, 0xD0CF + s) | b(3, 0xD0DD + s) << 8 for s in range(1, 15)}
    starts |= {b(3, 0xD330 + x) | b(3, 0xD338 + x) << 8 for x in (2, 3, 4, 5, 6, 8, 9)}
    cover = set()
    for p in starts:
        q = p
        while 0xD000 <= q < 0x10000:
            t = b(3, q)
            if t == 0xFF:
                cover.update(range(q, q + 3))
                q = w(3, q + 1)
                if q in cover:
                    break
                continue
            if t == 0xFE:
                cover.add(q)
                break
            if q in cover:
                break
            cover.update(range(q, q + 11))
            q += 11
    for s, e in runs(sorted(cover)):
        add(3, s, e, 'table', 'spawn_list', 'high', '11-byte spawn records, $FF jump, $FE hold')
    return rows


def runs(addrs, gap=1):
    out, s, prev = [], None, None
    for a in addrs:
        if s is None:
            s = prev = a
        elif a - prev <= gap:
            prev = a
        else:
            out.append((s, prev))
            s = prev = a
    if s is not None:
        out.append((s, prev))
    return out


def from_reads(dirn):
    hit = collections.defaultdict(set)
    for f in glob.glob(os.path.join(dirn, 'auto-reads-*.tsv')):
        for line in open(f):
            c = line.split('\t')
            if c[0] == 'bank':
                continue
            k, a, pc = int(c[0]), int(c[1][1:], 16), int(c[2][1:], 16)
            for rb, lo, hi, kind, fmt, conf in READERS:
                if k == rb and lo <= pc <= hi:
                    hit[(k, kind, fmt, conf)].add(a)
                    break
    rows = []
    for (k, kind, fmt, conf), s in hit.items():
        for a, e in runs(sorted(s), gap=1 if kind == 'graphics' else 4):
            if e - a + 1 >= 16 or kind == 'table':
                rows.append((k, a, e, kind, fmt, conf, 'read log: loader reads'))
    return rows


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--rom', default=ROM)
    ap.add_argument('--reads', default=os.path.join(GAME, 'Debugger'))
    ap.add_argument('--out', default=os.path.join(REPO, 'meta', 'data_kinds.tsv'))
    a = ap.parse_args()
    rows = structure(rom_banks(a.rom)) + from_reads(a.reads)
    rows.sort(key=lambda r: (r[0], r[1], r[2]))
    with open(a.out, 'w', newline='\n') as f:
        f.write('# data_kinds.tsv: PRG data ranges by format (tools/data_kinds.py; formats in docs/DATA_FORMATS.md)\n')
        f.write('bank\tstart\tend\tkind\tfmt\tconf\tsource\n')
        for k, s, e, kind, fmt, conf, src in rows:
            f.write('%d\t$%04X\t$%04X\t%s\t%s\t%s\t%s\n' % (k, s, e, kind, fmt, conf, src))
    tot = collections.Counter()
    for r in rows:
        tot[r[4]] += r[2] - r[1] + 1
    print('%d rows -> %s' % (len(rows), a.out))
    for k, v in sorted(tot.items(), key=lambda kv: -kv[1]):
        print('  %-18s %7d bytes' % (k, v))


if __name__ == '__main__':
    main()
