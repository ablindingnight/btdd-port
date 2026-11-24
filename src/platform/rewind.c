/* Rewind buffer (see rewind.h). */
#include "rewind.h"
#include <stdlib.h>
#include <string.h>

int rewind_init(Rewind *r, size_t blob_size, int max_entries, size_t max_bytes)
{
    memset(r, 0, sizeof *r);
    r->blob_size = blob_size;
    r->cap = max_entries;
    r->max_bytes = max_bytes;
    r->prev = calloc(1, blob_size);
    r->ring = calloc((size_t)max_entries, sizeof *r->ring);
    /* worst case: alternating literal bytes cost at most 1 + 2 * varint per byte group; 2x + slack is safe */
    r->scratch = malloc(blob_size * 2 + 64);
    if (!r->prev || !r->ring || !r->scratch) {
        rewind_free(r);
        return -1;
    }
    return 0;
}

void rewind_clear(Rewind *r)
{
    while (r->count) {
        free(r->ring[r->head].data);
        r->ring[r->head].data = NULL;
        r->head = (r->head + 1) % r->cap;
        r->count--;
    }
    r->head = 0;
    r->bytes = 0;
    r->have_prev = 0;
}

void rewind_free(Rewind *r)
{
    if (r->ring)
        rewind_clear(r);
    free(r->prev);
    free(r->ring);
    free(r->scratch);
    memset(r, 0, sizeof *r);
}

static uint8_t *put_varint(uint8_t *p, size_t v)
{
    while (v >= 0x80) {
        *p++ = (uint8_t)(v | 0x80);
        v >>= 7;
    }
    *p++ = (uint8_t)v;
    return p;
}

static const uint8_t *get_varint(const uint8_t *p, size_t *v)
{
    size_t x = 0;
    int shift = 0;
    while (*p & 0x80) {
        x |= (size_t)(*p++ & 0x7F) << shift;
        shift += 7;
    }
    *v = x | (size_t)*p++ << shift;
    return p;
}

/* a XOR b as: (zero run, literal count, literals)* */
static size_t encode(const uint8_t *a, const uint8_t *b, size_t n, uint8_t *out)
{
    uint8_t *o = out;
    size_t i = 0;
    while (i < n) {
        size_t z = i, l;
        while (z < n && a[z] == b[z])
            z++;
        l = z;
        /* a literal run ends at the first 4 equal bytes in a row (shorter zero runs are cheaper inline) */
        while (l < n) {
            if (a[l] == b[l]) {
                size_t k = l;
                while (k < n && k < l + 4 && a[k] == b[k])
                    k++;
                if (k == n || k == l + 4)
                    break;
                l = k;
            } else {
                l++;
            }
        }
        o = put_varint(o, z - i);
        o = put_varint(o, l - z);
        for (; z < l; z++)
            *o++ = a[z] ^ b[z];
        i = l;
    }
    return (size_t)(o - out);
}

static void apply(uint8_t *buf, size_t n, const uint8_t *code, size_t size)
{
    const uint8_t *p = code, *end = code + size;
    size_t i = 0;
    while (p < end && i < n) {
        size_t z, l;
        p = get_varint(p, &z);
        p = get_varint(p, &l);
        i += z;
        while (l-- && i < n)
            buf[i++] ^= *p++;
    }
}

static void drop_oldest(Rewind *r)
{
    RewindEntry *e = &r->ring[r->head];
    r->bytes -= e->size;
    free(e->data);
    e->data = NULL;
    e->size = 0;
    r->head = (r->head + 1) % r->cap;
    r->count--;
}

int rewind_push(Rewind *r, const uint8_t *blob)
{
    size_t size;
    RewindEntry *e;
    if (!r->have_prev)
        memset(r->prev, 0, r->blob_size);
    size = encode(r->prev, blob, r->blob_size, r->scratch);
    while (r->count && (r->count == r->cap || r->bytes + size > r->max_bytes))
        drop_oldest(r);
    e = &r->ring[(r->head + r->count) % r->cap];
    e->data = malloc(size ? size : 1);
    if (!e->data)
        return -1;
    memcpy(e->data, r->scratch, size);
    e->size = size;
    r->count++;
    r->bytes += size;
    memcpy(r->prev, blob, r->blob_size);
    r->have_prev = 1;
    return 0;
}

int rewind_available(const Rewind *r)
{
    return r->count > 1 ? r->count - 1 : 0;
}

int rewind_pop(Rewind *r, uint8_t *out)
{
    int last;
    RewindEntry *e;
    if (r->count <= 1)
        return 0;
    last = (r->head + r->count - 1) % r->cap;
    e = &r->ring[last];
    apply(r->prev, r->blob_size, e->data, e->size);
    r->bytes -= e->size;
    free(e->data);
    e->data = NULL;
    e->size = 0;
    r->count--;
    memcpy(out, r->prev, r->blob_size);
    return 1;
}
