/* Minimal .zip reader for the launcher: finds the first *.nes entry and returns its bytes. Supports "stored"
 * and "deflate" entries (RFC 1951, a small decoder written for this project). No encryption, no zip64. */
#include "unzip.h"
#include <stdlib.h>
#include <string.h>

/* ---------- inflate ---------- */
typedef struct Bits {
    const uint8_t *in;
    size_t len, pos;
    uint32_t buf;
    int cnt;
    uint8_t *out;
    size_t out_len, out_cap;
    int err;
} Bits;

static int need(Bits *s, int n)
{
    while (s->cnt < n) {
        if (s->pos >= s->len) {
            s->err = 1;
            return 0;
        }
        s->buf |= (uint32_t)s->in[s->pos++] << s->cnt;
        s->cnt += 8;
    }
    return 1;
}
static int bits(Bits *s, int n)
{
    int v;
    if (!n)
        return 0;
    if (!need(s, n))
        return 0;
    v = (int)(s->buf & ((1u << n) - 1));
    s->buf >>= n;
    s->cnt -= n;
    return v;
}
static int put(Bits *s, uint8_t b)
{
    if (s->out_len == s->out_cap) {
        s->err = 1;
        return 0;
    }
    s->out[s->out_len++] = b;
    return 1;
}

#define MAXBITS 15
typedef struct Huff {
    short count[MAXBITS + 1];
    short symbol[320];
} Huff;

static int build(Huff *h, const short *len, int n)
{
    short offs[MAXBITS + 1];
    int i, left = 1;
    memset(h->count, 0, sizeof h->count);
    for (i = 0; i < n; i++)
        h->count[len[i]]++;
    if (h->count[0] == n)
        return 0;
    for (i = 1; i <= MAXBITS; i++) {
        left <<= 1;
        left -= h->count[i];
        if (left < 0)
            return -1;
    }
    offs[1] = 0;
    for (i = 1; i < MAXBITS; i++)
        offs[i + 1] = (short)(offs[i] + h->count[i]);
    for (i = 0; i < n; i++)
        if (len[i])
            h->symbol[offs[len[i]]++] = (short)i;
    return left;
}

static int decode(Bits *s, const Huff *h)
{
    int code = 0, first = 0, index = 0, len;
    for (len = 1; len <= MAXBITS; len++) {
        int count;
        code |= bits(s, 1);
        if (s->err)
            return -1;
        count = h->count[len];
        if (code - count < first)
            return h->symbol[index + (code - first)];
        index += count;
        first += count;
        first <<= 1;
        code <<= 1;
    }
    s->err = 1;
    return -1;
}

