/* Mod "lives": start with N lives instead of 3 (logic mod, wrap hook).
 *
 * The game gives a player their starting lives in one routine, bank 6 $9888 (decomp sub_9888, called with
 * Y = player 0/1 when a player starts or continues; it stores the count into player_lives,y = $0011+Y).
 * We let it run unchanged and, after it returns, store our own count there. The routine leaves Y alone, so Y
 * still names the player when the after-hook runs. Our code, not a translation: the game's own rules for the
 * count (its button code, the $0017 minimum) simply get overridden. */
#include "mods.h"

#define PLAYER_LIVES 0x0011 /* player_lives (meta/btdd.meta), one byte per player */

static void set_lives(Nes *nes, Cpu *cpu, int value)
{
    nes->ram[PLAYER_LIVES + (cpu->y & 1)] = (uint8_t)value;
}

static const ModHook hooks[] = {
    {6, 0x9888, MOD_AFTER, set_lives, 0},
};

const ModDef mod_lives = {
    "lives",
    "Starting lives",
    "start (and continue) with N lives instead of 3 (1-9)",
    MOD_LOGIC,
    5, 1, 9,
    hooks,
    (int)(sizeof hooks / sizeof hooks[0]),
};
