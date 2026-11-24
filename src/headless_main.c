/* btdd_headless: runs the reference interpreter without a window.
 *
 *   btdd_headless ROM [--movie X.fm2] [--frames N] [--dump-ram out.bin] [--dump-every K] [--quiet]
 *                     [--watch 05,1F,0300-030F]   (print every CPU write to those RAM addresses)
 *                     [--exec 8291,82AC]          (print when an instruction at that address starts)
 *                     [--cdl out.cdl]             (code/data log, FCEUX format; see src/cov/coverage.h)
 *                     [--cdl-mesen out.cdl]       (same run as a Mesen-format log: adds jump-target/entry bits)
 *                     [--cdl-fceux-clear]         (clear a byte's FCEUX flags when the game writes to it, as FCEUX)
 *                     [--reads out.tsv]           (PRG data reads: byte, reader PC, routine, $000D, $0015, count)
 *                     [--snap-dir D] [--snap-every N]  (snapshots every N frames (default 60) and at every
 *                                                  level/section change and camera lock/unlock; gate log
 *                                                  gates.txt/gates.tsv + gate-*.ram in D)
 *                     [--load X.snap]             (start from a snapshot; the movie continues at its frame)
 *                     [--save-state N X.state]    (save state after frame N, see nes/state.h; run continues)
 *                     [--load-state X.state]      (start from a save state; the movie continues at its frame)
 *                     [--warp L[:S]] [--warp-every]  (start the next stage as level L, section S; see cov/play.h)
 *                     [--god]                     (refill players' HP and lives after every frame)
 *                     [--poke 11=00,12=00@900]    (RAM writes after frame N; "@N+" keeps them frozen)
 *                     [--movie-until N]           (movie input only for frames < N, then no input)
 *                     [--press-every N P1[,P2]]   (without movie input: press these pad bits (hex) every N frames)
 *                     [--shot-every N DIR]        (picture data every N frames for tools/shots.py)
 *                     [--indirect out.tsv]        (jmp (ptr) and $FFCB far-call sites -> targets, with counts)
 *                     [--ram-pcs out.txt]         (RAM addresses where instructions started; the explorer uses
 *                                                  the TAS list to spot crashes)
 *                     [--ppu old|new|auto]        (FCEUX PPU timing model; auto = the movie's NewPPU flag)
 *                     [--prg-poke 6:9B52=00,...]  (change PRG bytes in memory only, for data-format experiments;
 *                                                  the ROM file is never written)
 *
 * RAM dump format (shared with tools/fceux/ramdump.lua and tools/ramdiff.py): a sequence of records,
 * each a little-endian uint32 frame number followed by the 2048 bytes of CPU RAM. The frame number is
 * the number of frames emulated so far (FCEUX's emu.framecount() after emu.frameadvance()).
 * --dump-every K writes a record every K frames; the final frame is always written.
 */
#include "cov/coverage.h"
#include "cov/snapshot.h"
#include "nes/state.h"
#include "cov/tracker.h"
#include "cov/play.h"
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

