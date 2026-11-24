/* Minimal SHA-1 (FIPS 180-4), used only to identify the user's ROM. */
#include "sha1.h"
#include <string.h>

#define ROL(v, n) (((v) << (n)) | ((v) >> (32 - (n))))

static void block(uint32_t h[5], const uint8_t *p)
{
    uint32_t w[80], a, b, c, d, e, f, k, t;
    int i;
    for (i = 0; i < 16; i++)
        w[i] = (uint32_t)p[4 * i] << 24 | (uint32_t)p[4 * i + 1] << 16 | (uint32_t)p[4 * i + 2] << 8 | p[4 * i + 3];
    for (i = 16; i < 80; i++)
        w[i] = ROL(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    a = h[0]; b = h[1]; c = h[2]; d = h[3]; e = h[4];
    for (i = 0; i < 80; i++) {
        if (i < 20)      { f = (b & c) | (~b & d);          k = 0x5A827999; }
        else if (i < 40) { f = b ^ c ^ d;                   k = 0x6ED9EBA1; }
        else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; }
        else             { f = b ^ c ^ d;                   k = 0xCA62C1D6; }
        t = ROL(a, 5) + f + e + k + w[i];
        e = d; d = c; c = ROL(b, 30); b = a; a = t;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
}

void sha1(const uint8_t *data, size_t len, uint8_t out[20])
{
    uint32_t h[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};
    uint8_t tail[128];
    size_t full = len & ~(size_t)63, rest = len - full, tlen;
    uint64_t bits = (uint64_t)len * 8;
    size_t i;

    for (i = 0; i < full; i += 64)
        block(h, data + i);
    memset(tail, 0, sizeof tail);
    memcpy(tail, data + full, rest);
    tail[rest] = 0x80;
    tlen = rest < 56 ? 64 : 128;
    for (i = 0; i < 8; i++)
        tail[tlen - 1 - i] = (uint8_t)(bits >> (8 * i));
    for (i = 0; i < tlen; i += 64)
        block(h, tail + i);
    for (i = 0; i < 20; i++)
        out[i] = (uint8_t)(h[i / 4] >> (24 - 8 * (i % 4)));
}
