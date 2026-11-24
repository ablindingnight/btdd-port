/* btdd_gfxshot: renders movie frames through the GPU shader presets (src/platform/gpu.c) into PNG files, offscreen
 * (no window). For comparing the display options; output is game pictures, so write it outside the repo.
 *
 *   btdd_gfxshot ROM --movie X.fm2 --frames N[,N...] --out DIR [--size WxH] [--presets a,b,...] [--ppu old|new]
 *
 * Files: DIR/f<frame>_<preset>_<W>x<H>.png. Default size 1280x960 (4:3 picture, 4x vertical), default presets all. */
#include "nes/nes.h"
#include "platform/gpu.h"
#include "platform/png.h"
#include "platform/screen.h"
#include "platform/wide.h"
#include "rom/rom.h"
#include "runtime/fm2.h"
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static Rom rom;
static Nes nes;
static Screen screen;
static uint32_t pixels[WIDE_W * PPU_H];
static uint8_t widx[PPU_H][WIDE_W];
static WideState ws;

int main(int argc, char **argv)
{
    const char *rom_path = NULL, *movie_path = NULL, *frames_spec = NULL, *out_dir = NULL, *presets = NULL;
    int w = 1280, h = 960, i, want[GPU_PRESET_COUNT], model = PPU_MODEL_OLD, rc = 0;
    long frame = 0;
    int wide = 0;
    char hash[41], err[256];
    Fm2 movie = {0};
    Gpu *g;
    uint32_t *img;
    const char *s;

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--movie") && i + 1 < argc)
            movie_path = argv[++i];
        else if (!strcmp(argv[i], "--frames") && i + 1 < argc)
            frames_spec = argv[++i];
        else if (!strcmp(argv[i], "--out") && i + 1 < argc)
            out_dir = argv[++i];
        else if (!strcmp(argv[i], "--size") && i + 1 < argc)
            sscanf(argv[++i], "%dx%d", &w, &h);
        else if (!strcmp(argv[i], "--presets") && i + 1 < argc)
            presets = argv[++i];
        else if (!strcmp(argv[i], "--wide")) /* M9: the 424-pixel picture through the presets */
            wide = 1;
        else if (!strcmp(argv[i], "--ppu") && i + 1 < argc)
            model = !strcmp(argv[++i], "new") ? PPU_MODEL_NEW : PPU_MODEL_OLD;
        else if (argv[i][0] != '-' && !rom_path)
            rom_path = argv[i];
        else {
            fprintf(stderr, "usage: btdd_gfxshot ROM --movie X.fm2 --frames N[,N...] --out DIR [--size WxH] "
                            "[--presets a,b] [--ppu old|new]\n");
            return 2;
        }
    }
    if (!rom_path || !movie_path || !frames_spec || !out_dir || w < 16 || h < 16 || w > 8192 || h > 8192) {
        fprintf(stderr, "need ROM, --movie, --frames, --out (and a sane --size)\n");
        return 2;
    }
    for (i = 0; i < GPU_PRESET_COUNT; i++)
        want[i] = presets == NULL || strstr(presets, gpu_preset_names[i]) != NULL;
    if (presets) /* "smooth-crt" contains "smooth" and "crt": match whole names */
        for (i = 0; i < GPU_PRESET_COUNT; i++) {
            const char *p = presets;
            size_t n = strlen(gpu_preset_names[i]);
            want[i] = 0;
            while ((p = strstr(p, gpu_preset_names[i])) != NULL) {
                if ((p == presets || p[-1] == ',') && (p[n] == 0 || p[n] == ','))
                    want[i] = 1;
                p += n;
            }
        }
    if (rom_load(rom_path, &rom, hash) != ROM_OK || fm2_load(movie_path, &movie, err, sizeof err) != 0) {
        fprintf(stderr, "cannot load the ROM or the movie\n");
        return 2;
    }
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    g = gpu_open(NULL);
    img = malloc((size_t)w * (size_t)h * 4);
    if (!g || !img) {
        fprintf(stderr, "no GPU device\n");
        return 1;
    }
    nes_init(&nes, &rom);
    nes_power(&nes, (PpuModel)model);
    screen_attach(&screen, &nes.ppu);
    for (s = frames_spec; *s;) {
        char *e;
        long target = strtol(s, &e, 10);
        for (; frame < target && frame < movie.count; frame++) {
            nes_set_pads(&nes, movie.frames[frame].pad[0], movie.frames[frame].pad[1]);
            nes_run_frame(&nes);
        }
        if (wide) {
            ws.sprites = ws.hud = 1;
            ws.retry_wait = 0;
            wide_render(&ws, &nes, &screen, widx);
            wide_to_rgb(&screen, widx, pixels, WIDE_W, 0);
            gpu_set_frame_width(g, WIDE_W);
        } else {
            screen_to_rgb(&screen, pixels, PPU_W);
        }
        for (i = 0; i < GPU_PRESET_COUNT; i++) {
            char path[1200];
            if (!want[i])
                continue;
            snprintf(path, sizeof path, "%s/f%ld_%s_%dx%d.png", out_dir, frame, gpu_preset_names[i], w, h);
            if (gpu_render_offscreen(g, pixels, (GpuPreset)i, w, h, img) != 0 ||
                png_write_rgb(path, img, w, h, w) != 0) {
                fprintf(stderr, "failed: %s (%s)\n", path, SDL_GetError());
                rc = 1;
            } else {
                printf("%s\n", path);
            }
        }
        s = *e ? e + 1 : e;
    }
    free(img);
    gpu_close(g);
    fm2_free(&movie);
    SDL_Quit();
    return rc;
}