/* Picture data for tools/shots.py: background (NES colors), CHR-RAM, OAM, palette, PPUCTRL. */
static void write_shot(Nes *n, const char *dir, long frame)
{
    char path[1024];
    FILE *f;
    snprintf(path, sizeof path, "%s/f%06ld-L%02X-S%02X.shot", dir, frame, n->ram[0x0D], n->ram[0x15]);
    f = fopen(path, "wb");
    if (!f)
        return;
    fwrite(n->ppu.fb, 1, PPU_W * PPU_H, f);
    fwrite(n->ppu.chr, 1, sizeof n->ppu.chr, f);
    fwrite(n->ppu.oam, 1, sizeof n->ppu.oam, f);
    fwrite(n->ppu.pal, 1, sizeof n->ppu.pal, f);
    fwrite(&n->ppu.ctrl, 1, 1, f);
    fclose(f);
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
    const char *cdl_path = NULL, *cdl_mesen_path = NULL, *reads_path = NULL;
    int cdl_clear = 0;
    const char *snap_dir = NULL, *load_path = NULL, *state_save_path = NULL, *state_load_path = NULL;
    long state_save_at = -1;
    long snap_every = 60, start = 0;
    static Tracker trk;
    static Play play;
    int use_play = 0;
    long movie_until = -1, shot_every = 0, press_every = 0, press1 = 0, press2 = 0;
    const char *shot_dir = NULL, *ram_pcs_path = NULL, *ind_path = NULL, *prg_poke = NULL;
    static Coverage cov;
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
        else if (!strcmp(argv[i], "--cdl") && i + 1 < argc)
            cdl_path = argv[++i];
        else if (!strcmp(argv[i], "--cdl-mesen") && i + 1 < argc)
            cdl_mesen_path = argv[++i];
        else if (!strcmp(argv[i], "--reads") && i + 1 < argc)
            reads_path = argv[++i];
        else if (!strcmp(argv[i], "--snap-dir") && i + 1 < argc)
            snap_dir = argv[++i];
        else if (!strcmp(argv[i], "--snap-every") && i + 1 < argc)
            snap_every = strtol(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--save-state") && i + 2 < argc) {
            state_save_at = strtol(argv[++i], NULL, 10);
            state_save_path = argv[++i];
        } else if (!strcmp(argv[i], "--load-state") && i + 1 < argc)
            state_load_path = argv[++i];
        else if (!strcmp(argv[i], "--load") && i + 1 < argc)
            load_path = argv[++i];
        else if (!strcmp(argv[i], "--warp") && i + 1 < argc) {
            char *e;
            if (!use_play)
                play_init(&play);
            use_play = 1;
            play.warp_level = (int)strtol(argv[++i], &e, 16);
            if (*e == ':')
                play.warp_section = (int)strtol(e + 1, NULL, 16);
        } else if (!strcmp(argv[i], "--warp-every"))
            play.warp_every = 1;
        else if (!strcmp(argv[i], "--god")) {
            if (!use_play)
                play_init(&play);
            use_play = 1;
            play.god = 1;
        } else if (!strcmp(argv[i], "--poke") && i + 1 < argc) {
            if (!use_play)
                play_init(&play);
            use_play = 1;
            if (play_parse_pokes(&play, argv[++i]) != 0) {
                fprintf(stderr, "bad --poke %s\n", argv[i]);
                return 2;
            }
        } else if (!strcmp(argv[i], "--movie-until") && i + 1 < argc)
            movie_until = strtol(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--shot-every") && i + 2 < argc) {
            shot_every = strtol(argv[++i], NULL, 10);
            shot_dir = argv[++i];
        } else if (!strcmp(argv[i], "--press-every") && i + 2 < argc) {
            char *e;
            press_every = strtol(argv[++i], NULL, 10);
            press1 = strtol(argv[++i], &e, 16);
            if (*e == ',')
                press2 = strtol(e + 1, NULL, 16);
        } else if (!strcmp(argv[i], "--ram-pcs") && i + 1 < argc)
            ram_pcs_path = argv[++i];
        else if (!strcmp(argv[i], "--indirect") && i + 1 < argc)
            ind_path = argv[++i];
        else if (!strcmp(argv[i], "--cdl-fceux-clear"))
            cdl_clear = 1;
        else if (!strcmp(argv[i], "--ppu") && i + 1 < argc)
            ppu_opt = argv[++i];
        else if (!strcmp(argv[i], "--prg-poke") && i + 1 < argc)
            prg_poke = argv[++i];
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
    if (prg_poke) { /* bank:addr=value[,bank:addr=value...], hex; in memory only */
        const char *s = prg_poke;
        while (*s) {
            char *e;
            long b = strtol(s, &e, 16), a, v;
            if (*e != ':')
                break;
            a = strtol(e + 1, &e, 16);
            if (*e != '=')
                break;
            v = strtol(e + 1, &e, 16);
            if (b < 0 || b >= ROM_BANKS || a < 0x8000 || a > 0xFFFF || v < 0 || v > 255)
                break;
            rom.prg[b][a & 0x7FFF] = (uint8_t)v;
            s = *e ? e + 1 : e;
        }
        if (*s) {
            fprintf(stderr, "bad --prg-poke at \"%s\"\n", s);
            return 2;
        }
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
    if (use_play) {
        play_install(&play, &nes);
        play_after_frame(&play, &nes, 0);
    }
    if (state_load_path) {
        uint64_t f = 0;
        int lr = nes_state_load(&nes, &f, hash, state_load_path);
        if (lr != NES_STATE_OK) {
            fprintf(stderr, "%s: %s\n", state_load_path, nes_state_error(lr));
            return 1;
        }
        start = (long)f;
    }
    if (load_path) {
        SnapInfo si;
        int lr = snap_load(&nes, &si, load_path);
        if (lr != 0) {
            fprintf(stderr, "%s: %s\n", load_path, lr == -2 ? "not a snapshot of this build" : "cannot read");
            return 1;
        }
        start = (long)si.frame;
    }
    if (snap_dir) {
        char p[1024];
        snprintf(p, sizeof p, "%s/gates.txt", snap_dir);
        trk.log = fopen(p, "w");
        snprintf(p, sizeof p, "%s/gates.tsv", snap_dir);
        trk.tsv = fopen(p, "w");
        trk.ram_dir = snap_dir;
        if (!trk.log || !trk.tsv) {
            fprintf(stderr, "cannot write in %s (does the folder exist?)\n", snap_dir);
            return 1;
        }
        tracker_init(&trk, &nes, start);
    }
    if (cdl_path || cdl_mesen_path || reads_path || ram_pcs_path || ind_path) {
        cov_init(&cov, &rom);
        cov.fceux_clear_on_write = cdl_clear;
        cov.log_reads = reads_path != NULL;
        cov.log_indirect = ind_path != NULL;
        cov_attach(&cov, &nes);
    }
    t0 = clock();
    for (i = start; i < frames; i++) {
        cur_frame = i;
        if (movie.frames && i < movie.count && (movie_until < 0 || i < movie_until)) {
            const Fm2Frame *fr = &movie.frames[i];
            if (fr->cmd & (FM2_CMD_RESET | FM2_CMD_POWER)) {
                fprintf(stderr, "frame %ld: movie reset/power command not supported yet\n", i);
                return 1;
            }
            nes_set_pads(&nes, fr->pad[0], fr->pad[1]);
        } else if (press_every > 0 && i % press_every == 0) {
            nes_set_pads(&nes, (uint8_t)press1, (uint8_t)press2);
        } else {
            nes_set_pads(&nes, 0, 0);
        }
        nes_run_frame(&nes);
        if (use_play)
            play_after_frame(&play, &nes, i + 1);
        if (shot_every > 0 && (i + 1) % shot_every == 0)
            write_shot(&nes, shot_dir, i + 1);
        if (watch_spec || exec_spec)
            printf("f %ld cyc %llu\n", i + 1, (unsigned long long)nes.cpu.cycles);
        if (dump && ((every > 0 && (i + 1) % every == 0) || i + 1 == frames)) {
            if (write_record(dump, (uint32_t)(i + 1), nes.ram) != 0) {
                fprintf(stderr, "write error on %s\n", dump_path);
                return 1;
            }
        }
        if (snap_dir) {
            unsigned why = tracker_frame(&trk, &nes, i + 1);
            if (snap_every > 0 && (i + 1) % snap_every == 0)
                why |= SNAP_PERIODIC;
            if (why) {
                char p[1024], rs[32];
                SnapInfo si;
                si.frame = (uint32_t)(i + 1);
                si.level = nes.ram[0x0D];
                si.section = nes.ram[0x15];
                si.reason = (uint16_t)why;
                snprintf(p, sizeof p, "%s/f%06ld-L%02X-S%02X-%s.snap", snap_dir, i + 1, si.level, si.section,
                         snap_reason_text(si.reason, rs, sizeof rs));
                if (snap_save(&nes, &si, p) != 0) {
                    fprintf(stderr, "cannot write %s\n", p);
                    return 1;
                }
            }
        }
        if (state_save_path && i + 1 == state_save_at &&
            nes_state_save(&nes, (uint64_t)(i + 1), hash, state_save_path) != NES_STATE_OK) {
            fprintf(stderr, "cannot write %s\n", state_save_path);
            return 1;
        }
        if (nes.cpu.jammed) {
            fprintf(stderr, "CPU jammed at frame %ld, PC=$%04X\n", i + 1, nes.cpu.pc);
            break;
        }
    }
    if (dump)
        fclose(dump);
    if ((cdl_path && cov_write_fceux(&cov, cdl_path) != 0) ||
        (cdl_mesen_path && cov_write_mesen(&cov, cdl_mesen_path) != 0) ||
        (reads_path && cov_write_reads(&cov, reads_path) != 0) ||
        (ram_pcs_path && cov_write_ram_pcs(&cov, ram_pcs_path) != 0) ||
        (ind_path && cov_write_indirect(&cov, &rom, ind_path) != 0)) {
        fprintf(stderr, "cannot write the coverage output\n");
        return 1;
    }
    if (cdl_path || cdl_mesen_path || reads_path || ram_pcs_path || ind_path) {
        int b;
        long code, data, both, seen, tot = 0;
        for (b = 0; b < 8; b++) {
            cov_count(cov.fceux, b, &code, &data, &both, &seen);
            tot += seen;
            if (!quiet)
                printf("cdl bank %d: code %ld  data %ld  both %ld  seen %ld (%.1f%%)\n", b, code, data, both, seen,
                       100.0 * seen / 0x8000);
        }
        if (!quiet)
            printf("cdl total seen %ld of %d (%.1f%%), reads table %u rows, instructions run from RAM %llu\n",
                   tot, COV_PRG_SIZE, 100.0 * tot / COV_PRG_SIZE, cov.reads_used,
                   (unsigned long long)cov.ram_exec);
        cov_free(&cov);
    }

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
