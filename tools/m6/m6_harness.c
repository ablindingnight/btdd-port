/* M6 differential test harness: linked with one candidate (a C `routine(M6 *m)`, see m6_api.h) by
 * tools/m6/m6_test.py.
 *
 *   m6_harness ROM BANK ADDR OUTPUTS corpus.m6c [more.m6c ...]
 *     OUTPUTS: comma list of a,x,y,c,z,n,v the routine returns (or "-" for none)
 *
 * For every corpus sample:
 *   1. expected: the original 6502 code runs on the reference interpreter in a sandbox (RAM + mapped ROM bank;
 *      hardware reads answer with the values recorded live, in order) from the entry state to its final RTS.
 *      The sandbox must reproduce the live run (same hardware accesses, same registers, same RAM hash), else the
 *      sample is reported as unusable and skipped;
 *   2. got: the candidate runs from the same state with the same recorded hardware reads;
 *   3. compared: the ordered hardware writes (and that reads happen in the same order), all RAM except the free
 *      stack area below S, and the declared output registers.
 * Prints "PASS n/n" or the first mismatch in one compact line (state id, what, address, expected/got, entry
 * registers): that line is what goes back to the model. Exit 0 = pass, 1 = mismatch, 2 = setup problem.
 */
#include "m6_api.h"
#include "m6_corpus.h"
#include "rom/rom.h"
#include "runtime/cpu.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static Rom rom;

typedef struct Box {
    uint8_t ram[2048];
    int bank;
    const M6Sample *s;
    int pos;        /* next recorded access */
    char err[256];  /* first deviation from the recording */
    M6Sample *rec;  /* fuzz: record instead of replay (hardware reads answer with random bytes) */
} Box;

static uint64_t rng = 0x2545F4914F6CDD1Dull;
static uint8_t rnd8(void)
{
    rng ^= rng << 13;
    rng ^= rng >> 7;
    rng ^= rng << 17;
    return (uint8_t)(rng >> 24);
}

static void deviate(Box *b, const char *fmt, unsigned a1, unsigned a2, unsigned a3)
{
    if (!b->err[0])
        snprintf(b->err, sizeof b->err, fmt, a1, a2, a3);
}

static uint8_t box_read(Box *b, uint16_t addr)
{
    const M6Acc *e;
    if (addr < 0x2000)
        return b->ram[addr & 0x7FF];
    if (addr >= 0x8000)
        return rom_read(&rom, b->bank, addr);
    if (b->rec) {
        uint8_t v = rnd8();
        if (b->rec->n_acc < M6_MAX_ACC) {
            M6Acc *n = &b->rec->acc[b->rec->n_acc++];
            n->addr = addr;
            n->value = v;
            n->kind = M6_READ;
        } else
            deviate(b, "too many hardware accesses", 0, 0, 0);
        return v;
    }
    if (b->pos >= b->s->n_acc) {
        deviate(b, "extra read of $%04X (the original made no further hardware access)", addr, 0, 0);
        return 0;
    }
    e = &b->s->acc[b->pos++];
    if (e->kind != M6_READ || e->addr != addr)
        deviate(b, "read of $%04X where the original's next hardware access was $%04X (1 = read, 2 = write: %u)", addr, e->addr,
                e->kind == M6_READ ? 1u : 2u);
    return e->value;
}

static void box_write(Box *b, uint16_t addr, uint8_t v)
{
    const M6Acc *e;
    if (addr < 0x2000) {
        b->ram[addr & 0x7FF] = v;
        return;
    }
    if (b->rec) {
        if (b->rec->n_acc < M6_MAX_ACC) {
            M6Acc *n = &b->rec->acc[b->rec->n_acc++];
            n->addr = addr;
            n->value = v;
            n->kind = M6_WRITE;
        } else
            deviate(b, "too many hardware accesses", 0, 0, 0);
    } else if (b->pos >= b->s->n_acc)
        deviate(b, "extra write $%04X = $%02X (the original made no further hardware access)", addr, v, 0);
    else {
        e = &b->s->acc[b->pos++];
        if (e->kind != M6_WRITE || e->addr != addr || e->value != v)
            deviate(b, "write $%04X = $%02X where the original wrote $%02X", addr, v, e->value);
    }
    if (addr >= 0x4020)
        b->bank = v & 7;
}

/* ---- sandbox for the original code ---- */
static uint8_t cpu_rd(void *ctx, uint16_t addr) { return box_read(ctx, addr); }
static void cpu_wr(void *ctx, uint16_t addr, uint8_t v) { box_write(ctx, addr, v); }

static int run_original(Box *b, Cpu *c, uint16_t start)
{
    long steps;
    cpu_init(c, b, cpu_rd, cpu_wr, NULL);
    c->a = b->s->a;
    c->x = b->s->x;
    c->y = b->s->y;
    c->s = b->s->s;
    c->p = c->p_before = b->s->p;
    c->pc = start;
    c->irq = 0;
    for (steps = 0; steps < 2000000; steps++) {
        uint8_t op = c->pc >= 0x8000 ? rom_read(&rom, b->bank, c->pc) : b->ram[c->pc & 0x7FF];
        if (op == 0x60 && c->s == b->s->s)
            return 0;
        cpu_step(c);
        if (c->jammed)
            return 1;
    }
    return 1;
}

