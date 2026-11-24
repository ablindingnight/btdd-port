/* Per-mod checks (M7). Exit code 0 = all passed.
 *
 *   mods_test ROM 2P.fm2 1P.fm2 [--frames N]
 *
 * lives:   off -> both players start with 3 lives; lives=7 -> both start with 7 (after-hook on bank 6 $9888), and
 *          the hook fired once per player.
 * nolimit: two consoles run each movie in lockstep, one built with the mod: RAM identical on every frame (the
 *          mod is display only); on the old model our 64-sprite evaluation never disagrees with the PPU's 8; the
 *          pictures differ only on lines that have more than 8 sprites, and only by added sprite pixels. */
#include "mods/mods.h"
#include "nes/nes.h"
#include "platform/screen.h"
#include "platform/wide.h"
#include "rom/rom.h"
#include "runtime/fm2.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static Rom rom;
static int failed;

static void check(int ok, const char *what)
{
    printf("%s: %s\n", ok ? "ok  " : "FAIL", what);
    failed |= !ok;
}

static void run_to(Nes *n, const Fm2 *m, long *frame, long until)
{
    for (; *frame < until && *frame < m->count; (*frame)++) {
        nes_set_pads(n, m->frames[*frame].pad[0], m->frames[*frame].pad[1]);
        nes_run_frame(n);
    }
}

static void test_lives(const Fm2 *m2p)
{
    static Nes n;
    char text[200];
    long f = 0;
    uint64_t calls0;
    int i = mods_find("lives");

    mods_enable(i, 0);
    nes_init(&n, &rom);
    nes_power(&n, PPU_MODEL_OLD);
    mods_attach(&n);
    check(n.cpu.pc_hook_map == NULL, "lives off: no hook installed");
    run_to(&n, m2p, &f, 800);
    snprintf(text, sizeof text, "lives off: P1/P2 start with %d/%d (game default 3)", n.ram[0x11], n.ram[0x12]);
    check(n.ram[0x11] == 3 && n.ram[0x12] == 3, text);

    mods_apply_arg("lives=7");
    calls0 = mods_hook_calls();
    nes_init(&n, &rom);
    nes_power(&n, PPU_MODEL_OLD);
    mods_attach(&n);
    f = 0;
    run_to(&n, m2p, &f, 800);
    snprintf(text, sizeof text, "lives=7: P1/P2 start with %d/%d, after-hook ran %llu times", n.ram[0x11], n.ram[0x12],
             (unsigned long long)(mods_hook_calls() - calls0));
    check(n.ram[0x11] == 7 && n.ram[0x12] == 7 && mods_hook_calls() - calls0 == 2, text);
    mods_enable(i, 0);
}

/* BEFORE and REPLACE hooks, with test-only mods on the same routine (bank 6 $9888, player start lives) */
static int before_calls;
static void count_call(Nes *n, Cpu *c, int v)
{
    (void)n;
    (void)c;
    (void)v;
    before_calls++;
}
static void give_lives(Nes *n, Cpu *c, int v) /* our own replacement: player Y gets v lives */
{
    n->ram[0x11 + (c->y & 1)] = (uint8_t)v;
}
static const ModHook before_hooks[] = {{6, 0x9888, MOD_BEFORE, count_call, 0}};
static const ModHook replace_hooks[] = {{6, 0x9888, MOD_REPLACE, give_lives, 24}};
static const ModDef test_before = {"t_before", "test", "test", MOD_LOGIC, 0, 0, 0, before_hooks, 1};
static const ModDef test_replace = {"t_replace", "test", "test", MOD_LOGIC, 6, 1, 9, replace_hooks, 1};

static void test_before_replace(const Fm2 *m2p)
{
    static Nes n, ref;
    char text[200];
    long f = 0;
    int ib = mods_add(&test_before), ir = mods_add(&test_replace), il = mods_find("lives");

    /* reference: the lives mod (after-hook) with 6 lives */
    mods_enable(il, 1);
    mods_set_value(il, 6);
    nes_init(&ref, &rom);
    nes_power(&ref, PPU_MODEL_OLD);
    mods_attach(&ref);
    run_to(&ref, m2p, &f, 3000);
    mods_enable(il, 0);

    mods_enable(ib, 1);
    mods_enable(ir, 1);
    nes_init(&n, &rom);
    nes_power(&n, PPU_MODEL_OLD);
    mods_attach(&n);
    f = 0;
    run_to(&n, m2p, &f, 3000);
    snprintf(text, sizeof text, "before-hook ran %d times (2 players start)", before_calls);
    check(before_calls == 2, text);
    snprintf(text, sizeof text,
             "replace-hook (6 lives, original skipped): at frame 3000 lives %d/%d, level $%02X section $%02X; "
             "after-hook run with 6 lives: %d/%d, $%02X $%02X",
             n.ram[0x11], n.ram[0x12], n.ram[0x0D], n.ram[0x15], ref.ram[0x11], ref.ram[0x12], ref.ram[0x0D],
             ref.ram[0x15]);
    check(n.ram[0x11] == ref.ram[0x11] && n.ram[0x12] == ref.ram[0x12] && n.ram[0x0D] == ref.ram[0x0D] &&
              n.ram[0x15] == ref.ram[0x15],
          text);
    mods_enable(ib, 0);
    mods_enable(ir, 0);
}

