#ifndef BTDD_ROM_H
#define BTDD_ROM_H
#include <stdint.h>

#define ROM_BANKS     8
#define ROM_BANK_SIZE 0x8000

typedef struct Rom {
    uint8_t prg[ROM_BANKS][ROM_BANK_SIZE]; /* each bank is mapped whole at CPU $8000-$FFFF (AxROM) */
} Rom;

typedef enum RomResult {
    ROM_OK = 0,
    ROM_ERR_OPEN,
    ROM_ERR_SIZE,
    ROM_ERR_HEADER,
    ROM_ERR_HASH, /* a readable NES file, but not the supported USA dump */
} RomResult;

/* Loads and verifies the user's ROM. On ROM_ERR_HASH, sha1_hex (41 bytes) still receives the file's hash. */
RomResult rom_load(const char *path, Rom *rom, char sha1_hex[41]);
const char *rom_result_text(RomResult r);

/* Reads a CPU address in $8000-$FFFF while `bank` is mapped. */
static inline uint8_t rom_read(const Rom *rom, int bank, uint16_t addr)
{
    return rom->prg[bank & 7][addr & 0x7FFF];
}

#endif
