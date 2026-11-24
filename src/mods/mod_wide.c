/* M9 widescreen display mods (picture only; game logic untouched; src/platform/wide.c does the drawing):
 *   widescreen  - 424x240 picture: 84 extra columns on each side drawn from the level's block map
 *   widesprites - also draw the entities the game culls outside its 256-pixel window (its "cull" turned off for
 *                 the picture only: the game still sets its off-screen flag and its AI still sees it)
 *   widehud     - move each player's half of the status bar out to the edges of the wide picture
 *   widefade    - darken the outer margins, so enemies appearing (spawns are camera-triggered) or vanishing
 *                 (despawns behind the left edge) near the outer edges are less noticeable
 * The three options only matter while widescreen is on. */
#include "mods.h"

const ModDef mod_widescreen = {
    "widescreen", "Widescreen (16:9)",
    "424x240: extra columns from the level map where the stage allows it (black where not)",
    MOD_DISPLAY, 0, 0, 0, NULL, 0,
};

const ModDef mod_widesprites = {
    "widesprites", "Widescreen sprites",
    "draw enemies and players outside the original 256-pixel window too",
    MOD_DISPLAY, 0, 0, 0, NULL, 0,
};

const ModDef mod_widehud = {
    "widehud", "Widescreen status bar",
    "move each player's status bar to the edges of the wide picture",
    MOD_DISPLAY, 0, 0, 0, NULL, 0,
};

const ModDef mod_widefade = {
    "widefade", "Widescreen edge fade",
    "darken the outer margins (hides enemies appearing or vanishing at the edges)",
    MOD_DISPLAY, 0, 0, 0, NULL, 0,
};
