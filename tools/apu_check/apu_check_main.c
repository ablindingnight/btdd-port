/* btdd_apu_check: plays a movie on the reference interpreter with the APU attached, and writes
 *  - a per-frame channel log in the layout of FCEUX's musiclog.lua apu.csv (compare with tools/apudiff.py),
 *  - optionally a 48 kHz WAV of the first N seconds, and a RAM dump (same format as btdd_headless).
 *
 *   btdd_apu_check ROM --movie X.fm2 [--frames N] [--csv out.csv] [--wav out.wav] [--wav-seconds S]
 *                      [--dump-ram out.bin] [--ppu old|new|auto]
 *
 * CSV rows: row 1 is the power-on state, row f is the state after frame f-1. That is how musiclog.lua's
 * rows line up with our frames (its first frameadvance is FCEUX's frame before the movie starts).
 *
 * The APU is advanced at the start of every CPU instruction through the CPU's exec hook (all addresses
 * watched), which is where FCEUX advances its APU. Register writes come through Nes.apu_hook. */
#include "nes/apu.h"
#include "nes/nes.h"
#include "rom/rom.h"
#include "runtime/fm2.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static Apu apu;
static Nes nes;
static uint8_t watch_all[0x10000];

static void on_instruction(void *ctx, Cpu *cpu)
{
    (void)ctx;
    apu_tick(&apu, cpu->cycles);
}

static void on_apu_write(void *user, uint64_t cycle, uint16_t addr, uint8_t value)
{
    (void)user;
    apu_write(&apu, cycle, addr, value);
}

static uint8_t host_read(void *ctx, uint16_t addr)
{
    return cpu_bus_read(&((Nes *)ctx)->cpu, addr);
}

static void host_stall(void *ctx, int cycles)
{
    cpu_add_cycles(&((Nes *)ctx)->cpu, cycles);
}

static void host_irq(void *ctx, int frame_irq, int dmc_irq)
{
    Cpu *c = &((Nes *)ctx)->cpu;
    c->irq = (uint8_t)((c->irq & ~(CPU_IRQ_FRAME | CPU_IRQ_DMC)) | (frame_irq ? CPU_IRQ_FRAME : 0) |
                       (dmc_irq ? CPU_IRQ_DMC : 0));
}

/* Lua 5.1 number formatting as musiclog.lua does it: integers via %.14g, others with 2 decimals. */
static void put_num(FILE *f, double x)
{
    if (x == floor(x))
        fprintf(f, ",%.14g", x);
    else
        fprintf(f, ",%.2f", x);
}

static void csv_row(FILE *f, long row)
{
    ApuView v;
    int x;
    apu_view(&apu, &v);
    fprintf(f, "%ld", row);
    for (x = 0; x < 2; x++) {
        put_num(f, v.sq_key[x]);
        put_num(f, v.sq_vol[x]);
        put_num(f, v.sq_duty[x]);
    }
    put_num(f, v.tri_key);
    put_num(f, v.tri_vol);
    put_num(f, v.noise_key);
    put_num(f, v.noise_vol);
    put_num(f, v.noise_short);
    put_num(f, v.dmc_vol);
    put_num(f, v.dmc_addr);
    put_num(f, v.dmc_size);
    fputc('\n', f);
}

static void put_le(FILE *f, uint32_t v, int bytes)
{
    while (bytes--) {
        fputc((int)(v & 0xFF), f);
        v >>= 8;
    }
}

static int write_wav(const char *path, const float *s, size_t n, int rate)
{
    FILE *f = fopen(path, "wb");
    size_t i;
    if (!f)
        return -1;
    fwrite("RIFF", 1, 4, f);
    put_le(f, (uint32_t)(36 + n * 2), 4);
    fwrite("WAVEfmt ", 1, 8, f);
    put_le(f, 16, 4);
    put_le(f, 1, 2);            /* PCM */
    put_le(f, 1, 2);            /* mono */
    put_le(f, (uint32_t)rate, 4);
    put_le(f, (uint32_t)rate * 2, 4);
    put_le(f, 2, 2);
    put_le(f, 16, 2);
    fwrite("data", 1, 4, f);
    put_le(f, (uint32_t)(n * 2), 4);
    for (i = 0; i < n; i++) {
        long v = lrintf(s[i] * 32767.0f);
        put_le(f, (uint32_t)(uint16_t)(int16_t)v, 2);
    }
    return fclose(f);
}

