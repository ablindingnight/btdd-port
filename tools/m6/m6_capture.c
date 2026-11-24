/* m6_capture: builds the M6 entry-state corpus from a TAS replay on the reference interpreter.
 *
 *   m6_capture ROM --movie X.fm2 [--ppu old|new] --routines list.txt --out DIR [--max N] [--frames N]
 *
 * list.txt: one routine per line, "bank addr" in hex (e.g. "0 8b5f"). For every entry into a listed routine
 * (its first instruction starts executing in its bank), the registers and RAM are taken, and every hardware access
 * ($2000-$7FFF reads, $2000+ writes) is logged until the RTS that brings S back above the return address. Samples
 * during which an NMI started, or that leave without that RTS (stack tricks), are dropped. Per routine, up to N
 * distinct samples (default 2000) are kept by reservoir sampling over the whole movie, so later stages are
 * represented too. Output: DIR/b<bank>_<addr>.m6c (format in m6_corpus.h). ROM-derived: never inside the repo.
 */
#include "m6_corpus.h"
#include "nes/nes.h"
#include "rom/rom.h"
#include "runtime/fm2.h"
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_ROUTINES 256
#define MAX_DEPTH 24

typedef struct Routine {
    int bank, addr;
    M6Sample **keep;     /* reservoir; each sample allocated at its real size */
    uint64_t *hash;      /* hash of each kept sample */
    uint64_t *set;       /* open-addressing set of kept hashes (0 empty, 1 deleted) */
    int set_cap, set_dead;
    int kept;
    uint64_t seen, dropped_nmi, dropped_unwind, dropped_long;
} Routine;

typedef struct Frame {
    int r;
    int tainted;
    M6Sample *s;
} Frame;

static Nes nes;
static Routine routines[MAX_ROUTINES];
static int n_routines, max_keep = 2000;
static int16_t lookup[8][0x8000]; /* routine index + 1 */
static uint8_t watch_all[0x10000];
static Frame stack[MAX_DEPTH];
static int depth;
static M6Sample pool[MAX_DEPTH];
static CpuRead orig_read;
static CpuWrite orig_write;
static uint64_t rng = 0x9E3779B97F4A7C15ull;

static uint64_t next_rand(void)
{
    rng ^= rng << 13;
    rng ^= rng >> 7;
    rng ^= rng << 17;
    return rng;
}

static uint64_t sample_hash(const M6Sample *s)
{
    uint64_t h = 1469598103934665603ull;
    const uint8_t *p = (const uint8_t *)s;
    size_t n = offsetof(M6Sample, acc) + (size_t)s->n_acc * sizeof(M6Acc), i;
    for (i = 0; i < n; i++)
        h = (h ^ p[i]) * 1099511628211ull;
    return h;
}

static void log_acc(uint16_t addr, uint8_t v, uint8_t kind)
{
    int i;
    for (i = 0; i < depth; i++) {
        M6Sample *s = stack[i].s;
        if (stack[i].tainted)
            continue;
        if (s->n_acc >= M6_MAX_ACC) {
            stack[i].tainted = 3;
            continue;
        }
        s->acc[s->n_acc].addr = addr;
        s->acc[s->n_acc].value = v;
        s->acc[s->n_acc].kind = kind;
        s->n_acc++;
    }
}

static uint8_t rd(void *ctx, uint16_t addr)
{
    uint8_t v = orig_read(ctx, addr);
    if (depth && addr >= 0x2000 && addr < 0x8000)
        log_acc(addr, v, M6_READ);
    return v;
}

static void wr(void *ctx, uint16_t addr, uint8_t v)
{
    if (depth && addr >= 0x2000)
        log_acc(addr, v, M6_WRITE);
    orig_write(ctx, addr, v);
}

static uint64_t fix_hash(uint64_t h) { return h < 2 ? h + 2 : h; }

static int set_find(const Routine *r, uint64_t h)
{
    int i = (int)(h & (uint64_t)(r->set_cap - 1));
    while (r->set[i]) {
        if (r->set[i] == h)
            return i;
        i = (i + 1) & (r->set_cap - 1);
    }
    return -1;
}

static void set_add(Routine *r, uint64_t h)
{
    int i = (int)(h & (uint64_t)(r->set_cap - 1));
    while (r->set[i] > 1)
        i = (i + 1) & (r->set_cap - 1);
    r->set[i] = h;
}

static void set_rebuild(Routine *r)
{
    int i;
    memset(r->set, 0, sizeof(uint64_t) * (size_t)r->set_cap);
    for (i = 0; i < r->kept; i++)
        set_add(r, r->hash[i]);
    r->set_dead = 0;
}

