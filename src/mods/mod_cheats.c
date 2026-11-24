/* Cheats: the game's own codes, entered for the player (docs/GAME_FACTS.md, "Secrets, cheats and quirks"). Nothing
 * here writes lives: the hooks only make the game's own checks see the code's buttons, so the result is exactly what
 * the real code does (including its confirmation sound and its rules for who gets the lives).
 *
 *   cheat_lives5  - the held code: when a player's lives are handed out (bank 6 sub_9888, Y = player), that player's
 *                   held buttons read as Up+A+B+Start for the routine only -> 5 lives instead of 3.
 *   cheat_code1   - B, A, Down, B, Up, Down entered on the screen before stage 1 (bank 6 check_cheat_codes, $9809):
 *                   each call of the checker sees the next button of the sequence as a new press -> $17 = 5, and
 *                   every player who joins later (second player, continue) gets 5 lives.
 *   cheat_code2   - Down, Up, Up, Down, A, B, B, A the same way -> $17 = 10, players joining later get 10 lives.
 * The injected buttons are put back right after the routine (after-hooks), so nothing else sees them. */
#include "mods.h"

/* the game's button bits in joy_held / joy_pressed ($30-$33) */
enum { G_RIGHT = 0x01, G_LEFT = 0x02, G_DOWN = 0x04, G_UP = 0x08, G_START = 0x10, G_SELECT = 0x20, G_B = 0x40, G_A = 0x80 };

#define JOY_HELD 0x30
#define JOY_PRESSED 0x32
#define START_LIVES_BONUS 0x17 /* $17: set by the sequence codes */
#define SEQ1_POS 0x0747        /* the checker's position in sequence 1 / 2 */
#define SEQ2_POS 0x0748

static const uint8_t seq1[] = {G_B, G_A, G_DOWN, G_B, G_UP, G_DOWN};
static const uint8_t seq2[] = {G_DOWN, G_UP, G_UP, G_DOWN, G_A, G_B, G_B, G_A};

/* ---- held code ---- */
static uint8_t held_saved[2];

static void lives5_before(Nes *n, Cpu *c, int value)
{
    int p = c->y & 1;
    (void)value;
    held_saved[p] = n->ram[JOY_HELD + p];
    n->ram[JOY_HELD + p] = G_UP | G_A | G_B | G_START;
}

static void lives5_after(Nes *n, Cpu *c, int value)
{
    (void)value;
    n->ram[JOY_HELD + (c->y & 1)] = held_saved[c->y & 1];
}

/* ---- sequence codes ---- */
static uint8_t pressed_saved[2];
static int saved_valid; /* both codes on: save once, restore once */

static void feed(Nes *n, const uint8_t *seq, int len, int pos_addr)
{
    int pos = n->ram[pos_addr];
    if (!saved_valid) {
        pressed_saved[0] = n->ram[JOY_PRESSED];
        pressed_saved[1] = n->ram[JOY_PRESSED + 1];
        saved_valid = 1;
    }
    if (n->ram[START_LIVES_BONUS] || pos >= len) /* the checker stops once $17 is set */
        return;
    n->ram[JOY_PRESSED] = seq[pos]; /* the next button, as a new press on pad 1 */
    n->ram[JOY_PRESSED + 1] = 0;
}

static void code1_before(Nes *n, Cpu *c, int value)
{
    (void)c;
    (void)value;
    feed(n, seq1, (int)sizeof seq1, SEQ1_POS);
}

static void code2_before(Nes *n, Cpu *c, int value)
{
    (void)c;
    (void)value;
    feed(n, seq2, (int)sizeof seq2, SEQ2_POS);
}

static void code_after(Nes *n, Cpu *c, int value)
{
    (void)c;
    (void)value;
    if (!saved_valid)
        return;
    n->ram[JOY_PRESSED] = pressed_saved[0];
    n->ram[JOY_PRESSED + 1] = pressed_saved[1];
    saved_valid = 0;
}

static const ModHook lives5_hooks[] = {
    {6, 0x9888, MOD_BEFORE, lives5_before, 0},
    {6, 0x9888, MOD_AFTER, lives5_after, 0},
};
static const ModHook code1_hooks[] = {
    {6, 0x9809, MOD_BEFORE, code1_before, 0},
    {6, 0x9809, MOD_AFTER, code_after, 0},
};
static const ModHook code2_hooks[] = {
    {6, 0x9809, MOD_BEFORE, code2_before, 0},
    {6, 0x9809, MOD_AFTER, code_after, 0},
};

const ModDef mod_cheat_lives5 = {
    "cheat_lives5", "Cheat: 5 lives",
    "the game's held code (Up+A+B on Start) for every player: start with 5 lives",
    MOD_LOGIC, 0, 0, 0, lives5_hooks, 2,
};

const ModDef mod_cheat_code1 = {
    "cheat_code1", "Cheat: code 1",
    "enters B,A,Down,B,Up,Down for you: players joining later get 5 lives",
    MOD_LOGIC, 0, 0, 0, code1_hooks, 2,
};

const ModDef mod_cheat_code2 = {
    "cheat_code2", "Cheat: code 2",
    "enters Down,Up,Up,Down,A,B,B,A for you: players joining later get 10 lives",
    MOD_LOGIC, 0, 0, 0, code2_hooks, 2,
};
