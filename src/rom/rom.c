#include "rom.h"
#include "sha1.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define INES_HEADER 16
#define ROM_FILE_SIZE (INES_HEADER + ROM_BANKS * ROM_BANK_SIZE)

/* Battletoads-Double Dragon (USA).nes, whole file including the iNES header. */
static const char USA_SHA1[] = "0218a66c6ee9887ec9115213a2f5b64055d79599";

RomResult rom_load(const char *path, Rom *rom, char sha1_hex[41])
{
    uint8_t *buf, digest[20];
    long size;
    int i;
    FILE *f = fopen(path, "rb");

    if (!f)
        return ROM_ERR_OPEN;
    fseek(f, 0, SEEK_END);
    size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size != ROM_FILE_SIZE) {
        fclose(f);
        return ROM_ERR_SIZE;
    }
    buf = malloc(ROM_FILE_SIZE);
    if (!buf || fread(buf, 1, ROM_FILE_SIZE, f) != ROM_FILE_SIZE) {
        free(buf);
        fclose(f);
        return ROM_ERR_OPEN;
    }
    fclose(f);

    if (memcmp(buf, "NES\x1A", 4) != 0 || buf[4] != ROM_BANKS * 2 || buf[5] != 0 ||
        ((buf[6] >> 4) | (buf[7] & 0xF0)) != 7) {
        free(buf);
        return ROM_ERR_HEADER;
    }

    sha1(buf, ROM_FILE_SIZE, digest);
    for (i = 0; i < 20; i++)
        sprintf(sha1_hex + 2 * i, "%02x", digest[i]);
    if (strcmp(sha1_hex, USA_SHA1) != 0) {
        free(buf);
        return ROM_ERR_HASH;
    }

    memcpy(rom->prg, buf + INES_HEADER, ROM_BANKS * ROM_BANK_SIZE);
    free(buf);
    return ROM_OK;
}

int rom_is_usa(const uint8_t *file, size_t len)
{
    uint8_t digest[20];
    char hex[41];
    int i;
    if (len != ROM_FILE_SIZE)
        return 0;
    sha1(file, len, digest);
    for (i = 0; i < 20; i++)
        sprintf(hex + 2 * i, "%02x", digest[i]);
    return strcmp(hex, USA_SHA1) == 0;
}

const char *rom_result_text(RomResult r)
{
    switch (r) {
    case ROM_OK:         return "ok";
    case ROM_ERR_OPEN:   return "cannot open or read the file";
    case ROM_ERR_SIZE:   return "wrong file size (expected 262,160 bytes)";
    case ROM_ERR_HEADER: return "not an iNES mapper-7 ROM with 256 KB PRG and no CHR";
    case ROM_ERR_HASH:   return "not the supported dump: Battletoads-Double Dragon (USA)";
    }
    return "unknown error";
}
