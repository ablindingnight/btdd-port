/* Machine state: everything that determines what the console does next (CPU, RAM, PPU registers and
 * memory incl. CHR-RAM/OAM/palette and the new-PPU model's internals, APU, mapper bank, controllers),
 * taken at a frame boundary. Used by save states, rewind and the coverage snapshots.
 *
 * The state is the Nes struct with the host's fields normalized (function pointers, hook users, the
 * picture buffer fb, debug watches, the APU's output buffer): fb is only read for the line being drawn, so
 * it carries nothing across a frame boundary. The state is therefore tied to this build's struct layout;
 * files carry a format version, sizeof(Nes) and a layout fingerprint, and loading refuses a mismatch.
 *
 * File: 80-byte header, then the normalized Nes (all little-endian):
 *   "BTDDSTAT", u32 version, u32 sizeof(Nes), u32 layout fingerprint, u32 reserved, u64 frame,
 *   u8 PPU model, u8 APU on, 6 reserved, ROM SHA-1 as 40 hex chars, 8 reserved.
 */
#ifndef BTDD_STATE_H
#define BTDD_STATE_H
#include "nes.h"
#include <stddef.h>
#include <stdint.h>

#define NES_STATE_VERSION 1
#define NES_STATE_HEADER 80

enum {
    NES_STATE_OK = 0,
    NES_STATE_IO = -1,      /* cannot open/read/write the file */
    NES_STATE_FORMAT = -2,  /* not a state file, other version, or another build's layout */
    NES_STATE_OTHER_ROM = -3,
};

/* Size of a state blob (the normalized Nes, without the file header). */
size_t nes_state_size(void);
/* Copies the machine state of `nes` into blob (nes_state_size() bytes); host fields are zeroed. */
void nes_state_capture(const Nes *nes, uint8_t *blob);
/* Replaces the machine state of `nes` with blob, keeping nes's host fields (hooks, buffers, picture). */
void nes_state_restore(Nes *nes, const uint8_t *blob);

/* Low-level helpers (also used by cov/snapshot.c): zero the host fields of a copy; copy the host fields
 * of `host` into `n` (APU: if either side has no APU attached, n keeps host's whole APU). */
void nes_state_clear_host(Nes *n);
void nes_state_adopt_host(Nes *n, const Nes *host);
uint32_t nes_state_fingerprint(void);

int nes_state_save(const Nes *nes, uint64_t frame, const char *rom_sha1, const char *path);
/* rom_sha1 may be NULL to skip the ROM check. */
int nes_state_load(Nes *nes, uint64_t *frame, const char *rom_sha1, const char *path);
const char *nes_state_error(int code);

#endif
