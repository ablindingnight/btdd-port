"""fcm2fm2.py: convert an old FCEU (< 2.0) .fcm movie (version 2) to a text .fm2 our core can play.

  python tools/fcm2fm2.py IN.fcm OUT.fm2
Format as FCEUX 2.6 reads it (oldmovie.cpp convert_fcm, used as the specification): a header (magic "FCM\\x1a",
version 2, flags, frame count, rerecords, data size, savestate offset, first-frame offset, ROM MD5, emulator
version, ROM name, author), then a stream of events. Each event byte: bit 7 = command (low 5 bits: 1 reset,
2 power), else it toggles button (bits 0-2) of pad (bits 3-4); bits 5-6 = how many little-endian bytes follow
with the number of frames until the next event. Movies must start from power-on or reset (not a savestate); a start-from-reset is played from power-on.
"""
import struct, sys

MAGIC = 0x1A4D4346
FLAG_FROM_RESET, FLAG_PAL, FLAG_FROM_POWERON = 0x02, 0x04, 0x08
BUTTONS = 'RLDUTSBA'


def main():
    raw = open(sys.argv[1], 'rb').read()
    magic, version = struct.unpack_from('<II', raw, 0)
    if magic != MAGIC or version != 2:
        sys.exit('not a version-2 .fcm (magic %08X version %d)' % (magic, version))
    flags = raw[8]
    frames, rerecords, size, _ss_off, first = struct.unpack_from('<IIIII', raw, 12)
    if not flags & (FLAG_FROM_POWERON | FLAG_FROM_RESET):
        sys.exit('movie starts from a savestate: not supported')
    data = raw[first:first + size]
    pos = 0

    def rd():
        nonlocal pos
        if pos >= len(data):
            return -1
        pos += 1
        return data[pos - 1]

    joop = [0, 0, 0, 0]
    st = {'nextd': -1, 'nextts': 0, 'framets': 0, 'cmd': 0}

    def addjoy():
        while st['nextts'] == st['framets'] or st['nextd'] == -1:
            nd = st['nextd']
            if nd != -1:
                if nd & 0x80:
                    c = nd & 0x1F
                    st['cmd'] = 1 if c == 1 else (2 if c == 2 else st['cmd'])
                else:
                    joop[(nd >> 3) & 3] ^= 1 << (nd & 7)
            t = rd()
            if t < 0:
                return
            d = t
            n = (t >> 5) & 3
            ts = 0
            for k in range(n):
                ts |= rd() << (k * 8)
            if n == 1 and not ts:
                ts |= rd() << 8
            elif n == 2 and not ts:
                ts |= rd() << 16
            st['nextts'] = ts
            if nd != -1:
                st['framets'] = 0
            st['nextd'] = d
        st['framets'] += 1

    out = ['version 3', 'emuVersion 9800', 'rerecordCount %d' % rerecords, 'palFlag %d' % (1 if flags & FLAG_PAL else 0),
           'fourscore 0', 'port0 1', 'port1 1', 'port2 0', 'comment converted from %s by tools/fcm2fm2.py' % sys.argv[1].replace('\\', '/').split('/')[-1]]
    resets = 0
    for i in range(frames):
        st['cmd'] = 0  # a "from reset" movie's reset on frame 0 (just after power-on) is dropped: our core
        addjoy()       # does not run fm2 reset commands, and the game re-initialises itself anyway
        if i == 0:
            st['cmd'] = 0
        resets += st['cmd'] != 0
        pads = [''.join(BUTTONS[k] if joop[p] & (0x80 >> k) else '.' for k in range(8)) for p in range(2)]
        out.append('|%d|%s|%s||' % (st['cmd'], pads[0], pads[1]))
    open(sys.argv[2], 'w', newline='\n').write('\n'.join(out) + '\n')
    print('%d frames, %d reset/power commands, from %s' % (frames, resets, 'reset' if flags & FLAG_FROM_RESET else 'power-on'))


if __name__ == '__main__':
    main()
