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
    test_nolimit(&m2p, PPU_MODEL_OLD, frames, "2P old PPU");
    test_nolimit(&m1p, PPU_MODEL_NEW, frames, "1P new PPU");
    printf("%s\n", failed ? "FAIL" : "PASS");
    fm2_free(&m2p);
    fm2_free(&m1p);
    return failed;
}