/* ---- candidate side ---- */
uint8_t bus_read(M6 *m, uint16_t addr) { return box_read(m->ctx, addr); }
void bus_write(M6 *m, uint16_t addr, uint8_t value) { box_write(m->ctx, addr, value); }

/* ---- corpus ---- */
static unsigned g16(const uint8_t *p) { return p[0] | p[1] << 8; }
static uint32_t g32(const uint8_t *p) { return g16(p) | (uint32_t)g16(p + 2) << 16; }

static uint8_t *load(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    uint8_t *buf;
    long n;
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    buf = malloc((size_t)n);
    if (buf && fread(buf, 1, (size_t)n, f) != (size_t)n) {
        free(buf);
        buf = NULL;
    }
    fclose(f);
    *len = (size_t)n;
    return buf;
}

static M6Sample smp;
static Box exp_box, got_box;
static int outputs[7];
static unsigned routine_addr;

/* Checks the candidate on smp. Returns 0 pass, 1 mismatch (printed), 2 unusable sample. */
static int check(const char *label, int fuzz)
{
    Cpu cpu;
    M6 m;
    int k, bad = 0, ram_diff = -1;
    memset(&exp_box, 0, sizeof exp_box);
    memcpy(exp_box.ram, smp.ram, 2048);
    exp_box.bank = smp.bank;
    exp_box.s = &smp;
    if (fuzz) {
        smp.n_acc = 0;
        exp_box.rec = &smp;
        if (run_original(&exp_box, &cpu, (uint16_t)routine_addr) != 0 || exp_box.err[0])
            return 2;
    } else if (run_original(&exp_box, &cpu, (uint16_t)routine_addr) != 0 || exp_box.err[0] ||
               exp_box.pos != smp.n_acc || cpu.a != smp.out_a || cpu.x != smp.out_x || cpu.y != smp.out_y ||
               cpu.p != smp.out_p || m6_ram_hash(exp_box.ram, smp.s) != smp.out_ram_hash)
        return 2;
    if (getenv("M6_VALIDATE")) /* corpus check only: does the sandbox reproduce the live run? */
        return 0;
    memset(&got_box, 0, sizeof got_box);
    memcpy(got_box.ram, smp.ram, 2048);
    got_box.bank = smp.bank;
    got_box.s = &smp;
    m.a = smp.a; m.x = smp.x; m.y = smp.y;
    m.c = smp.p & 1; m.z = (smp.p >> 1) & 1; m.v = (smp.p >> 6) & 1; m.n = (smp.p >> 7) & 1;
    m.ram = got_box.ram;
    m.ctx = &got_box;
    routine(&m);
    if (!got_box.err[0] && got_box.pos != smp.n_acc)
        snprintf(got_box.err, sizeof got_box.err, "missing hardware access: original next %s $%04X",
                 smp.acc[got_box.pos].kind == M6_READ ? "reads" : "writes", smp.acc[got_box.pos].addr);
    if (got_box.err[0])
        bad = 1;
    for (k = 0; k < 2048 && !bad; k++) {
        if (k >= 0x100 && k <= 0x100 + smp.s)
            continue;
        if (exp_box.ram[k] != got_box.ram[k]) {
            ram_diff = k;
            bad = 1;
        }
    }
    if (!bad) {
        const unsigned ev[7] = {cpu.a, cpu.x, cpu.y, cpu.p & 1u, (cpu.p >> 1) & 1u, (cpu.p >> 7) & 1u,
                                (cpu.p >> 6) & 1u};
        const unsigned gv[7] = {m.a, m.x, m.y, m.c != 0, m.z != 0, m.n != 0, m.v != 0};
        const char *rn[7] = {"A", "X", "Y", "C", "Z", "N", "V"};
        for (k = 0; k < 7; k++)
            if (outputs[k] && ev[k] != gv[k]) {
                snprintf(got_box.err, sizeof got_box.err, "output %s expected $%02X got $%02X", rn[k], ev[k], gv[k]);
                bad = 1;
                break;
            }
    }
    if (bad) {
        char what[300];
        if (ram_diff >= 0) {
            int extra = 0, k2;
            for (k2 = ram_diff + 1; k2 < 2048; k2++)
                if (!(k2 >= 0x100 && k2 <= 0x100 + smp.s) && exp_box.ram[k2] != got_box.ram[k2])
                    extra++;
            snprintf(what, sizeof what, "RAM $%04X expected $%02X got $%02X (+%d more bytes differ)", ram_diff,
                     exp_box.ram[ram_diff], got_box.ram[ram_diff], extra);
        } else
            snprintf(what, sizeof what, "%s", got_box.err);
        printf("FAIL %s: %s | entry A=$%02X X=$%02X Y=$%02X C=%d Z=%d N=%d V=%d bank %d", label, what, smp.a, smp.x,
               smp.y, smp.p & 1, (smp.p >> 1) & 1, (smp.p >> 7) & 1, (smp.p >> 6) & 1, smp.bank);
        if (ram_diff >= 0 && fuzz)
            printf(" (random entry RAM; entry value at $%04X was $%02X)", ram_diff, smp.ram[ram_diff]);
        if (getenv("M6_SHOW") && *getenv("M6_SHOW")) { /* entry values of the RAM the routine names */
            const char *q = getenv("M6_SHOW");
            printf(" | entry RAM");
            while (*q) {
                char *e;
                unsigned a = (unsigned)strtoul(q, &e, 16);
                printf(" $%04X=$%02X", a, smp.ram[a & 0x7FF]);
                if (*e != ',')
                    break;
                q = e + 1;
            }
        }
        printf("\n");
        return 1;
    }
    return 0;
}