int main(int argc, char **argv)
{
    static Rom rom;
    const char *rom_path = NULL, *movie_path = NULL, *csv_path = NULL, *wav_path = NULL, *dump_path = NULL;
    const char *ppu_opt = "auto";
    long frames = -1, i;
    double wav_seconds = 120;
    char hash[41], err[256];
    Fm2 movie = {0};
    PpuModel model = PPU_MODEL_OLD;
    FILE *csv = NULL, *dump = NULL;
    float *samples = NULL;
    size_t nsamples = 0, cap = 0;
    ApuHost host;
    clock_t t0;
    const int rate = 48000;

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--movie") && i + 1 < argc)
            movie_path = argv[++i];
        else if (!strcmp(argv[i], "--frames") && i + 1 < argc)
            frames = strtol(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--csv") && i + 1 < argc)
            csv_path = argv[++i];
        else if (!strcmp(argv[i], "--wav") && i + 1 < argc)
            wav_path = argv[++i];
        else if (!strcmp(argv[i], "--wav-seconds") && i + 1 < argc)
            wav_seconds = strtod(argv[++i], NULL);
        else if (!strcmp(argv[i], "--dump-ram") && i + 1 < argc)
            dump_path = argv[++i];
        else if (!strcmp(argv[i], "--ppu") && i + 1 < argc)
            ppu_opt = argv[++i];
        else if (argv[i][0] != '-' && !rom_path)
            rom_path = argv[i];
        else {
            fprintf(stderr, "usage: btdd_apu_check ROM --movie X.fm2 [--frames N] [--csv out.csv] [--wav out.wav] "
                            "[--wav-seconds S] [--dump-ram out.bin] [--ppu old|new|auto]\n");
            return 2;
        }
    }
    if (!rom_path || !movie_path) {
        fprintf(stderr, "need ROM and --movie\n");
        return 2;
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
    if (frames < 0 || frames > movie.count)
        frames = movie.count;

    nes_init(&nes, &rom);
    nes_power(&nes, model);
    host.ctx = &nes;
    host.read = host_read;
    host.stall = host_stall;
    host.irq = host_irq;
    apu_power(&apu, rate, &host);
    nes.apu_hook = on_apu_write;
    memset(watch_all, 1, sizeof watch_all);
    nes.cpu.exec_watch = watch_all;
    nes.cpu.exec_hook = on_instruction;
    if (wav_path) {
        cap = (size_t)(wav_seconds * rate) + rate;
        samples = malloc(cap * sizeof *samples);
        if (!samples) {
            fprintf(stderr, "out of memory\n");
            return 1;
        }
        apu_set_buffer(&apu, samples, cap);
    }
    if (csv_path) {
        csv = fopen(csv_path, "w");
        if (!csv) {
            fprintf(stderr, "cannot write %s\n", csv_path);
            return 1;
        }
        fputs("frame,sq1_key,sq1_vol,sq1_duty,sq2_key,sq2_vol,sq2_duty,tri_key,tri_vol,noise_key,noise_vol,"
              "noise_short,dpcm_vol,dpcm_addr,dpcm_size\n", csv);
        csv_row(csv, 1);
    }
    if (dump_path && !(dump = fopen(dump_path, "wb"))) {
        fprintf(stderr, "cannot write %s\n", dump_path);
        return 1;
    }

    t0 = clock();
    for (i = 0; i < frames; i++) {
        nes_set_pads(&nes, movie.frames[i].pad[0], movie.frames[i].pad[1]);
        nes_run_frame(&nes);
        if (csv)
            csv_row(csv, i + 2);
        if (dump) {
            uint32_t fr = (uint32_t)(i + 1);
            uint8_t hdr[4] = {(uint8_t)fr, (uint8_t)(fr >> 8), (uint8_t)(fr >> 16), (uint8_t)(fr >> 24)};
            fwrite(hdr, 1, 4, dump);
            fwrite(nes.ram, 1, 0x800, dump);
        }
        if (samples) {
            apu_render(&apu, nes.cpu.cycles);
            nsamples += apu_take(&apu);
            apu_set_buffer(&apu, samples + nsamples, cap - nsamples);
            if (nsamples >= (size_t)(wav_seconds * rate)) {
                if (write_wav(wav_path, samples, (size_t)(wav_seconds * rate), rate) != 0)
                    fprintf(stderr, "cannot write %s\n", wav_path);
                free(samples);
                samples = NULL;
                apu_set_buffer(&apu, NULL, 0);
            }
        }
    }
    if (samples) { /* movie shorter than the requested length */
        if (write_wav(wav_path, samples, nsamples, rate) != 0)
            fprintf(stderr, "cannot write %s\n", wav_path);
        free(samples);
    }
    if (csv)
        fclose(csv);
    if (dump)
        fclose(dump);
    printf("frames %ld  %.1f s  dropped samples %llu\n", i, (double)(clock() - t0) / CLOCKS_PER_SEC,
           (unsigned long long)apu.samples_dropped);
    fm2_free(&movie);
    return 0;
}
