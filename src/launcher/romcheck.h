/* What is wrong with a picked ROM, in plain words, with what to do about it (first-run checklist, --rom-check).
 * Never points to download sites: the player uses their own cartridge dump. */
#ifndef BTDD_LAUNCHER_ROMCHECK_H
#define BTDD_LAUNCHER_ROMCHECK_H
#include <stddef.h>

typedef enum RomCheck {
    RC_OK, RC_OPEN, RC_EMPTY, RC_NOT_NES, RC_NO_HEADER, RC_INCOMPLETE, RC_EXTRA_DATA, RC_OTHER_GAME,
    RC_OTHER_VERSION, RC_ZIP_EMPTY, RC_ZIP_MANY, RC_ZIP_BAD, RC_COUNT
} RomCheck;

/* checks a .nes file or a .zip holding one; title/hint get the player-facing words */
RomCheck romcheck_file(const char *path, char *title, size_t tlen, char *hint, size_t hlen);
const char *romcheck_code(RomCheck c); /* short id for tests, e.g. "no-header" */

#endif