int main(int argc, char **argv)
{
    char hash[41], label[600];
    int f, total = 0, passed = 0, unusable = 0, fuzz_n = getenv("M6_FUZZ") ? atoi(getenv("M6_FUZZ")) : 0;
    int fuzz_done = 0, fuzz_skip = 0, k;
    uint8_t first_bank = 0, first_s = 0xFD;
    if (argc < 6) {
        fprintf(stderr, "usage: m6_harness ROM BANK ADDR OUTPUTS corpus.m6c...   (env M6_FUZZ=n, M6_VALIDATE=1)\n");
        return 2;
    }
    if (rom_load(argv[1], &rom, hash) != ROM_OK) {
        fprintf(stderr, "ROM rejected\n");
        return 2;
    }
    first_bank = (uint8_t)atoi(argv[2]);
    routine_addr = (unsigned)strtoul(argv[3], NULL, 16);
    {
        const char *names[7] = {"a", "x", "y", "c", "z", "n", "v"};
        for (k = 0; k < 7; k++)
            outputs[k] = strcmp(argv[4], "-") != 0 && strstr(argv[4], names[k]) != NULL;
    }
    for (f = 5; f < argc; f++) {
        size_t len, off = 12;
        uint8_t *buf = load(argv[f], &len);
        uint32_t count, i;
        if (!buf || len < 12 || memcmp(buf, "M6C1", 4) != 0) {
            fprintf(stderr, "bad corpus %s\n", argv[f]);
            return 2;
        }
        count = g32(buf + 4);
        for (i = 0; i < count && off + 2064 <= len; i++) {
            const uint8_t *p = buf + off;
            int r;
            smp.a = p[0]; smp.x = p[1]; smp.y = p[2]; smp.s = p[3]; smp.p = p[4]; smp.bank = p[5];
            smp.out_a = p[6]; smp.out_x = p[7]; smp.out_y = p[8]; smp.out_p = p[9];
            smp.n_acc = (uint16_t)g16(p + 10);
            smp.out_ram_hash = g32(p + 12);
            memcpy(smp.ram, p + 16, 2048);
            for (k = 0; k < smp.n_acc; k++) {
                const uint8_t *q = p + 16 + 2048 + 4 * k;
                smp.acc[k].addr = (uint16_t)g16(q);
                smp.acc[k].value = q[2];
                smp.acc[k].kind = q[3];
            }
            off += 16 + 2048 + 4 * (size_t)smp.n_acc;
            if (total == 0) {
                first_bank = smp.bank;
                first_s = smp.s;
            }
            total++;
            {
                const char *base = strrchr(argv[f], '/'), *b2 = strrchr(argv[f], '\\');
                if (b2 > base)
                    base = b2;
                snprintf(label, sizeof label, "state %d (%s #%u)", total, base ? base + 1 : argv[f], i);
            }
            r = check(label, 0);
            if (r == 1) {
                printf("checked %d states before the failure (%d passed, %d unusable)\n", total, passed, unusable);
                return 1;
            }
            if (r == 2)
                unusable++;
            else
                passed++;
        }
        free(buf);
    }
    /* fuzz: random registers, flags and RAM; hardware reads answer random bytes, replayed to the candidate */
    for (k = 0; k < fuzz_n; k++) {
        int j, r;
        smp.a = rnd8(); smp.x = rnd8(); smp.y = rnd8(); smp.p = (uint8_t)((rnd8() & 0xCF) | 0x20);
        smp.s = first_s;
        smp.bank = first_bank;
        for (j = 0; j < 2048; j++)
            smp.ram[j] = rnd8();
        snprintf(label, sizeof label, "random state %d", k + 1);
        r = check(label, 1);
        if (r == 1) {
            printf("after %d corpus states passed\n", passed);
            return 1;
        }
        if (r == 2)
            fuzz_skip++;
        else
            fuzz_done++;
    }
    printf("PASS %d/%d states (%d unusable samples skipped)", passed, total, unusable);
    if (fuzz_n)
        printf(" + %d/%d random states (%d did not finish)", fuzz_done, fuzz_n, fuzz_skip);
    printf("\n");
    return passed > 0 ? 0 : 2;
}
