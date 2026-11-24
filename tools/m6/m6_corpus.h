/* M6 entry-state corpus format (files are ROM/RAM-derived: they live under ..\Gamefiles\Movies\m6\corpus only).
 *
 * One file per routine, b<bank>_<addr>.m6c:
 *   header  "M6C1", u32 count, u16 addr, u8 bank, u8 reserved
 *   count records:
 *     u8 a, x, y, s, p, bank        entry registers (S after the JSR) and mapped PRG bank
 *     u8 out_a, out_x, out_y, out_p registers at the routine's final RTS (live run)
 *     u16 n_acc                     hardware accesses between entry and return, in order
 *     u32 out_ram_hash              FNV-1a of the 2 KB RAM after the return, stack page below S excluded
 *     u8 ram[2048]                  RAM at entry
 *     n_acc x { u16 addr; u8 value; u8 kind }   kind 1 = read, 2 = write ($2000-$7FFF reads, $2000+ writes)
 * All integers little-endian.
 */
#ifndef BTDD_M6_CORPUS_H
#define BTDD_M6_CORPUS_H
#include <stdint.h>

#define M6_MAX_ACC 4096
enum { M6_READ = 1, M6_WRITE = 2 };

typedef struct M6Acc {
    uint16_t addr;
    uint8_t value, kind;
} M6Acc;

typedef struct M6Sample {
    uint8_t a, x, y, s, p, bank;
    uint8_t out_a, out_x, out_y, out_p;
    uint16_t n_acc;
    uint32_t out_ram_hash;
    uint8_t ram[2048];
    M6Acc acc[M6_MAX_ACC];
} M6Sample;

/* RAM hash used for out_ram_hash: skips the free stack area $0100..$0100+s (below the stack pointer) */
static inline uint32_t m6_ram_hash(const uint8_t *ram, uint8_t s)
{
    uint32_t h = 2166136261u;
    int i;
    for (i = 0; i < 2048; i++) {
        if (i >= 0x100 && i <= 0x100 + s)
            continue;
        h = (h ^ ram[i]) * 16777619u;
    }
    return h;
}

#endif