/* widescreen (M9, display only): over a movie, two consoles in lockstep (one rendered wide, with sprites): RAM
 * identical; the wide picture's middle (columns 8-255) is the game's own picture on every frame; and some frames of
 * the widening stages get margins drawn from the map. */
/* cheats: the game's own codes entered by the cheat mods (2P movie). join = player 2 does not start at frame 680
 * (pad 2 released there) and presses Start at frame 1200 instead, i.e. joins later. */
static void run_cheat(const Fm2 *m, const char *mod, long until, int join, Nes *n)
{
    long f;
    int i;
    for (i = 0; i < mods_count(); i++)
        if (!strncmp(mods_def(i)->id, "cheat_", 6))
            mods_enable(i, 0);
    if (mod)
        mods_enable(mods_find(mod), 1);
    nes_init(n, &rom);
    nes_power(n, PPU_MODEL_OLD);
    mods_attach(n);
    for (f = 0; f < until && f < m->count; f++) {
        uint8_t p1 = m->frames[f].pad[0], p2 = m->frames[f].pad[1];
        if (join && (f == 679 || f == 680))
            p2 = 0;
        if (join && (f == 1200 || f == 1201))
            p2 = PAD_START;
        nes_set_pads(n, p1, p2);
        nes_run_frame(n);
    }
}

static void test_cheats(const Fm2 *m2p)
{
    static Nes n;
    char text[200];

    run_cheat(m2p, NULL, 800, 0, &n);
    snprintf(text, sizeof text, "cheats off: $17 = %d, lives %d/%d", n.ram[0x17], n.ram[0x11], n.ram[0x12]);
    check(n.ram[0x17] == 0 && n.ram[0x11] == 3 && n.ram[0x12] == 3, text);

    run_cheat(m2p, "cheat_lives5", 700, 0, &n);
    snprintf(text, sizeof text, "cheat_lives5 (held code on the Start frame): lives %d/%d", n.ram[0x11], n.ram[0x12]);
    check(n.ram[0x11] == 5 && n.ram[0x12] == 5, text);

    run_cheat(m2p, "cheat_code1", 800, 0, &n);
    snprintf(text, sizeof text, "cheat_code1 (B,A,Down,B,Up,Down fed to the game's checker): $17 = %d", n.ram[0x17]);
    check(n.ram[0x17] == 5, text);

    run_cheat(m2p, "cheat_code2", 800, 0, &n);
    snprintf(text, sizeof text, "cheat_code2 (Down,Up,Up,Down,A,B,B,A): $17 = %d", n.ram[0x17]);
    check(n.ram[0x17] == 10, text);

    run_cheat(m2p, "cheat_code2", 1300, 1, &n);
    snprintf(text, sizeof text, "cheat_code2, player 2 joins at frame 1200: P1 %d lives, P2 %d lives (game rule: joiners get $17)",
             n.ram[0x11], n.ram[0x12]);
    check(n.ram[0x11] == 3 && n.ram[0x12] == 10, text);

    run_cheat(m2p, NULL, 1300, 1, &n);
    snprintf(text, sizeof text, "no cheat, player 2 joins at frame 1200: P2 %d lives", n.ram[0x12]);
    check(n.ram[0x12] == 3, text);
    run_cheat(m2p, NULL, 0, 0, &n); /* all cheat mods off again */
}

static void test_widescreen(const Fm2 *m, PpuModel model, long frames, const char *label)
{
    static Nes a, b;
    static Screen sa, sb;
    static uint8_t widx[PPU_H][WIDE_W];
    static WideState ws;
    char text[300];
    long f, ram_diff = -1, center_bad = -1, widened = 0, margin_entities = 0;

    nes_init(&a, &rom);
    nes_power(&a, model);
    screen_attach(&sa, &a.ppu);
    nes_init(&b, &rom);
    nes_power(&b, model);
    screen_attach(&sb, &b.ppu);
    ws.sprites = 1;
    for (f = 0; f < frames && f < m->count; f++) {
        int y;
        nes_set_pads(&a, m->frames[f].pad[0], m->frames[f].pad[1]);
        nes_set_pads(&b, m->frames[f].pad[0], m->frames[f].pad[1]);
        nes_run_frame(&a);
        nes_run_frame(&b);
        wide_render(&ws, &b, &sb, widx);
        if (ram_diff < 0 && memcmp(a.ram, b.ram, sizeof a.ram))
            ram_diff = f + 1;
        for (y = 0; y < PPU_H && center_bad < 0; y++)
            if (memcmp(widx[y] + WIDE_MARGIN + 8, sa.idx[y] + 8, PPU_W - 8))
                center_bad = f + 1;
        widened += ws.lines_widened > 150;
        margin_entities += ws.margin_entities;
    }
    snprintf(text, sizeof text, "widescreen %s: RAM identical with and without the wide picture over %ld frames", label,
             f);
    check(ram_diff < 0, text);
    snprintf(text, sizeof text, "widescreen %s: the wide picture's middle is the game's picture on every frame", label);
    check(center_bad < 0, text);
    snprintf(text, sizeof text, "widescreen %s: %ld of %ld frames widened, %ld entity draws in the margins", label,
             widened, f, margin_entities);
    check(widened > 0, text);
}

