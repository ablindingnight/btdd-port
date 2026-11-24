"""shots.py: turn btdd_headless --shot-every output (*.shot) into PNG pictures (background + sprites).

The .shot files and the PNGs show game graphics: keep them under ..\\Gamefiles (never in this repo).
  python tools/shots.py DIR [--scale 2]      writes DIR/*.png next to every *.shot (skips existing)
"""
import glob, os, struct, sys, zlib

W, H = 256, 240
# A common 2C02 palette (RGB per NES color index 0-63)
PAL = [int(x, 16) for x in (
    '7C7C7C 0000FC 0000BC 4428BC 940084 A80020 A81000 881400 503000 007800 006800 005800 004058 000000 000000 000000 '
    'BCBCBC 0078F8 0058F8 6844FC D800CC E40058 F83800 E45C10 AC7C00 00B800 00A800 00A844 008888 000000 000000 000000 '
    'F8F8F8 3CBCFC 6888FC 9878F8 F878F8 F85898 F87858 FCA044 F8B800 B8F818 58D854 58F898 00E8D8 787878 000000 000000 '
    'FCFCFC A4E4FC B8B8F8 D8B8F8 F8B8F8 F8A4C0 F0D0B0 FCE0A8 F8D878 D8F878 B8F8B8 B8F8D8 00FCFC F8D8F8 000000 000000'
).split()]


def render(data):
    fb = data[:W * H]
    chr_ = data[W * H:W * H + 0x2000]
    oam = data[W * H + 0x2000:W * H + 0x2100]
    pal = data[W * H + 0x2100:W * H + 0x2120]
    ctrl = data[W * H + 0x2120]
    img = [PAL[b & 0x3F] for b in fb]
    opaque_bg = [not (b & 0x40) for b in fb]
    tall = ctrl & 0x20
    for s in range(63, -1, -1):
        y, tile, attr, x = oam[s * 4:s * 4 + 4]
        y += 1
        if y >= 240:
            continue
        hgt = 16 if tall else 8
        for row in range(hgt):
            py = y + row
            if py >= H:
                break
            r = hgt - 1 - row if attr & 0x80 else row
            if tall:
                base = (tile & 1) * 0x1000 + (tile & 0xFE) * 16 + (16 if r >= 8 else 0)
            else:
                base = (0x1000 if ctrl & 0x08 else 0) + tile * 16
            lo, hi = chr_[base + (r & 7)], chr_[base + (r & 7) + 8]
            for col in range(8):
                px = x + col
                if px >= W:
                    break
                bit = col if attr & 0x40 else 7 - col
                c = ((lo >> bit) & 1) | (((hi >> bit) & 1) << 1)
                if not c:
                    continue
                if attr & 0x20 and opaque_bg[py * W + px]:
                    continue
                img[py * W + px] = PAL[pal[16 + (attr & 3) * 4 + c] & 0x3F]
    return img


def png(path, img, scale):
    w, h = W * scale, H * scale
    rows = bytearray()
    for y in range(h):
        rows.append(0)
        line = img[(y // scale) * W:(y // scale + 1) * W]
        for p in line:
            rows += bytes(((p >> 16) & 255, (p >> 8) & 255, p & 255)) * scale
    def chunk(t, d):
        return struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d) & 0xFFFFFFFF)
    out = b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0))
    out += chunk(b'IDAT', zlib.compress(bytes(rows), 6)) + chunk(b'IEND', b'')
    open(path, 'wb').write(out)


def main():
    d = sys.argv[1]
    scale = int(sys.argv[sys.argv.index('--scale') + 1]) if '--scale' in sys.argv else 2
    n = 0
    for f in sorted(glob.glob(os.path.join(d, '*.shot'))):
        p = f[:-5] + '.png'
        if os.path.exists(p):
            continue
        png(p, render(open(f, 'rb').read()), scale)
        n += 1
    print('%d pictures written' % n)


if __name__ == '__main__':
    main()
