/* Minimal PNG writer (RGB, 8 bit, zlib "stored" blocks: no compression library needed). */
#include "png.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t crc_table[256];

static void crc_init(void)
{
    uint32_t c;
    int n, k;
    if (crc_table[1])
        return;
    for (n = 0; n < 256; n++) {
        c = (uint32_t)n;
        for (k = 0; k < 8; k++)
            c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        crc_table[n] = c;
    }
}

static uint32_t crc(uint32_t c, const uint8_t *buf, size_t len)
{
    size_t i;
    c ^= 0xFFFFFFFFu;
    for (i = 0; i < len; i++)
        c = crc_table[(c ^ buf[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

static void be32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static int chunk(FILE *f, const char *type, const uint8_t *data, size_t len)
{
    uint8_t hdr[8];
    uint32_t c;
    be32(hdr, (uint32_t)len);
    memcpy(hdr + 4, type, 4);
    c = crc(0, hdr + 4, 4);
    c = crc(c, data, len);
    be32(hdr, (uint32_t)len);
    if (fwrite(hdr, 1, 8, f) != 8 || (len && fwrite(data, 1, len, f) != len))
        return -1;
    be32(hdr, c);
    return fwrite(hdr, 1, 4, f) == 4 ? 0 : -1;
}

int png_write_rgb(const char *path, const uint32_t *argb, int w, int h, int pitch)
{
    size_t row = (size_t)w * 3 + 1, raw_len = row * (size_t)h, blocks = (raw_len + 65534) / 65535;
    size_t z_len = 2 + raw_len + blocks * 5 + 4, i, pos = 0;
    uint8_t *raw = malloc(raw_len), *z = malloc(z_len), ihdr[13];
    uint32_t a = 1, b = 0;
    FILE *f;
    int x, y, ok;

    if (!raw || !z) {
        free(raw);
        free(z);
        return -1;
    }
    crc_init();
    for (y = 0; y < h; y++) {
        uint8_t *r = raw + (size_t)y * row;
        r[0] = 0; /* filter: none */
        for (x = 0; x < w; x++) {
            uint32_t p = argb[y * pitch + x];
            r[1 + x * 3] = (uint8_t)(p >> 16);
            r[2 + x * 3] = (uint8_t)(p >> 8);
            r[3 + x * 3] = (uint8_t)p;
        }
    }
    z[pos++] = 0x78;
    z[pos++] = 0x01;
    for (i = 0; i < raw_len; i += 65535) {
        size_t n = raw_len - i < 65535 ? raw_len - i : 65535;
        z[pos++] = (uint8_t)(i + n == raw_len);
        z[pos++] = (uint8_t)n;
        z[pos++] = (uint8_t)(n >> 8);
        z[pos++] = (uint8_t)~n;
        z[pos++] = (uint8_t)(~n >> 8);
        memcpy(z + pos, raw + i, n);
        pos += n;
    }
    for (i = 0; i < raw_len; i++) {
        a = (a + raw[i]) % 65521;
        b = (b + a) % 65521;
    }
    be32(z + pos, b << 16 | a);
    pos += 4;

    be32(ihdr, (uint32_t)w);
    be32(ihdr + 4, (uint32_t)h);
    ihdr[8] = 8;  /* bit depth */
    ihdr[9] = 2;  /* RGB */
    ihdr[10] = ihdr[11] = ihdr[12] = 0;
    f = fopen(path, "wb");
    ok = f && fwrite("\x89PNG\r\n\x1a\n", 1, 8, f) == 8 && chunk(f, "IHDR", ihdr, 13) == 0 &&
         chunk(f, "IDAT", z, pos) == 0 && chunk(f, "IEND", NULL, 0) == 0;
    if (f && fclose(f) != 0)
        ok = 0;
    free(raw);
    free(z);
    return ok ? 0 : -1;
}