static void test_nolimit(const Fm2 *m, PpuModel model, long frames, const char *label)
{
    static Nes a, b;
    static Screen sa, sb;
    char text[300];
    long f, ram_diff = -1, bad_pixels = 0, diff_frames = 0, diff_pixels = 0;

    nes_init(&a, &rom);
    nes_power(&a, model);
    screen_attach(&sa, &a.ppu);
    nes_init(&b, &rom);
    nes_power(&b, model);
    screen_attach(&sb, &b.ppu);
    for (f = 0; f < frames && f < m->count; f++) {
        int y, x, frame_diff = 0;
        nes_set_pads(&a, m->frames[f].pad[0], m->frames[f].pad[1]);
        nes_set_pads(&b, m->frames[f].pad[0], m->frames[f].pad[1]);
        screen_set_no_sprite_limit(0);
        nes_run_frame(&a);
        screen_set_no_sprite_limit(1);
        nes_run_frame(&b);
        screen_set_no_sprite_limit(0);
        if (ram_diff < 0 && memcmp(a.ram, b.ram, sizeof a.ram))
            ram_diff = f + 1;
        for (y = 0; y < PPU_H; y++) {
            for (x = 0; x < PPU_W; x++) {
                if (sa.idx[y][x] == sb.idx[y][x])
                    continue;
                diff_pixels++;
                frame_diff = 1;
                if (!sb.line_extra[y])
                    bad_pixels++; /* a difference on a line without extra sprites */
            }
        }
        diff_frames += frame_diff;
    }
    snprintf(text, sizeof text, "nolimit %s: RAM identical with and without the mod over %ld frames", label, f);
    check(ram_diff < 0, text);
    if (model == PPU_MODEL_OLD) {
        /* lines where the sprite data changed mid-line fall back to the PPU's 8 (as without the mod); they
         * must stay rare */
        snprintf(text, sizeof text,
                 "nolimit %s: our sprite evaluation agreed with the PPU's first 8 on all but %u lines (those show the "
                 "PPU's 8)", label, (unsigned)sb.sprite_mismatch_lines);
        check(sb.sprite_mismatch_lines * 10000u <= (uint32_t)(f * PPU_H), text);
    }
    snprintf(text, sizeof text,
             "nolimit %s: %u lines had more than 8 sprites; %ld frames (%.1f%%) show extra sprite pixels (%ld pixels)",
             label, (unsigned)sb.extra_sprite_lines, diff_frames, 100.0 * diff_frames / (f ? f : 1), diff_pixels);
    check(sb.extra_sprite_lines > 0 && diff_frames > 0 && sa.extra_sprite_lines == 0, text);
    snprintf(text, sizeof text, "nolimit %s: pictures differ only on lines with more than 8 sprites (%ld other pixels)",
             label, bad_pixels);
    check(bad_pixels == 0, text);
}

int main(int argc, char **argv)
{
    char hash[41], err[256];
    Fm2 m2p = {0}, m1p = {0};
    long frames = 20000;
    int i;
    if (argc < 4) {
        fprintf(stderr, "usage: mods_test ROM 2P.fm2 1P.fm2 [--frames N]\n");
        return 2;
    }
    for (i = 4; i < argc; i++)
        if (!strcmp(argv[i], "--frames") && i + 1 < argc)
            frames = strtol(argv[++i], NULL, 10);
    if (rom_load(argv[1], &rom, hash) != ROM_OK || fm2_load(argv[2], &m2p, err, sizeof err) ||
        fm2_load(argv[3], &m1p, err, sizeof err)) {
        fprintf(stderr, "cannot load the ROM or a movie\n");
        return 2;
    }
    test_lives(&m2p);
    test_before_replace(&m2p);
    test_cheats(&m2p);
    test_nolimit(&m2p, PPU_MODEL_OLD, frames, "2P old PPU");
    test_nolimit(&m1p, PPU_MODEL_NEW, frames, "1P new PPU");
    test_widescreen(&m2p, PPU_MODEL_OLD, 60000, "2P old PPU");
    test_widescreen(&m1p, PPU_MODEL_NEW, frames, "1P new PPU");
    printf("%s\n", failed ? "FAIL" : "PASS");
    fm2_free(&m2p);
    fm2_free(&m1p);
    return failed;
}
