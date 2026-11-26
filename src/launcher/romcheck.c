#include "romcheck.h"
#include "rom/rom.h"
#include "unzip.h"
#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *const CODES[RC_COUNT] = {"ok", "open", "empty", "not-nes", "no-header", "incomplete", "extra-data",
                                            "other-game", "other-version", "zip-empty", "zip-many", "zip-bad"};
static const char *const TITLES[RC_COUNT] = {
    "USA cartridge, verified",
    "We can't open that file.",
    "That file is empty.",
    "That isn't an NES ROM.",
    "This dump is missing its 16-byte NES header.",
    "This dump is too small: it looks incomplete.",
    "This dump has extra data in it.",
    "This is a different NES game.",
    "Right game, but not the USA version (or a changed copy).",
    "That .zip has no .nes file inside.",
    "That .zip holds several .nes files.",
    "We can't read that .zip.",
};
static const char *const HINTS[RC_COUNT] = {
    "",
    "Make sure it has finished copying, then pick it again.",
    "Copy your ROM again from where you keep your backups.",
    "Pick the .nes file of your cartridge (or a .zip with it inside).",
    "Dump the cartridge again with a tool that saves .nes files with a header.",
    "Copy or dump it again: the right file is 262,160 bytes.",
    "Use a clean dump of 262,160 bytes (no trainer, nothing added at the end).",
    "Pick your Battletoads & Double Dragon (USA) ROM.",
    "Only the USA version works for now (Europe is planned); patched copies don't.",
    "Pick the .zip that holds your ROM, or unzip it and pick the .nes.",
    "Unzip it (right-click, Extract All) and pick the USA .nes file.",
    "Unzip it (right-click, Extract All) and pick the .nes file.",
};

const char *romcheck_code(RomCheck c) { return c < RC_COUNT ? CODES[c] : "?"; }

static RomCheck check_bytes(const unsigned char *b, size_t n)
{
    int header = n >= 16 && !memcmp(b, "NES\x1a", 4);
    if (n == 0)
        return RC_EMPTY;
    if (!header)
        return n == ROM_FILE_BYTES - 16 ? RC_NO_HEADER : RC_NOT_NES;
    {
        int prg = b[4], chr = b[5], mapper = (b[6] >> 4) | (b[7] & 0xF0);
        int layout = prg == ROM_BANKS * 2 && chr == 0 && mapper == 7; /* this game's board */
        if (!layout)
            return RC_OTHER_GAME;
        if (n < ROM_FILE_BYTES)
            return RC_INCOMPLETE;
        if (n > ROM_FILE_BYTES || (b[6] & 4))
            return RC_EXTRA_DATA;
        return rom_is_usa(b, n) ? RC_OK : RC_OTHER_VERSION;
    }
}

RomCheck romcheck_file(const char *path, char *title, size_t tlen, char *hint, size_t hlen)
{
    size_t n = 0, pl = strlen(path);
    unsigned char *buf = (unsigned char *)SDL_LoadFile(path, &n);
    RomCheck r;
    if (!buf)
        r = RC_OPEN;
    else if (pl > 4 && !SDL_strcasecmp(path + pl - 4, ".zip")) {
        int count = unzip_count_nes(buf, n);
        if (count < 0)
            r = RC_ZIP_BAD;
        else if (count == 0)
            r = RC_ZIP_EMPTY;
        else if (count > 1)
            r = RC_ZIP_MANY;
        else {
            uint8_t *rom = NULL;
            size_t rn = 0;
            char name[200];
            r = unzip_first_nes(buf, n, &rom, &rn, name, sizeof name) == 0 ? check_bytes(rom, rn) : RC_ZIP_BAD;
            free(rom);
        }
    } else
        r = check_bytes(buf, n);
    SDL_free(buf);
    if (title)
        snprintf(title, tlen, "%s", TITLES[r]);
    if (hint)
        snprintf(hint, hlen, "%s", HINTS[r]);
    return r;
}