static void finish(Frame *f, Cpu *c)
{
    Routine *r = &routines[f->r];
    M6Sample *s = f->s;
    uint64_t h;
    int i;
    if (f->tainted) {
        if (f->tainted == 1)
            r->dropped_nmi++;
        else if (f->tainted == 2)
            r->dropped_unwind++;
        else
            r->dropped_long++;
        return;
    }
    s->out_a = c->a;
    s->out_x = c->x;
    s->out_y = c->y;
    s->out_p = c->p;
    s->out_ram_hash = m6_ram_hash(nes.ram, s->s);
    h = fix_hash(sample_hash(s));
    if (set_find(r, h) >= 0)
        return; /* duplicate of a kept sample */
    r->seen++;
    if (r->kept < max_keep) {
        i = r->kept++;
    } else {
        uint64_t j = next_rand() % r->seen;
        if (j >= (uint64_t)max_keep)
            return;
        i = (int)j;
        r->set[set_find(r, r->hash[i])] = 1; /* the replaced sample leaves the set */
        if (++r->set_dead > max_keep)
            set_rebuild(r);
    }
    {
        size_t sz = offsetof(M6Sample, acc) + (size_t)s->n_acc * sizeof(M6Acc);
        free(r->keep[i]);
        r->keep[i] = malloc(sz);
        if (!r->keep[i]) {
            fprintf(stderr, "out of memory\n");
            exit(1);
        }
        memcpy(r->keep[i], s, sz);
    }
    r->hash[i] = h;
    set_add(r, h);
}

static void hook(void *ctx, Cpu *c)
{
    Nes *n = ctx;
    int idx;
    uint16_t nmi = (uint16_t)(rom_read(n->rom, n->prg_bank, 0xFFFA) | rom_read(n->rom, n->prg_bank, 0xFFFB) << 8);
    /* returns and unwinds of the innermost frames. A frame ends at its own RTS: the instruction about to run is
     * an RTS and S is at the entry level (the return address is the next thing on the stack). Taking the state
     * here, before the RTS, keeps an interrupt right after the return out of the sample (RTS touches no
     * hardware and changes no register or flag). */
    while (depth) {
        Frame *f = &stack[depth - 1];
        uint8_t d = (uint8_t)(c->s - f->s->s);
        uint8_t op = c->pc >= 0x8000 ? rom_read(n->rom, n->prg_bank, c->pc) : n->ram[c->pc & 0x7FF];
        if (d == 0 && op == 0x60) {
            finish(f, c);
            depth--;
        } else if (d >= 1 && d < 0x80) {
            if (!f->tainted)
                f->tainted = 2; /* the return address was removed without an RTS */
            finish(f, c);
            depth--;
        } else
            break;
    }
    if (depth && c->pc == nmi) {
        int i;
        for (i = 0; i < depth; i++)
            if (!stack[i].tainted)
                stack[i].tainted = 1;
    }
    if (c->pc < 0x8000)
        return;
    idx = lookup[n->prg_bank][c->pc - 0x8000];
    if (idx && depth < MAX_DEPTH) {
        Frame *f = &stack[depth];
        M6Sample *s = &pool[depth];
        depth++;
        f->r = idx - 1;
        f->tainted = 0;
        f->s = s;
        s->a = c->a;
        s->x = c->x;
        s->y = c->y;
        s->s = c->s;
        s->p = c->p;
        s->bank = n->prg_bank;
        s->n_acc = 0;
        memcpy(s->ram, n->ram, 2048);
    }
}

static void put16(FILE *f, unsigned v) { fputc(v & 0xFF, f); fputc((v >> 8) & 0xFF, f); }
static void put32(FILE *f, uint32_t v) { put16(f, v & 0xFFFF); put16(f, v >> 16); }

static int write_corpus(const char *dir, const Routine *r)
{
    char path[1024];
    FILE *f;
    int i, k;
    snprintf(path, sizeof path, "%s/b%d_%04x.m6c", dir, r->bank, r->addr);
    f = fopen(path, "wb");
    if (!f)
        return 1;
    fwrite("M6C1", 1, 4, f);
    put32(f, (uint32_t)r->kept);
    put16(f, (unsigned)r->addr);
    fputc(r->bank, f);
    fputc(0, f);
    for (i = 0; i < r->kept; i++) {
        const M6Sample *s = r->keep[i];
        fputc(s->a, f); fputc(s->x, f); fputc(s->y, f); fputc(s->s, f); fputc(s->p, f); fputc(s->bank, f);
        fputc(s->out_a, f); fputc(s->out_x, f); fputc(s->out_y, f); fputc(s->out_p, f);
        put16(f, s->n_acc);
        put32(f, s->out_ram_hash);
        fwrite(s->ram, 1, 2048, f);
        for (k = 0; k < s->n_acc; k++) {
            put16(f, s->acc[k].addr);
            fputc(s->acc[k].value, f);
            fputc(s->acc[k].kind, f);
        }
    }
    fclose(f);
    return 0;
}

