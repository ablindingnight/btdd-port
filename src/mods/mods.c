/* Mod registry and hook dispatch (see mods.h). */
#include "mods.h"
#include <stdlib.h>
#include <string.h>

/* ---- the built-in mods (mod_*.c) ---- */
extern const ModDef mod_lives;
extern const ModDef mod_nolimit;
extern const ModDef mod_widescreen, mod_widesprites, mod_widehud, mod_widefade;
extern const ModDef mod_cheat_lives5, mod_cheat_code1, mod_cheat_code2;
#define MAX_MODS 32
static const ModDef *all[MAX_MODS] = {&mod_nolimit, &mod_lives, &mod_widescreen, &mod_widesprites, &mod_widehud,
                                      &mod_widefade, &mod_cheat_lives5, &mod_cheat_code1, &mod_cheat_code2};
static int n_mods = 9; /* built-in mods first, then mods_add() */

static int enabled[MAX_MODS];
static int values[MAX_MODS];
static int values_set;

static void init_values(void)
{
    int i;
    if (values_set)
        return;
    for (i = 0; i < n_mods; i++)
        values[i] = all[i]->value_default;
    values_set = 1;
}

int mods_add(const ModDef *def)
{
    if (n_mods == MAX_MODS || mods_find(def->id) >= 0)
        return -1;
    all[n_mods] = def;
    values[n_mods] = def->value_default;
    return n_mods++;
}

int mods_count(void)
{
    return n_mods;
}

const ModDef *mods_def(int i)
{
    return i >= 0 && i < n_mods ? all[i] : NULL;
}

int mods_find(const char *id)
{
    int i;
    for (i = 0; i < n_mods; i++)
        if (!strcmp(all[i]->id, id))
            return i;
    return -1;
}

void mods_enable(int i, int on)
{
    if (i >= 0 && i < n_mods)
        enabled[i] = on != 0;
}

int mods_enabled(int i)
{
    return i >= 0 && i < n_mods && enabled[i];
}

void mods_set_value(int i, int v)
{
    init_values();
    if (i < 0 || i >= n_mods)
        return;
    if (v < all[i]->value_min)
        v = all[i]->value_min;
    if (v > all[i]->value_max)
        v = all[i]->value_max;
    values[i] = v;
}

int mods_value(int i)
{
    init_values();
    return i >= 0 && i < n_mods ? values[i] : 0;
}

int mods_display_flags(void)
{
    int i, f = 0;
    for (i = 0; i < n_mods; i++)
        if (enabled[i]) {
            if (all[i] == &mod_nolimit)
                f |= MODS_DISPLAY_NO_SPRITE_LIMIT;
            else if (all[i] == &mod_widescreen)
                f |= MODS_DISPLAY_WIDESCREEN;
            else if (all[i] == &mod_widesprites)
                f |= MODS_DISPLAY_WIDE_SPRITES;
            else if (all[i] == &mod_widehud)
                f |= MODS_DISPLAY_WIDE_HUD;
            else if (all[i] == &mod_widefade)
                f |= MODS_DISPLAY_WIDE_FADE;
        }
    return f;
}

/* ---- hook dispatch ---- */

enum { MAP_ENTRY = 1, MAP_PENDING = 2 };
#define MAX_PENDING 32

typedef struct Pending {
    uint16_t pc;   /* return address */
    uint8_t s;     /* S once the routine has returned */
    uint8_t bank;  /* bank at entry */
    const ModHook *hook;
    int value;
} Pending;

static uint8_t map[0x10000];
static Pending pending[MAX_PENDING];
static int pending_count;
static uint64_t calls;

uint64_t mods_hook_calls(void)
{
    return calls;
}

static void remark(uint16_t pc)
{
    int i;
    map[pc] &= (uint8_t)~MAP_PENDING;
    for (i = 0; i < pending_count; i++)
        if (pending[i].pc == pc)
            map[pc] |= MAP_PENDING;
}

static void drop_pending(int i)
{
    uint16_t pc = pending[i].pc;
    memmove(&pending[i], &pending[i + 1], (size_t)(pending_count - i - 1) * sizeof pending[0]);
    pending_count--;
    remark(pc);
}

void mods_forget_pending(void)
{
    while (pending_count)
        drop_pending(pending_count - 1);
}

static uint16_t return_address(Nes *n, const Cpu *c)
{
    uint8_t lo = n->ram[0x100 | (uint8_t)(c->s + 1)], hi = n->ram[0x100 | (uint8_t)(c->s + 2)];
    return (uint16_t)((lo | hi << 8) + 1);
}

