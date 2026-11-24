/* M7: mods. Our own C code that changes or adds behaviour, attached to the game at ROM routine addresses.
 *
 * A logic mod lists hooks by (PRG bank, CPU address) of a routine entry:
 *   MOD_BEFORE   fn runs when the routine is entered, then the original runs;
 *   MOD_AFTER    fn runs when the routine has returned to its caller (S back to the entry level + 2), before the
 *                caller's next instruction;
 *   MOD_REPLACE  fn runs instead of the routine, which then returns to its caller as if by RTS; `cycles` CPU
 *                cycles are charged in place of the original's.
 * Hooks fire through Cpu.pc_hook at instruction start, in the interpreter and in native (M5) dispatch alike
 * (native code returns to the run loop at every routine entry and return). Hooked addresses must be routine
 * entries or return points, not the middle of a routine (native code runs straight through those).
 * A display mod changes only how the picture is built (no hooks; see screen.c).
 *
 * Mods are original code written for this port; they never contain translated game code. With no logic mod
 * enabled, Cpu.pc_hook_map stays NULL: emulation is exactly the unmodded one.
 *
 * One console per process (the hook state is global), like the rest of the platform layer. */
#ifndef BTDD_MODS_H
#define BTDD_MODS_H
#include "nes/nes.h"
#include <stdio.h>

typedef enum ModHookKind { MOD_BEFORE, MOD_AFTER, MOD_REPLACE } ModHookKind;
typedef enum ModKind { MOD_LOGIC, MOD_DISPLAY } ModKind;

typedef struct ModHook {
    uint8_t bank;
    uint16_t addr;
    ModHookKind kind;
    void (*fn)(Nes *nes, Cpu *cpu, int value); /* value = the mod's setting */
    int cycles;                                /* MOD_REPLACE: CPU cycles charged */
} ModHook;

typedef struct ModDef {
    const char *id;   /* config key is mod_<id> */
    const char *name; /* short title for menus / the overlay */
    const char *help;
    ModKind kind;
    int value_default, value_min, value_max; /* value_min == value_max: no value, just on/off */
    const ModHook *hooks;
    int hook_count;
} ModDef;

enum {
    MODS_DISPLAY_NO_SPRITE_LIMIT = 1,
    MODS_DISPLAY_WIDESCREEN = 2,   /* M9: src/platform/wide.c */
    MODS_DISPLAY_WIDE_SPRITES = 4,
    MODS_DISPLAY_WIDE_HUD = 8,
    MODS_DISPLAY_WIDE_FADE = 16,
};

int mods_count(void);
/* Adds a mod defined elsewhere (tests, future plugin code); returns its index or -1. */
int mods_add(const ModDef *def);
const ModDef *mods_def(int i);
int mods_find(const char *id);           /* index or -1 */
void mods_enable(int i, int on);
int mods_enabled(int i);
void mods_set_value(int i, int value);   /* clamped to the mod's range */
int mods_value(int i);

/* Installs (or removes, if no logic mod is on) the hooks on nes->cpu. Call after nes_power and after
 * every change of the enabled set. */
void mods_attach(Nes *nes);
/* Drops pending "after" hooks (call after loading a state or rewinding: their routine frames are gone). */
void mods_forget_pending(void);
int mods_display_flags(void);            /* MODS_DISPLAY_* of the enabled display mods */
uint64_t mods_hook_calls(void);          /* hook functions run so far (tests) */

/* Settings: "mod_<id> = on | off | <number>" (a number turns the mod on with that value). Returns 1 if the
 * key was a mod key. */
int mods_apply_setting(const char *key, const char *value);
/* "id" or "id=value" (command lines). Returns 0 if the mod exists. */
int mods_apply_arg(const char *arg);
void mods_write_settings(FILE *f);
/* "lives=5, nolimit" or "none" */
const char *mods_describe(char *buf, size_t len);

#endif
