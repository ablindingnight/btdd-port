/* btdd_headless: runs the reference interpreter without a window.
 *
 *   btdd_headless ROM [--movie X.fm2] [--frames N] [--dump-ram out.bin] [--dump-every K] [--quiet]
 *                     [--watch 05,1F,0300-030F]   (print every CPU write to those RAM addresses)
 *                     [--exec 8291,82AC]          (print when an instruction at that address starts)
 *                     [--ppu old|new|auto]        (FCEUX PPU timing model; auto = the movie's NewPPU flag)
 *
 * RAM dump format (shared with tools/fceux/ramdump.lua and tools/ramdiff.py): a sequence of records,
 * each a little-endian uint32 frame number followed by the 2048 bytes of CPU RAM. The frame number is
 * the number of frames emulated so far (FCEUX's emu.framecount() after emu.frameadvance()).
 * --dump-every K writes a record every K frames; the final frame is always written.
 */
#include "nes/nes.h"
#include "rom/rom.h"
#include "runtime/fm2.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static void usage(void)
{
    fprintf(stderr, "usage: btdd_headless ROM [--movie X.fm2] [--frames N] [--dump-ram out.bin] "
                    "[--dump-every K] [--ppu old|new|auto] [--quiet]\n");
}

static long cur_frame;

static void print_watch(Nes *n, uint16_t addr, uint8_t v)
{
    printf("w frame %ld line %d cyc %llu pc %d:$%04X  $%04X = %02X  A%02X X%02X Y%02X\n", cur_frame + 1,
           n->ppu.scanline, (unsigned long long)n->cpu.cycles, n->prg_bank, n->cpu.pc, addr, v, n->cpu.a,
           n->cpu.x, n->cpu.y);
}

static uint8_t exec_map[0x10000];

static void print_exec(void *ctx, Cpu *c)
{
    Nes *n = ctx;
    printf("x frame %ld line %d cyc %llu pc %d:$%04X  A%02X X%02X Y%02X P%02X\n", cur_frame + 1,
           n->ppu.scanline, (unsigned long long)c->cycles, n->prg_bank, c->pc, c->a, c->x, c->y, c->p);
}

static int write_record(FILE *f, uint32_t frame, const uint8_t *ram)
{
    uint8_t hdr[4] = {(uint8_t)frame, (uint8_t)(frame >> 8), (uint8_t)(frame >> 16), (uint8_t)(frame >> 24)};
    return fwrite(hdr, 1, 4, f) == 4 && fwrite(ram, 1, 0x800, f) == 0x800 ? 0 : -1;
}

