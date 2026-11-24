/* Save-state and rewind check on a TAS, with the APU and the display attached (as in btdd_play).
 *
 *   rewind_test ROM MOVIE.fm2 [--ppu old|new] [--at F] [--back K] [--dump-ram out.bin]
 *
 * Runs the movie to frame F pushing every frame's blob (machine state + picture + frame number) into the
 * rewind ring, keeping plain copies of the last K blobs and the audio of the last K frames. Then:
 *  1. pops K times: every popped blob must equal the copy taken at that frame;
 *  2. restores the state K frames back and replays those K frames: the audio must be sample-identical;
 *  3. continues the movie to its end, dumping RAM for tools/ramdiff.py against the FCEUX dump.
 * Also prints the average compressed size per frame. Exit code 0 = all checks passed. */
#include "nes/nes.h"
#include "nes/state.h"
#include "platform/rewind.h"
#include "platform/screen.h"
#include "rom/rom.h"
#include "runtime/fm2.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RATE 48000
#define MAX_FRAME_SAMPLES 2048

static Rom rom;
static Nes nes;
static Screen screen;
static float buf[MAX_FRAME_SAMPLES];

static size_t blob_size(void)
{
    return nes_state_size() + sizeof(Screen) + 8;
}

static void capture(uint8_t *b, long frame)
{
    uint64_t f = (uint64_t)frame;
    nes_state_capture(&nes, b);
    memcpy(b + nes_state_size(), &screen, sizeof screen);
    memcpy(b + nes_state_size() + sizeof screen, &f, 8);
}

static long restore(const uint8_t *b)
{
    uint64_t f;
    nes_state_restore(&nes, b);
    memcpy(&screen, b + nes_state_size(), sizeof screen);
    memcpy(&f, b + nes_state_size() + sizeof screen, 8);
    return (long)f;
}

static size_t run_frame(const Fm2 *m, long frame, float *out)
{
    size_t n;
    if (frame < m->count)
        nes_set_pads(&nes, m->frames[frame].pad[0], m->frames[frame].pad[1]);
    else
        nes_set_pads(&nes, 0, 0);
    nes_run_frame(&nes);
    apu_render(&nes.apu, nes.cpu.cycles);
    n = apu_take(&nes.apu);
    if (out)
        memcpy(out, buf, n * sizeof(float));
    apu_set_buffer(&nes.apu, buf, MAX_FRAME_SAMPLES);
    return n;
}

static void put32(FILE *f, uint32_t v)
{
    uint8_t b[4] = {(uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24)};
    fwrite(b, 1, 4, f);
}

