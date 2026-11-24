/* Mod "nolimit": no sprite flicker (display mod). The NES shows at most 8 sprites per scanline; the game
 * rotates its sprite order every frame so that, where more than 8 share a line, different ones drop out each
 * frame (flicker). With this mod the picture is built with every sprite on every line (src/platform/screen.c),
 * so nothing drops out. Emulation is untouched: the PPU still evaluates 8 per line (sprite overflow flag,
 * sprite-0 hit), and the game's own rotation still runs; it now only changes which of two overlapping sprites
 * is drawn on top. */
#include "mods.h"

const ModDef mod_nolimit = {
    "nolimit",
    "No sprite flicker",
    "draw every sprite on every line (no 8-per-line limit); picture only, game logic untouched",
    MOD_DISPLAY,
    0, 0, 0,
    NULL,
    0,
};
