/* btdd_shot: plays a movie headless and saves the picture at chosen frames.
 *
 *   btdd_shot ROM --movie X.fm2 --frames 600,1200 --out DIR [--ppu old|new|auto] [--mod ID[=N]]
 *
 * For each frame N writes DIR/shot_N.png and DIR/idx_N.bin (61,440 bytes: NES palette index per pixel,
 * row-major), comparable with tools/fceux/screendump.lua output via tools/shotdiff.py. Frame N = the picture
 * drawn during frame N (FCEUX's screen after emu.frameadvance() returns with framecount N). */
#include "mods/mods.h"
#include "nes/nes.h"
#include "platform/png.h"
#include "platform/screen.h"
#include "platform/wide.h"
#include "rom/rom.h"
#include "runtime/fm2.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static Nes nes;
static Screen screen;
static uint32_t rgb[PPU_W * PPU_H];

int main(int argc, char **argv)
{
    static Rom rom;
    const char *rom_path = NULL, *movie_path = NULL, *frames_spec = NULL, *out_dir = ".", *ppu_opt = "auto";
    char hash[41], err[256], path[1024];
    long wanted[256], nwanted = 0, last = 0, i;
    Fm2 movie = {0};
    PpuModel model = PPU_MODEL_OLD;
    int wide = 0;
    static WideState ws;

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--movie") && i + 1 < argc)
            movie_path = argv[++i];
        else if (!strcmp(argv[i], "--frames") && i + 1 < argc)
            frames_spec = argv[++i];
        else if (!strcmp(argv[i], "--out") && i + 1 < argc)
            out_dir = argv[++i];
        else if (!strcmp(argv[i], "--ppu") && i + 1 < argc)
            ppu_opt = argv[++i];
        else if (!strcmp(argv[i], "--wide"))
            wide = 1, ws.sprites = 1;
        else if (!strcmp(argv[i], "--wide-nosprites"))
            wide = 1, ws.sprites = 0;
        else if (!strcmp(argv[i], "--mod") && i + 1 < argc) { /* M7 mods, e.g. --mod nolimit */
            if (mods_apply_arg(argv[++i]) != 0)
                return 2;
        }
        else if (argv[i][0] != '-' && !rom_path)
            rom_path = argv[i];
        else {
            fprintf(stderr, "usage: btdd_shot ROM --movie X.fm2 --frames N,N,... --out DIR [--ppu old|new|auto] [--mod ID[=N]]\n");
            return 2;
        }
    }
    if (!rom_path || !movie_path || !frames_spec) {
        fprintf(stderr, "need ROM, --movie and --frames\n");
        return 2;
    }
    {
        const char *s = frames_spec;
        while (*s && nwanted < 256) {
            char *e;
            wanted[nwanted] = strtol(s, &e, 10);
            if (wanted[nwanted] > last)
                last = wanted[nwanted];
            nwanted++;
            s = *e ? e + 1 : e;
        }
    }
    if (rom_load(rom_path, &rom, hash) != ROM_OK) {
        fprintf(stderr, "ROM rejected\n");
        return 1;
    }
    if (fm2_load(movie_path, &movie, err, sizeof err) != 0) {
        fprintf(stderr, "%s\n", err);
        return 1;
    }
    if (movie.new_ppu)
        model = PPU_MODEL_NEW;
    if (!strcmp(ppu_opt, "old"))
        model = PPU_MODEL_OLD;
    else if (!strcmp(ppu_opt, "new"))
        model = PPU_MODEL_NEW;

    nes_init(&nes, &rom);
    nes_power(&nes, model);
    mods_attach(&nes);
    screen_set_no_sprite_limit(mods_display_flags() & MODS_DISPLAY_NO_SPRITE_LIMIT);
    screen_attach(&screen, &nes.ppu);
    for (i = 0; i < last && i < movie.count; i++) {
        long k;
        nes_set_pads(&nes, movie.frames[i].pad[0], movie.frames[i].pad[1]);
        nes_run_frame(&nes);
        for (k = 0; k < nwanted; k++) {
            if (wanted[k] == i + 1) {
                FILE *f;
                screen_to_rgb(&screen, rgb, PPU_W);
                snprintf(path, sizeof path, "%s/shot_%ld.png", out_dir, i + 1);
                if (png_write_rgb(path, rgb, PPU_W, PPU_H, PPU_W) != 0)
                    fprintf(stderr, "cannot write %s\n", path);
                snprintf(path, sizeof path, "%s/idx_%ld.bin", out_dir, i + 1);
                f = fopen(path, "wb");
                if (!f || fwrite(screen.idx, 1, sizeof screen.idx, f) != sizeof screen.idx)
                    fprintf(stderr, "cannot write %s\n", path);
                if (f)
                    fclose(f);
                if (wide || (mods_display_flags() & MODS_DISPLAY_WIDESCREEN)) { /* M9: the 424-pixel picture */
                    static uint8_t widx[PPU_H][WIDE_W];
                    static uint32_t wrgb[PPU_H * WIDE_W];
                    if (getenv("WIDE_DEBUG"))
                        wide_debug(&nes);
                    if (mods_display_flags() & MODS_DISPLAY_WIDESCREEN) { /* --mod widescreen etc. */
                        ws.sprites = (mods_display_flags() & MODS_DISPLAY_WIDE_SPRITES) != 0;
                        ws.hud = (mods_display_flags() & MODS_DISPLAY_WIDE_HUD) != 0;
                    }
                    ws.retry_wait = 0; /* sparse frames: always allow a full calibration search */
                    wide_render(&ws, &nes, &screen, widx);
                    wide_to_rgb(&screen, widx, wrgb, WIDE_W, (mods_display_flags() & MODS_DISPLAY_WIDE_FADE) != 0);
                    snprintf(path, sizeof path, "%s/wide_%ld.png", out_dir, i + 1);
                    if (png_write_rgb(path, wrgb, WIDE_W, PPU_H, WIDE_W) != 0)
                        fprintf(stderr, "cannot write %s\n", path);
                    printf("frame %ld level %02X section %02X: %d lines widened, %d margin entities, calibration %d/1000 (dx %d dy %d), map %dx%d\n",
                           i + 1, nes.ram[0x10], nes.ram[0x15], ws.lines_widened, ws.margin_entities, ws.calib_match, ws.dx, ws.dy,
                           ws.map_cols, ws.map_rows);
                }
            }
        }
    }
    printf("frames %ld, %ld shots\n", i, nwanted);
    fm2_free(&movie);
    return 0;
}