int main(int argc, char **argv)
{
    static Rom rom;
    static Nes nes;
    const char *rom_path = NULL, *movie_path = NULL, *dump_path = NULL;
    long frames = -1, every = 0, i;
    int quiet = 0;
    const char *ppu_opt = "auto";
    PpuModel model = PPU_MODEL_OLD;
    const char *watch_spec = NULL, *exec_spec = NULL;
    char hash[41] = "", err[256];
    Fm2 movie = {0};
    FILE *dump = NULL;
    RomResult r;
    clock_t t0;

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--movie") && i + 1 < argc)
            movie_path = argv[++i];
        else if (!strcmp(argv[i], "--frames") && i + 1 < argc)
            frames = strtol(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--dump-ram") && i + 1 < argc)
            dump_path = argv[++i];
        else if (!strcmp(argv[i], "--dump-every") && i + 1 < argc)
            every = strtol(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--watch") && i + 1 < argc)
            watch_spec = argv[++i];
        else if (!strcmp(argv[i], "--exec") && i + 1 < argc)
            exec_spec = argv[++i];
        else if (!strcmp(argv[i], "--ppu") && i + 1 < argc)
            ppu_opt = argv[++i];
        else if (!strcmp(argv[i], "--quiet"))
            quiet = 1;
        else if (argv[i][0] != '-' && !rom_path)
            rom_path = argv[i];
        else {
            usage();
            return 2;
        }
    }
    if (!rom_path) {
        usage();
        return 2;
    }
    r = rom_load(rom_path, &rom, hash);
    if (r != ROM_OK) {
        fprintf(stderr, "ROM rejected: %s\n", rom_result_text(r));
        return 1;
    }
    if (movie_path) {
        if (fm2_load(movie_path, &movie, err, sizeof err) != 0) {
            fprintf(stderr, "%s\n", err);
            return 1;
        }
        if (movie.new_ppu)
            model = PPU_MODEL_NEW;
        if (frames < 0)
            frames = movie.count;
    }
    if (frames < 0)
        frames = 60;
    if (!strcmp(ppu_opt, "old"))
        model = PPU_MODEL_OLD;
    else if (!strcmp(ppu_opt, "new"))
        model = PPU_MODEL_NEW;
    else if (strcmp(ppu_opt, "auto") != 0) {
        usage();
        return 2;
    }
    if (dump_path) {
        dump = fopen(dump_path, "wb");
        if (!dump) {
            fprintf(stderr, "cannot write %s\n", dump_path);
            return 1;
        }
    }

    nes_init(&nes, &rom);
    nes_power(&nes, model);
    if (watch_spec) { /* comma list of RAM addresses in hex, e.g. 05,1F,0300-0310 */
        const char *s = watch_spec;
        while (*s) {
            char *e;
            long lo = strtol(s, &e, 16), hi = lo;
            if (*e == '-')
                hi = strtol(e + 1, &e, 16);
            for (; lo <= hi && lo < 0x800; lo++)
                nes.watch[lo] = 1;
            s = *e ? e + 1 : e;
        }
        nes.watch_hook = print_watch;
    }
    if (exec_spec) { /* comma list of CPU addresses in hex (any bank) */
        const char *s = exec_spec;
        while (*s) {
            char *e;
            exec_map[strtol(s, &e, 16) & 0xFFFF] = 1;
            s = *e ? e + 1 : e;
        }
        nes.cpu.exec_watch = exec_map;
        nes.cpu.exec_hook = print_exec;
    }
    t0 = clock();
    for (i = 0; i < frames; i++) {
        cur_frame = i;
        if (movie.frames && i < movie.count) {
            const Fm2Frame *fr = &movie.frames[i];
            if (fr->cmd & (FM2_CMD_RESET | FM2_CMD_POWER)) {
                fprintf(stderr, "frame %ld: movie reset/power command not supported yet\n", i);
                return 1;
            }
            nes_set_pads(&nes, fr->pad[0], fr->pad[1]);
        } else {
            nes_set_pads(&nes, 0, 0);
        }
        nes_run_frame(&nes);
        if (watch_spec || exec_spec)
            printf("f %ld cyc %llu\n", i + 1, (unsigned long long)nes.cpu.cycles);
        if (dump && ((every > 0 && (i + 1) % every == 0) || i + 1 == frames)) {
            if (write_record(dump, (uint32_t)(i + 1), nes.ram) != 0) {
                fprintf(stderr, "write error on %s\n", dump_path);
                return 1;
            }
        }
        if (nes.cpu.jammed) {
            fprintf(stderr, "CPU jammed at frame %ld, PC=$%04X\n", i + 1, nes.cpu.pc);
            break;
        }
    }
    if (dump)
        fclose(dump);

    if (!quiet) {
        double secs = (double)(clock() - t0) / CLOCKS_PER_SEC;
        printf("frames %ld  cpu cycles %llu  instructions %llu  %.2f s (%.0f fps)\n", i,
               (unsigned long long)nes.cpu.cycles, (unsigned long long)nes.cpu.instructions, secs,
               secs > 0 ? (double)i / secs : 0.0);
        printf("bank %d  PC $%04X  unofficial ops %llu  bus conflicts %llu  mapper writes %llu  "
               "$4015 reads %llu  DMC enables %llu\n",
               nes.prg_bank, nes.cpu.pc, (unsigned long long)nes.stats.unofficial_ops,
               (unsigned long long)nes.stats.bus_conflicts, (unsigned long long)nes.stats.mapper_writes,
               (unsigned long long)nes.stats.apu_status_reads, (unsigned long long)nes.stats.dmc_enables);
    }
    fm2_free(&movie);
    return nes.cpu.jammed ? 3 : 0;
}
