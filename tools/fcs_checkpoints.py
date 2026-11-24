"""fcs_checkpoints.py: turn the raw states of tools/fceux/checkpoints.lua into named, movie-free FCEUX states.

  python tools/fcs_checkpoints.py RAW_DIR OUT_DIR PREFIX[=NAME] [PREFIX[=NAME] ...]
For each PREFIX.tsv in RAW_DIR keeps the first state of every (stage, section) with section != 0
(NAME_stageN_secSS.fcs), the state where a stage's section drops back to 0 (NAME_stageN_end.fcs: stage cleared,
the intermission and next stage intro follow) and states saved by name from the menu script (NAME_word.fcs);
everything after the route returns to the title screen (a desync or game over) is dropped. Every kept state is
rewritten without FCEUX's movie chunks (6 = movie position, 7 = embedded movie), so loading it never attaches a
movie. Writes OUT_DIR/states.tsv (file, source frame, level, section, mode, lives, hp). The states are ROM-derived:
keep RAW_DIR and OUT_DIR under ..\\Gamefiles.
"""
import os, struct, sys, zlib

STAGE = {0x03: 1, 0x05: 2, 0x02: 3, 0x04: 4, 0x08: 5, 0x09: 6, 0x06: 7, 0x0B: 'ending'}
NO_MOVIE = 0xFFFFFFFF


def strip_movie(src, dst):
    raw = open(src, 'rb').read()
    assert raw[:4] == b'FCSX', src
    total, version, comprlen = struct.unpack('<III', raw[4:16])
    body = raw[16:] if comprlen == NO_MOVIE else zlib.decompress(raw[16:16 + comprlen])
    assert len(body) == total, src
    out, i, dropped = [], 0, []
    while i < len(body):
        t, size = body[i], struct.unpack('<I', body[i + 1:i + 5])[0]
        if t in (6, 7):
            dropped.append(t)
        else:
            out.append(body[i:i + 5 + size])
        i += 5 + size
    body = b''.join(out)
    comp = zlib.compress(body, 9)
    with open(dst, 'wb') as f:
        f.write(b'FCSX' + struct.pack('<III', len(body), version, len(comp)) + comp)
    return dropped


def main():
    raw_dir, out_dir = sys.argv[1], sys.argv[2]
    os.makedirs(out_dir, exist_ok=True)
    index = ['file\tsource\tframe\tlevel\tsection\tmode\tlives1\tlives2\thp0\thp1']
    for spec in sys.argv[3:]:
        prefix, _, name = spec.partition('=')
        name = name or prefix
        rows = [l.rstrip('\n').split('\t') for l in open(os.path.join(raw_dir, prefix + '.tsv'))][1:]
        seen, prev = set(), None
        for r in rows:
            frame, lv, sec = int(r[0]), int(r[1], 16), int(r[2], 16)
            src = r[8]
            word = os.path.basename(src)[len(prefix) + 9:-4]          # after PREFIX_fNNNNNN_
            if lv == 0 and frame > 1000:
                break
            out = None
            if not word.startswith('L'):
                out = '%s_%s' % (name, word)
            elif lv in STAGE and sec != 0:
                out = '%s_stage%s_sec%02X' % (name, STAGE[lv], sec)
            elif lv in STAGE and prev and prev[0] == lv and prev[1] != 0:
                out = '%s_stage%s_end' % (name, STAGE[lv])
            prev = (lv, sec) if lv < 0xF0 else prev
            if out is None or out in seen:
                continue
            seen.add(out)
            dropped = strip_movie(os.path.join(raw_dir, os.path.basename(src)), os.path.join(out_dir, out + '.fcs'))
            index.append('\t'.join([out + '.fcs', prefix] + r[:8]))
            print('%-28s frame %6d  lives %s/%s  hp %s/%s  movie chunks dropped %s' % (out, frame, r[4], r[5], r[6],
                                                                                     r[7], dropped or '-'))
    with open(os.path.join(out_dir, 'states.tsv'), 'w', newline='\n') as f:
        f.write('\n'.join(index) + '\n')


if __name__ == '__main__':
    main()
