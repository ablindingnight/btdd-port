/* btdd_explore: coverage-guided exploration from snapshots (docs/COVERAGE_PLAN.md, "ways to reach new code" 3-4).
 *
 *   btdd_explore ROM --snaps LIST.txt --out PREFIX [--worker I --workers N] [--seconds T] [--seed S]
 *                [--known KNOWN.txt] [--ram-pcs TAS_RAM_PCS.txt] [--god-ratio 0.5] [--death-ratio 0.05]
 *                [--burst MIN,MAX] [--queue 300] [--save-finds N]
 *
 * LIST.txt holds one snapshot path per line; worker I takes lines I, I+N, I+2N... Every burst starts from a
 * state in the queue (the worker's snapshots, then states that found something), plays MIN..MAX frames of
 * random but plausible input for both pads (held directions, attack/jump rhythms, rare pause/select), and
 * keeps its end state when it ran a code byte or read a data byte that neither the known logs (the .cdl files listed
 * in KNOWN.txt) nor this worker had seen. A burst that jams the CPU or starts an instruction at a RAM address
 * the TAS never used is treated as a crash and its coverage is thrown away.
 *
 * Output: PREFIX.cdl (FCEUX format, this worker's own observations), PREFIX-m.cdl (Mesen format: the same
 * code/data bits plus jump-target/entry bits), PREFIX-finds.tsv (one line per kept burst), and up to N
 * snapshots of the best finds in PREFIX-finds/ (folder must exist).
 */
#include "cov/coverage.h"
#include "cov/snapshot.h"
#include "nes/nes.h"
#include "rom/rom.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct State {
    Nes nes;
    long origin;      /* index into the snapshot list it descends from */
    int depth;        /* bursts since the snapshot */
    int tries;
} State;

static uint64_t rng_s;

static uint32_t rnd(void)
{
    rng_s ^= rng_s << 13;
    rng_s ^= rng_s >> 7;
    rng_s ^= rng_s << 17;
    return (uint32_t)(rng_s >> 11);
}

static int rnd_range(int lo, int hi) /* inclusive */
{
    return lo + (int)(rnd() % (uint32_t)(hi - lo + 1));
}

/* ---- plausible input ---- */

typedef struct PadGen {
    int seg;          /* frames left in this segment */
    uint8_t dir;
    int mode;         /* 0 none, 1 mash A, 2 mash B, 3 hold A, 4 hold B, 5 alternate A/B, 6 mash both */
    int period, phase;
    int start_in;     /* >0: press Start when it reaches 1 (unpause) */
    int idle;         /* this pad does nothing in this burst */
} PadGen;

static const uint8_t DIRS[] = {0, PAD_RIGHT, PAD_RIGHT, PAD_RIGHT, PAD_LEFT, PAD_UP, PAD_DOWN,
                               PAD_RIGHT | PAD_UP, PAD_RIGHT | PAD_DOWN, PAD_LEFT | PAD_UP, PAD_LEFT | PAD_DOWN,
                               PAD_UP, PAD_DOWN, 0};

static uint8_t pad_next(PadGen *g)
{
    uint8_t p = 0;
    if (g->idle)
        return 0;
    if (g->seg <= 0) {
        g->seg = rnd_range(4, 70);
        g->dir = DIRS[rnd() % sizeof DIRS];
        g->mode = (int)(rnd() % 7);
        g->period = rnd_range(2, 12);
        g->phase = 0;
        if (rnd() % 400 == 0 && !g->start_in) { /* pause, unpause later */
            g->start_in = rnd_range(20, 150);
            return PAD_START;
        }
        if (rnd() % 300 == 0)
            return PAD_SELECT;
    }
    g->seg--;
    g->phase++;
    if (g->start_in > 0 && --g->start_in == 0)
        return PAD_START;
    p = g->dir;
    switch (g->mode) {
    case 1: if (g->phase % g->period == 0) p |= PAD_A; break;
    case 2: if (g->phase % g->period == 0) p |= PAD_B; break;
    case 3: p |= PAD_A; break;
    case 4: p |= PAD_B; break;
    case 5: p |= (g->phase / g->period) & 1 ? PAD_A : PAD_B; break;
    case 6: if (g->phase % g->period == 0) p |= PAD_A | PAD_B; break;
    default: break;
    }
    return p;
}

/* ---- main ---- */

static int load_list(const char *path, char ***out)
{
    FILE *f = fopen(path, "r");
    char line[2048];
    int n = 0, cap = 0;
    char **v = NULL;
    if (!f)
        return -1;
    while (fgets(line, sizeof line, f)) {
        size_t l = strlen(line);
        while (l && (line[l - 1] == '\n' || line[l - 1] == '\r' || line[l - 1] == ' '))
            line[--l] = 0;
        if (!l)
            continue;
        if (n == cap) {
            cap = cap ? cap * 2 : 256;
            v = realloc(v, (size_t)cap * sizeof *v);
        }
        v[n] = malloc(l + 1);
        memcpy(v[n], line, l + 1);
        n++;
    }
    fclose(f);
    *out = v;
    return n;
}