int main(int argc, char **argv)
{
    static Rom rom;
    const char *movie = NULL, *list = NULL, *out = NULL, *ppu = "old";
    long frames = -1, i;
    char hash[41], line[256];
    Fm2 fm = {0};
    FILE *lf;
    int a;

    for (a = 2; a < argc; a++) {
        if (!strcmp(argv[a], "--movie") && a + 1 < argc) movie = argv[++a];
        else if (!strcmp(argv[a], "--routines") && a + 1 < argc) list = argv[++a];
        else if (!strcmp(argv[a], "--out") && a + 1 < argc) out = argv[++a];
        else if (!strcmp(argv[a], "--ppu") && a + 1 < argc) ppu = argv[++a];
        else if (!strcmp(argv[a], "--max") && a + 1 < argc) max_keep = atoi(argv[++a]);
        else if (!strcmp(argv[a], "--frames") && a + 1 < argc) frames = atol(argv[++a]);
        else { fprintf(stderr, "unknown option %s\n", argv[a]); return 2; }
    }
    if (argc < 2 || !movie || !list || !out) {
        fprintf(stderr, "usage: m6_capture ROM --movie X.fm2 [--ppu old|new] --routines list.txt --out DIR [--max N]\n");
        return 2;
    }
    if (rom_load(argv[1], &rom, hash) != ROM_OK) {
        fprintf(stderr, "ROM rejected\n");
        return 1;
    }
    lf = fopen(list, "r");
    if (!lf) {
        fprintf(stderr, "cannot read %s\n", list);
        return 1;
    }
    while (fgets(line, sizeof line, lf) && n_routines < MAX_ROUTINES) {
        unsigned b, ad;
        Routine *r;
        if (sscanf(line, "%x %x", &b, &ad) != 2 || b > 7 || ad < 0x8000 || ad > 0xFFFF)
            continue;
        r = &routines[n_routines];
        r->bank = (int)b;
        r->addr = (int)ad;
        r->keep = calloc((size_t)max_keep, sizeof *r->keep);
        r->hash = malloc(sizeof(uint64_t) * (size_t)max_keep);
        r->set_cap = 16;
        while (r->set_cap < 4 * max_keep)
            r->set_cap *= 2;
        r->set = calloc((size_t)r->set_cap, sizeof(uint64_t));
        if (!r->keep || !r->hash || !r->set) {
            fprintf(stderr, "out of memory (lower --max)\n");
            return 1;
        }
        lookup[b][ad - 0x8000] = (int16_t)(++n_routines);
    }
    fclose(lf);
    if (fm2_load(movie, &fm, line, sizeof line) != 0) {
        fprintf(stderr, "%s\n", line);
        return 1;
    }
    if (frames < 0)
        frames = fm.count;
    nes_init(&nes, &rom);
    nes_power(&nes, !strcmp(ppu, "new") ? PPU_MODEL_NEW : PPU_MODEL_OLD);
    orig_read = nes.cpu.read;
    orig_write = nes.cpu.write;
    nes.cpu.read = rd;
    nes.cpu.write = wr;
    memset(watch_all, 1, sizeof watch_all);
    nes.cpu.exec_watch = watch_all;
    nes.cpu.exec_hook = hook;
    for (i = 0; i < frames; i++) {
        const Fm2Frame *fr = i < fm.count ? &fm.frames[i] : NULL;
        nes_set_pads(&nes, fr ? fr->pad[0] : 0, fr ? fr->pad[1] : 0);
        nes_run_frame(&nes);
    }
    for (a = 0; a < n_routines; a++) {
        Routine *r = &routines[a];
        if (write_corpus(out, r) != 0) {
            fprintf(stderr, "cannot write into %s\n", out);
            return 1;
        }
        printf("b%d_%04x: %d kept of %llu distinct; dropped nmi %llu, unwind %llu, too long %llu\n", r->bank, r->addr,
               r->kept, (unsigned long long)r->seen, (unsigned long long)r->dropped_nmi,
               (unsigned long long)r->dropped_unwind, (unsigned long long)r->dropped_long);
    }
    fm2_free(&fm);
    return 0;
}
