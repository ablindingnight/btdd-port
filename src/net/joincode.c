/* Join codes (see joincode.h). Crockford-style base32 (no I, L, O, U), 13 bytes -> 21 characters. */
#include "joincode.h"
#include <stdio.h>
#include <string.h>

static const char ALPHA[] = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";

static uint8_t sum(const uint8_t *b, int n)
{
    uint8_t s = 0x5A;
    int i;
    for (i = 0; i < n; i++)
        s = (uint8_t)((s << 1 | s >> 7) ^ b[i]);
    return s;
}

static void pack(const JoinEndpoints *e, uint8_t b[13])
{
    b[0] = (uint8_t)(e->public_ip >> 24);
    b[1] = (uint8_t)(e->public_ip >> 16);
    b[2] = (uint8_t)(e->public_ip >> 8);
    b[3] = (uint8_t)e->public_ip;
    b[4] = (uint8_t)(e->public_port >> 8);
    b[5] = (uint8_t)e->public_port;
    b[6] = (uint8_t)(e->local_ip >> 24);
    b[7] = (uint8_t)(e->local_ip >> 16);
    b[8] = (uint8_t)(e->local_ip >> 8);
    b[9] = (uint8_t)e->local_ip;
    b[10] = (uint8_t)(e->local_port >> 8);
    b[11] = (uint8_t)e->local_port;
    b[12] = sum(b, 12);
}

void joincode_encode(const JoinEndpoints *e, char out[JOINCODE_LEN])
{
    uint8_t b[13];
    char c[22];
    uint32_t acc = 0;
    int bits = 0, i, n = 0;
    pack(e, b);
    for (i = 0; i < 13; i++) {
        acc = (acc << 8) | b[i];
        bits += 8;
        while (bits >= 5) {
            c[n++] = ALPHA[(acc >> (bits - 5)) & 31];
            bits -= 5;
        }
    }
    if (bits)
        c[n++] = ALPHA[(acc << (5 - bits)) & 31];
    c[n] = 0; /* 21 characters */
    snprintf(out, JOINCODE_LEN, "BT1-%.5s-%.5s-%.5s-%.6s", c, c + 5, c + 10, c + 15);
}

static int value(char ch)
{
    const char *p;
    if (ch >= 'a' && ch <= 'z')
        ch = (char)(ch - 32);
    if (ch == 'O')
        ch = '0';
    if (ch == 'I' || ch == 'L')
        ch = '1';
    p = strchr(ALPHA, ch);
    return ch && p ? (int)(p - ALPHA) : -1;
}

int joincode_decode(const char *code, JoinEndpoints *e)
{
    uint8_t b[13];
    uint32_t acc = 0;
    int bits = 0, n = 0, got = 0;
    const char *s = code;
    while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n')
        s++;
    if ((s[0] == 'B' || s[0] == 'b') && (s[1] == 'T' || s[1] == 't') && s[2] == '1')
        s += 3;
    else
        return -1;
    for (; *s && n < 13; s++) {
        int v;
        if (*s == '-' || *s == ' ' || *s == '\r' || *s == '\n' || *s == '\t')
            continue;
        v = value(*s);
        if (v < 0)
            return -1;
        acc = (acc << 5) | (uint32_t)v;
        bits += 5;
        got++;
        if (bits >= 8) {
            b[n++] = (uint8_t)(acc >> (bits - 8));
            bits -= 8;
        }
    }
    if (n != 13 || got != 21 || sum(b, 12) != b[12])
        return -1;
    e->public_ip = (uint32_t)b[0] << 24 | (uint32_t)b[1] << 16 | (uint32_t)b[2] << 8 | b[3];
    e->public_port = (uint16_t)(b[4] << 8 | b[5]);
    e->local_ip = (uint32_t)b[6] << 24 | (uint32_t)b[7] << 16 | (uint32_t)b[8] << 8 | b[9];
    e->local_port = (uint16_t)(b[10] << 8 | b[11]);
    return 0;
}