static int on_pc(void *ctx, Cpu *c)
{
    Nes *n = ctx;
    uint16_t pc = c->pc;
    int i, h;

    if (map[pc] & MAP_PENDING) { /* a hooked routine has returned here? newest frame first */
        for (i = pending_count - 1; i >= 0; i--) {
            Pending p = pending[i];
            if (p.pc == pc && p.s == c->s && p.bank == n->prg_bank) {
                drop_pending(i);
                calls++;
                p.hook->fn(n, c, p.value);
            }
        }
    }
    if (!(map[pc] & MAP_ENTRY))
        return 0;
    for (i = 0; i < n_mods; i++) {
        if (!enabled[i] || all[i]->kind != MOD_LOGIC)
            continue;
        for (h = 0; h < all[i]->hook_count; h++) {
            const ModHook *k = &all[i]->hooks[h];
            if (k->addr != pc || k->bank != n->prg_bank)
                continue;
            if (k->kind == MOD_BEFORE) {
                calls++;
                k->fn(n, c, values[i]);
            } else if (k->kind == MOD_AFTER) {
                if (pending_count == MAX_PENDING)
                    drop_pending(0); /* oldest: a routine that never returned (stack unwound) */
                pending[pending_count].pc = return_address(n, c);
                pending[pending_count].s = (uint8_t)(c->s + 2);
                pending[pending_count].bank = n->prg_bank;
                pending[pending_count].hook = k;
                pending[pending_count].value = values[i];
                map[pending[pending_count].pc] |= MAP_PENDING;
                pending_count++;
            } else { /* MOD_REPLACE: run ours, then return to the caller like RTS */
                calls++;
                k->fn(n, c, values[i]);
                c->pc = return_address(n, c);
                c->s = (uint8_t)(c->s + 2);
                cpu_add_cycles(c, k->cycles);
                return 1;
            }
        }
    }
    return 0;
}

void mods_attach(Nes *nes)
{
    int i, h, any = 0;
    init_values();
    mods_forget_pending();
    for (i = 0; i < 0x10000; i++)
        map[i] = 0;
    for (i = 0; i < n_mods; i++) {
        if (!enabled[i] || all[i]->kind != MOD_LOGIC)
            continue;
        for (h = 0; h < all[i]->hook_count; h++) {
            map[all[i]->hooks[h].addr] |= MAP_ENTRY;
            any = 1;
        }
    }
    nes->cpu.pc_hook_map = any ? map : NULL;
    nes->cpu.pc_hook = any ? on_pc : NULL;
}

/* ---- settings ---- */

static int parse_value(const ModDef *d, const char *v, int *on, int *value)
{
    char *end;
    long x;
    if (!strcmp(v, "on") || !strcmp(v, "yes") || !strcmp(v, "true")) {
        *on = 1;
        return 0;
    }
    if (!strcmp(v, "off") || !strcmp(v, "no") || !strcmp(v, "false") || !v[0]) {
        *on = 0;
        return 0;
    }
    x = strtol(v, &end, 10);
    if (*end)
        return -1;
    if (d->value_min == d->value_max) { /* on/off mod: 0 = off, anything else = on */
        *on = x != 0;
        return 0;
    }
    *on = 1;
    *value = (int)x;
    return 1;
}

int mods_apply_setting(const char *key, const char *value)
{
    int i, on = 0, v = 0, r;
    if (strncmp(key, "mod_", 4) != 0)
        return 0;
    i = mods_find(key + 4);
    if (i < 0)
        return 1; /* a mod this build does not have: ignored */
    r = parse_value(all[i], value, &on, &v);
    if (r < 0)
        return 1;
    mods_enable(i, on);
    if (r == 1)
        mods_set_value(i, v);
    return 1;
}

int mods_apply_arg(const char *arg)
{
    char key[80];
    const char *eq = strchr(arg, '=');
    size_t len = eq ? (size_t)(eq - arg) : strlen(arg);
    if (len > sizeof key - 5)
        return -1;
    memcpy(key, "mod_", 4);
    memcpy(key + 4, arg, len);
    key[4 + len] = 0;
    if (mods_find(key + 4) < 0)
        return -1;
    mods_apply_setting(key, eq ? eq + 1 : "on");
    return 0;
}

void mods_write_settings(FILE *f)
{
    int i;
    fprintf(f, "# mods (docs/M7_MODS.md): mod_<id> = on | off, or a number for mods that take one\n");
    for (i = 0; i < n_mods; i++) {
        const ModDef *d = all[i];
        fprintf(f, "# %s: %s\n", d->id, d->help);
        if (d->value_min == d->value_max)
            fprintf(f, "mod_%s = %s\n", d->id, enabled[i] ? "on" : "off");
        else if (enabled[i])
            fprintf(f, "mod_%s = %d\n", d->id, mods_value(i));
        else
            fprintf(f, "mod_%s = off\n", d->id);
    }
}

const char *mods_describe(char *buf, size_t len)
{
    int i;
    size_t n = 0;
    buf[0] = 0;
    for (i = 0; i < n_mods && n + 1 < len; i++) {
        if (!enabled[i])
            continue;
        if (all[i]->value_min == all[i]->value_max)
            n += (size_t)snprintf(buf + n, len - n, "%s%s", n ? ", " : "", all[i]->id);
        else
            n += (size_t)snprintf(buf + n, len - n, "%s%s=%d", n ? ", " : "", all[i]->id, mods_value(i));
        if (n >= len) { /* truncated */
            buf[len - 1] = 0;
            break;
        }
    }
    if (!buf[0])
        snprintf(buf, len, "none");
    return buf;
}