static void write_outputs(Coverage *cov, const char *prefix)
{
    static uint8_t m[COV_PRG_SIZE];
    char p[1024];
    FILE *f;
    long i;
    snprintf(p, sizeof p, "%s.cdl", prefix);
    cov_write_fceux(cov, p);
    for (i = 0; i < COV_PRG_SIZE; i++)
        m[i] = (uint8_t)((cov->fceux[i] & 0x11 ? MESEN_CODE : 0) | (cov->fceux[i] & 0x22 ? MESEN_DATA : 0) |
                         (cov->mesen[i] & (MESEN_JUMP | MESEN_ENTRY)));
    snprintf(p, sizeof p, "%s-m.cdl", prefix);
    f = fopen(p, "wb");
    if (f) {
        uint8_t hdr[9] = {'C', 'D', 'L', 'v', '2'};
        hdr[5] = (uint8_t)cov->prg_crc;
        hdr[6] = (uint8_t)(cov->prg_crc >> 8);
        hdr[7] = (uint8_t)(cov->prg_crc >> 16);
        hdr[8] = (uint8_t)(cov->prg_crc >> 24);
        fwrite(hdr, 1, 9, f);
        fwrite(m, 1, COV_PRG_SIZE, f);
        fclose(f);
    }
}

int main(int argc, char **argv)
{
    static Rom rom;
    static Nes work;
    static Coverage cov;
    static uint8_t allowed[0x800], save_f[COV_PRG_SIZE], save_m[COV_PRG_SIZE];
    const char *rom_path = NULL, *list_path = NULL, *prefix = NULL, *known_list = NULL, *ram_pcs = NULL;
    int worker = 0, workers = 1, qcap = 300, save_finds = 50, saved = 0, bmin = 60, bmax = 600;
    double seconds = 60, god_ratio = 0.5, death_ratio = 0.05;
    unsigned long long seed = 1;
    char **list = NULL, hash[41], p[1024];
    int nlist, i, nq = 0;
    State **q;
    FILE *finds;
    clock_t t0;
    long bursts = 0, kept = 0, crashes = 0;
    unsigned long long frames_run = 0;

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--snaps") && i + 1 < argc) list_path = argv[++i];
        else if (!strcmp(argv[i], "--out") && i + 1 < argc) prefix = argv[++i];
        else if (!strcmp(argv[i], "--worker") && i + 1 < argc) worker = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--workers") && i + 1 < argc) workers = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--seconds") && i + 1 < argc) seconds = atof(argv[++i]);
        else if (!strcmp(argv[i], "--seed") && i + 1 < argc) seed = strtoull(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--known") && i + 1 < argc) known_list = argv[++i];
        else if (!strcmp(argv[i], "--ram-pcs") && i + 1 < argc) ram_pcs = argv[++i];
        else if (!strcmp(argv[i], "--god-ratio") && i + 1 < argc) god_ratio = atof(argv[++i]);
        else if (!strcmp(argv[i], "--death-ratio") && i + 1 < argc) death_ratio = atof(argv[++i]);
        else if (!strcmp(argv[i], "--queue") && i + 1 < argc) qcap = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--save-finds") && i + 1 < argc) save_finds = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--burst") && i + 1 < argc) {
            if (sscanf(argv[++i], "%d,%d", &bmin, &bmax) != 2 || bmin < 1 || bmax < bmin) return 2;
        } else if (argv[i][0] != '-' && !rom_path) rom_path = argv[i];
        else {
            fprintf(stderr, "btdd_explore: bad argument %s (see the header of src/explore_main.c)\n", argv[i]);
            return 2;
        }
    }
    if (!rom_path || !list_path || !prefix || workers < 1 || worker < 0 || worker >= workers) {
        fprintf(stderr, "usage: btdd_explore ROM --snaps LIST --out PREFIX [options]\n");
        return 2;
    }
    if (rom_load(rom_path, &rom, hash) != ROM_OK) {
        fprintf(stderr, "ROM rejected\n");
        return 1;
    }
    rng_s = seed * 0x9E3779B97F4A7C15ull + (unsigned long long)worker * 0xD1B54A32D192ED03ull + 1;
    nes_init(&work, &rom);
    nes_power(&work);
    cov_init(&cov, &rom);
    cov_attach(&cov, &work);
    work.log_traps = 0;
    if (ram_pcs) {
        if (cov_load_ram_pcs(allowed, ram_pcs) != 0) {
            fprintf(stderr, "cannot read %s\n", ram_pcs);
            return 1;
        }
        cov.ram_pc_allowed = allowed;
    }
    if (known_list) { /* novelty is measured against every log listed there */
        char **kl;
        int nk = load_list(known_list, &kl), k;
        for (k = 0; k < nk; k++)
            if (cov_merge_file(&cov, kl[k]) != 0)
                fprintf(stderr, "warning: cannot merge %s\n", kl[k]);
    }
    nlist = load_list(list_path, &list);
    if (nlist <= 0) {
        fprintf(stderr, "no snapshots in %s\n", list_path);
        return 1;
    }
    q = calloc((size_t)qcap, sizeof *q);
    for (i = worker; i < nlist && nq < qcap; i += workers) {
        State *s = malloc(sizeof *s);
        nes_init(&s->nes, &rom);
        if (snap_load(&s->nes, NULL, list[i]) != 0) {
            fprintf(stderr, "cannot load %s\n", list[i]);
            free(s);
            continue;
        }
        s->origin = i;
        s->depth = 0;
        s->tries = 0;
        q[nq++] = s;
    }
    if (!nq) {
        fprintf(stderr, "worker %d: no snapshots\n", worker);
        return 1;
    }
    snprintf(p, sizeof p, "%s-finds.tsv", prefix);
    finds = fopen(p, "w");
    if (!finds) {
        fprintf(stderr, "cannot write %s\n", p);
        return 1;
    }
    fprintf(finds, "burst\torigin_snapshot\tdepth\tframes\tgod\tnew_code\tnew_data\tlevel\tsection\tsaved_as\n");

    t0 = clock();
    while ((double)(clock() - t0) / CLOCKS_PER_SEC < seconds) {
        State *root;
        PadGen g[2];
        int len = rnd_range(bmin, bmax), god = (double)(rnd() % 1000) < god_ratio * 1000, f, k;
        int kill = !god && (double)(rnd() % 1000) < death_ratio * 1000;
        uint64_t c0, d0;
        /* half the time any state, else the later of two random picks (finds come later in the queue) */
        if (rnd() & 1)
            root = q[rnd() % (uint32_t)nq];
        else {
            int a = (int)(rnd() % (uint32_t)nq), b = (int)(rnd() % (uint32_t)nq);
            root = q[a > b ? a : b];
        }
        root->tries++;
        snap_copy(&work, &root->nes);
        cov_reset_tracking(&cov);
        memcpy(save_f, cov.fceux, COV_PRG_SIZE);
        memcpy(save_m, cov.mesen, COV_PRG_SIZE);
        c0 = cov.new_code;
        d0 = cov.new_data;
        cov.bad_exec = 0;
        memset(g, 0, sizeof g);
        g[1].idle = (rnd() % 4) == 0;
        if (kill) { /* forced deaths: no lives left */
            work.ram[0x11] = work.ram[0x12] = 0;
        }
        for (f = 0; f < len; f++) {
            nes_set_pads(&work, pad_next(&g[0]), pad_next(&g[1]));
            nes_run_frame(&work);
            if (god) {
                for (k = 0; k < 2; k++) {
                    if (work.ram[0x51B + k] > 0 && work.ram[0x51B + k] < 47)
                        work.ram[0x51B + k] = 47;
                    if (work.ram[0x11 + k] < 3)
                        work.ram[0x11 + k] = 3;
                }
            }
            if (work.cpu.jammed || cov.bad_exec)
                break;
        }
        frames_run += (unsigned long long)f;
        bursts++;
        if (work.cpu.jammed || cov.bad_exec) {
            memcpy(cov.fceux, save_f, COV_PRG_SIZE);
            memcpy(cov.mesen, save_m, COV_PRG_SIZE);
            cov.new_code = c0;
            cov.new_data = d0;
            crashes++;
            continue;
        }
        if (cov.new_code > c0 || cov.new_data > d0) {
            State *s;
            char rs[64] = "-";
            if (nq < qcap) {
                s = malloc(sizeof *s);
                nes_init(&s->nes, &rom);
                q[nq++] = s;
            } else { /* replace a random explored state (never an original snapshot of this worker) */
                int idx, tries = 0;
                do
                    idx = (int)(rnd() % (uint32_t)nq);
                while (q[idx]->depth == 0 && ++tries < 50);
                s = q[idx];
            }
            snap_copy(&s->nes, &work);
            s->origin = root->origin;
            s->depth = root->depth + 1;
            s->tries = 0;
            kept++;
            if (saved < save_finds && cov.new_code > c0) {
                SnapInfo si;
                si.frame = 0;
                si.level = work.ram[0x0D];
                si.section = work.ram[0x15];
                si.reason = 0;
                snprintf(rs, sizeof rs, "w%d-%05ld.snap", worker, bursts);
                snprintf(p, sizeof p, "%s-finds/%s", prefix, rs);
                if (snap_save(&work, &si, p) == 0)
                    saved++;
                else
                    snprintf(rs, sizeof rs, "-");
            }
            fprintf(finds, "%ld\t%s\t%d\t%d\t%d\t%llu\t%llu\t%02X\t%02X\t%s\n", bursts, list[root->origin], s->depth, f,
                    god, (unsigned long long)(cov.new_code - c0), (unsigned long long)(cov.new_data - d0),
                    work.ram[0x0D], work.ram[0x15], rs);
            fflush(finds);
        }
        if (bursts % 500 == 0)
            write_outputs(&cov, prefix);
    }
    write_outputs(&cov, prefix);
    fclose(finds);
    printf("worker %d: %ld bursts, %llu frames, %ld kept, %ld crashes, new code %llu, new data %llu, %.0f s\n", worker,
           bursts, frames_run, kept, crashes, (unsigned long long)cov.new_code, (unsigned long long)cov.new_data,
           (double)(clock() - t0) / CLOCKS_PER_SEC);
    return 0;
}
