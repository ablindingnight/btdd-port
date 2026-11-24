"""fm3tofm2.py: input of an FCEUX TAS Editor project (.fm3, "binary 1") as a text .fm2 our core can play.

  python tools/fm3tofm2.py IN.fm3 OUT.fm2
Binary layout after the header line "length N" and a '|': per frame one command byte, then one byte per
enabled port (FCEUX bit order: A B Select Start Up Down Left Right = bits 0-7). The TAS Editor data that
follows the input (greenzone, markers, ...) is ignored. Movies are input files, not ROM data, but keep the
output next to the input under ..\\Gamefiles anyway.
"""
import sys

BUTTONS = 'RLDUTSBA'  # fm2 text order, left to right = bits 7..0


def main():
    raw = open(sys.argv[1], 'rb').read()
    head_end = raw.index(b'|')
    header = raw[:head_end].decode('latin-1').splitlines()
    kv = dict(l.split(' ', 1) for l in header if ' ' in l)
    if kv.get('binary') != '1':
        sys.exit('not a binary movie')
    n = int(kv['length'])
    ports = [int(kv.get('port%d' % i, '0')) for i in range(3)]
    nports = sum(1 for p in ports[:2] if p == 1)
    per = 1 + nports
    body = raw[head_end + 1:head_end + 1 + n * per]
    if len(body) < n * per:
        sys.exit('short input block')
    out = [l for l in header if not l.startswith(('binary', 'length'))]
    for f in range(n):
        rec = body[f * per:(f + 1) * per]
        pads = []
        for p in range(2):
            if p < nports:
                b = rec[1 + p]
                pads.append(''.join(BUTTONS[i] if b & (0x80 >> i) else '.' for i in range(8)))
            else:
                pads.append('')
        out.append('|%d|%s|%s||' % (rec[0], pads[0], pads[1]))
    open(sys.argv[2], 'w', newline='\n').write('\n'.join(out) + '\n')
    print('%d frames written' % n)


if __name__ == '__main__':
    main()
