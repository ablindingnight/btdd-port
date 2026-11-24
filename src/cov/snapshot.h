/* Snapshots: the whole emulated console (CPU, PPU, RAM, CHR-RAM, mapper, controller state) at a frame
 * boundary, in our own format. A file is a 32-byte header followed by the Nes struct with every pointer
 * cleared, so it is tied to the build's struct layout: the header stores sizeof(Nes) and a layout
 * fingerprint, and loading refuses a mismatch (re-record the snapshots after changing the core).
 *
 * Header: "BTDDSNP1", u32 struct size, u32 layout fingerprint, u32 frame, u8 level ($000D),
 * u8 section ($0015), u16 reason flags, u32 reserved, u32 reserved (all little-endian).
 */
#ifndef BTDD_SNAPSHOT_H
#define BTDD_SNAPSHOT_H
#include "nes/nes.h"
#include <stdint.h>

enum {
    SNAP_PERIODIC = 1,  /* every N frames */
    SNAP_LEVEL    = 2,  /* $000D changed */
    SNAP_SECTION  = 4,  /* $0015 changed */
    SNAP_LOCK     = 8,  /* camera stopped after moving */
    SNAP_UNLOCK   = 16, /* camera started moving after standing still */
};

typedef struct SnapInfo {
    uint32_t frame;
    uint8_t level, section;
    uint16_t reason;
} SnapInfo;

/* In-memory copy (exploration branches from these without touching the disk). */
void snap_copy(Nes *dst, const Nes *src);      /* dst keeps its own pointers/hooks */

int snap_save(const Nes *nes, const SnapInfo *info, const char *path);
int snap_load(Nes *nes, SnapInfo *info, const char *path); /* nes must be nes_init'ed with the ROM */
const char *snap_reason_text(uint16_t reason, char *buf, int len);

#endif