static const short LBASE[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
static const short LEXT[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
static const short DBASE[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
static const short DEXT[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

static int codes(Bits *s, const Huff *lc, const Huff *dc)
{
    for (;;) {
        int sym = decode(s, lc);
        if (sym < 0)
            return -1;
        if (sym < 256) {
            if (!put(s, (uint8_t)sym))
                return -1;
        } else if (sym == 256)
            return 0;
        else {
            int len, dist;
            sym -= 257;
            if (sym >= 29)
                return -1;
            len = LBASE[sym] + bits(s, LEXT[sym]);
            sym = decode(s, dc);
            if (sym < 0 || sym >= 30)
                return -1;
            dist = DBASE[sym] + bits(s, DEXT[sym]);
            if (s->err || (size_t)dist > s->out_len)
                return -1;
            while (len--)
                if (!put(s, s->out[s->out_len - (size_t)dist]))
                    return -1;
        }
    }
}

static int fixed(Bits *s)
{
    static Huff lc, dc;
    static int done;
    if (!done) {
        short l[288];
        int i;
        for (i = 0; i < 144; i++) l[i] = 8;
        for (; i < 256; i++) l[i] = 9;
        for (; i < 280; i++) l[i] = 7;
        for (; i < 288; i++) l[i] = 8;
        build(&lc, l, 288);
        for (i = 0; i < 30; i++) l[i] = 5;
        build(&dc, l, 30);
        done = 1;
    }
    return codes(s, &lc, &dc);
}

static int dynamic(Bits *s)
{
    static const short order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
    short lengths[320];
    Huff lc, dc;
    int nlen = bits(s, 5) + 257, ndist = bits(s, 5) + 1, ncode = bits(s, 4) + 4, i;
    if (s->err || nlen > 286 || ndist > 30)
        return -1;
    for (i = 0; i < 19; i++)
        lengths[order[i]] = (short)(i < ncode ? bits(s, 3) : 0);
    if (build(&lc, lengths, 19) != 0)
        return -1;
    for (i = 0; i < nlen + ndist;) {
        int sym = decode(s, &lc), len = 0, rep;
        if (sym < 0)
            return -1;
        if (sym < 16) {
            lengths[i++] = (short)sym;
            continue;
        }
        if (sym == 16) {
            if (!i)
                return -1;
            len = lengths[i - 1];
            rep = 3 + bits(s, 2);
        } else if (sym == 17)
            rep = 3 + bits(s, 3);
        else
            rep = 11 + bits(s, 7);
        if (i + rep > nlen + ndist)
            return -1;
        while (rep--)
            lengths[i++] = (short)len;
    }
    if (lengths[256] == 0)
        return -1;
    if (build(&lc, lengths, nlen) < 0 || build(&dc, lengths + nlen, ndist) < 0)
        return -1;
    return codes(s, &lc, &dc);
}

static int inflate_raw(const uint8_t *in, size_t len, uint8_t *out, size_t cap, size_t *out_len)
{
    Bits s;
    int last, type, r = 0;
    memset(&s, 0, sizeof s);
    s.in = in, s.len = len, s.out = out, s.out_cap = cap;
    do {
        last = bits(&s, 1);
        type = bits(&s, 2);
        if (s.err)
            return -1;
        if (type == 0) {
            unsigned n;
            s.buf = 0, s.cnt = 0;
            if (s.pos + 4 > s.len)
                return -1;
            n = s.in[s.pos] | (unsigned)s.in[s.pos + 1] << 8;
            s.pos += 4;
            if (s.pos + n > s.len)
                return -1;
            while (n--)
                if (!put(&s, s.in[s.pos++]))
                    return -1;
        } else if (type == 1)
            r = fixed(&s);
        else if (type == 2)
            r = dynamic(&s);
        else
            return -1;
        if (r || s.err)
            return -1;
    } while (!last);
    *out_len = s.out_len;
    return 0;
}

/* ---------- zip ---------- */
static uint32_t rd16(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8; }
static uint32_t rd32(const uint8_t *p) { return rd16(p) | rd16(p + 2) << 16; }

static int ends_with_nes(const uint8_t *name, size_t n)
{
    return n > 4 && name[n - 4] == '.' && (name[n - 3] | 32) == 'n' && (name[n - 2] | 32) == 'e' && (name[n - 1] | 32) == 's';
}

int unzip_first_nes(const uint8_t *zip, size_t len, uint8_t **out, size_t *out_len, char *name, size_t name_len)
{
    size_t eocd, i, cd, n_entries, k;
    if (len < 22)
        return -1;
    for (eocd = len - 22;; eocd--) { /* end of central directory, scanning back over a comment */
        if (rd32(zip + eocd) == 0x06054b50)
            break;
        if (eocd == 0 || len - eocd > 22 + 65535)
            return -1;
    }
    n_entries = rd16(zip + eocd + 10);
    cd = rd32(zip + eocd + 16);
    for (i = cd, k = 0; k < n_entries; k++) {
        uint32_t method, csize, usize, nlen, xlen, clen, lho;
        if (i + 46 > len || rd32(zip + i) != 0x02014b50)
            return -1;
        method = rd16(zip + i + 10);
        csize = rd32(zip + i + 20);
        usize = rd32(zip + i + 24);
        nlen = rd16(zip + i + 28);
        xlen = rd16(zip + i + 30);
        clen = rd16(zip + i + 32);
        lho = rd32(zip + i + 42);
        if (i + 46 + nlen > len)
            return -1;
        if (ends_with_nes(zip + i + 46, nlen) && usize > 0 && usize < 16u * 1024 * 1024) {
            size_t data, got = 0;
            uint8_t *buf;
            if ((size_t)lho + 30 > len || rd32(zip + lho) != 0x04034b50)
                return -1;
            data = lho + 30 + rd16(zip + lho + 26) + rd16(zip + lho + 28);
            if (data + csize > len)
                return -1;
            buf = (uint8_t *)malloc(usize);
            if (!buf)
                return -1;
            if (method == 0 && csize == usize) {
                memcpy(buf, zip + data, usize);
                got = usize;
            } else if (method != 8 || inflate_raw(zip + data, csize, buf, usize, &got) != 0 || got != usize) {
                free(buf);
                return -2; /* unsupported or damaged entry */
            }
            if (name && name_len) {
                const uint8_t *nm = zip + i + 46;
                size_t s = 0, j;
                for (j = 0; j < nlen; j++) /* keep only the file name, not folders */
                    if (nm[j] == '/' || nm[j] == '\\')
                        s = j + 1;
                if (nlen - s >= name_len)
                    s = nlen - (name_len - 1);
                memcpy(name, nm + s, nlen - s);
                name[nlen - s] = 0;
            }
            *out = buf;
            *out_len = got;
            return 0;
        }
        i += 46 + nlen + xlen + clen;
    }
    return -3; /* no .nes inside */
}
