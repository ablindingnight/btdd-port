/* Rewind buffer: a ring of compressed frame-to-frame deltas of a fixed-size blob (machine state + picture).
 *
 * Each push stores prev XOR new, run-length coded (zero runs and literal runs, varint lengths), and keeps the
 * new blob as `prev`. Popping applies the newest delta to prev, which yields the blob pushed before it, exactly;
 * so stepping back works across anything (loads, resets) without keyframes. The oldest entry is a delta against
 * a blob that is gone, so pops stop when one entry is left. Oldest entries are dropped to stay within the byte
 * budget and the entry cap. Platform independent (no SDL). */
#ifndef BTDD_REWIND_H
#define BTDD_REWIND_H
#include <stddef.h>
#include <stdint.h>

typedef struct RewindEntry {
    uint8_t *data;
    size_t size;
} RewindEntry;

typedef struct Rewind {
    size_t blob_size;
    uint8_t *prev;        /* the last pushed (or popped-to) blob */
    int have_prev;
    RewindEntry *ring;
    int cap, head, count; /* head = index of the oldest entry */
    size_t bytes, max_bytes;
    uint8_t *scratch;     /* worst-case encode buffer */
} Rewind;

int rewind_init(Rewind *r, size_t blob_size, int max_entries, size_t max_bytes); /* 0 = ok */
void rewind_free(Rewind *r);
void rewind_clear(Rewind *r);
int rewind_push(Rewind *r, const uint8_t *blob); /* 0 = ok */
/* Steps back one push: writes the previous blob to out (blob_size bytes). Returns 0 when nothing is left. */
int rewind_pop(Rewind *r, uint8_t *out);
int rewind_available(const Rewind *r); /* number of pops possible */

#endif