int main(int argc, char **argv)
{
    const char *rom_path = NULL, *movie_path = NULL, *dump_path = NULL;
    long at = 3000, back = 300, frame, i;
    int model = PPU_MODEL_OLD, ok = 1;
    char hash[41], err[256];
    Fm2 movie = {0};
    Rewind rw;
    uint8_t **copies, *blob;
    float *audio;
    size_t *audio_n, bs;
    FILE *dump = NULL;

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--ppu") && i + 1 < argc)
            model = !strcmp(argv[++i], "new") ? PPU_MODEL_NEW : PPU_MODEL_OLD;
        else if (!strcmp(argv[i], "--at") && i + 1 < argc)
            at = strtol(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--back") && i + 1 < argc)
            back = strtol(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--dump-ram") && i + 1 < argc)
            dump_path = argv[++i];
        else if (!rom_path)
            rom_path = argv[i];
        else
            movie_path = argv[i];
    }
    if (!rom_path || !movie_path || back < 1 || back >= at) {
        fprintf(stderr, "usage: rewind_test ROM MOVIE.fm2 [--ppu old|new] [--at F] [--back K] [--dump-ram out.bin]\n");
        return 2;
    }
    if (rom_load(rom_path, &rom, hash) != ROM_OK || fm2_load(movie_path, &movie, err, sizeof err) != 0) {
        fprintf(stderr, "cannot load the ROM or the movie\n");
        return 2;
    }
    nes_init(&nes, &rom);
    nes_power(&nes, (PpuModel)model);
    screen_attach(&screen, &nes.ppu);
    nes_enable_apu(&nes, RATE);
    apu_set_buffer(&nes.apu, buf, MAX_FRAME_SAMPLES);

    bs = blob_size();
    if (rewind_init(&rw, bs, 4096, (size_t)256 << 20) != 0)
        return 2;
    blob = malloc(bs);
    copies = calloc((size_t)back + 1, sizeof *copies);
    audio = malloc((size_t)back * MAX_FRAME_SAMPLES * sizeof(float));
    audio_n = calloc((size_t)back, sizeof *audio_n);
    for (i = 0; i <= back; i++)
        copies[i] = malloc(bs);
    if (!blob || !audio || !audio_n)
        return 2;
    if (dump_path && !(dump = fopen(dump_path, "wb")))
        return 2;

    /* run to `at`, pushing every frame; keep copies of frames at-back .. at */
    capture(blob, 0);
    rewind_push(&rw, blob);
    for (frame = 0; frame < at; frame++) {
        long k = frame + 1 - (at - back); /* index of this frame's result among the copies */

        size_t n = run_frame(&movie, frame, k >= 1 ? audio + (size_t)(k - 1) * MAX_FRAME_SAMPLES : NULL);
        if (k >= 1)
            audio_n[k - 1] = n;
        capture(blob, frame + 1);
        rewind_push(&rw, blob);

        if (k >= 0)
            memcpy(copies[k], blob, bs);
        if (dump) {
            put32(dump, (uint32_t)(frame + 1));
            fwrite(nes.ram, 1, 0x800, dump);
        }
    }
    printf("ring: %d entries, %.1f MB, %.1f KB per frame on average (blob %.1f KB)\n", rw.count,
           rw.bytes / 1048576.0, rw.bytes / 1024.0 / rw.count, bs / 1024.0);

    /* 1: pop back K frames, each must equal the copy */
    for (i = back - 1; i >= 0; i--) {
        if (!rewind_pop(&rw, blob) || memcmp(blob, copies[i], bs) != 0) {
            printf("FAIL: pop to frame %ld differs\n", at - back + i);
            ok = 0;
            break;
        }
    }
    if (ok)
        printf("ok: %ld pops, every blob identical to the copy\n", back);


    /* 2: restore frame at-back, replay K frames, audio must match the first run */
    frame = restore(copies[0]);
    if (frame != at - back) {
        printf("FAIL: restored frame number %ld\n", frame);
        ok = 0;
    }
    {
        long bad = -1;
        static float again[MAX_FRAME_SAMPLES];
        for (i = 0; i < back; i++, frame++) {
            size_t n = run_frame(&movie, frame, again);
            capture(blob, frame + 1);
            if (memcmp(blob, copies[i + 1], bs) != 0 && bad < 0)
                bad = frame + 1;
            if ((n != audio_n[i] || memcmp(again, audio + (size_t)i * MAX_FRAME_SAMPLES, n * sizeof(float)) != 0) &&
                bad < 0)
                bad = frame + 1;
        }
        if (bad >= 0) {
            printf("FAIL: replay after restore differs at frame %ld\n", bad);
            ok = 0;
        } else {
            printf("ok: replayed %ld frames after restore: state and audio identical\n", back);
        }
    }

    /* 3: continue to the end of the movie (the dump already holds frames 1..at) */
    for (; frame < movie.count; frame++) {
        run_frame(&movie, frame, NULL);
        if (dump) {
            put32(dump, (uint32_t)(frame + 1));
            fwrite(nes.ram, 1, 0x800, dump);
        }
    }
    if (dump)
        fclose(dump);
    printf("%s\n", ok ? "PASS" : "FAIL");
    rewind_free(&rw);
    fm2_free(&movie);
    return ok ? 0 : 1;
}
