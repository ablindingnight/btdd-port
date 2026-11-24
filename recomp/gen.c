/* btdd_gen: the M5 generator in C (same output as recomp/gen.py, byte for byte).
 *
 * The shipped exe cannot run Python, so the load-time recompiler needs this in C. It reads the player's ROM
 * (SHA-1 checked by rom_load and against the meta's `rom` record) and meta/btdd.meta, and writes
 * native_b0..7.c, native_all.c and skipped.txt into the output folder. See recomp/gen.py for the design notes.
 *
 *   btdd_gen ROM META OUTDIR [--conf verified,static] [--exclude-timing] [--exclude b:addr,...]
 */
#include "rom/rom.h"
#include "runtime/cpu.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- opcode table ---- */
enum { IMP, ACC, IMM, ZP, ZPX, ZPY, REL, INDX, INDY, ABS, ABSX, ABSY, IND };
static const int mode_size[] = {1, 1, 2, 2, 2, 2, 2, 2, 2, 3, 3, 3, 3};

typedef struct { const char *m; int mode; } Op;
static Op ops[256];

static void def(const char *m, int op, int mode)
{
    ops[op].m = m;
    ops[op].mode = mode;
}

static void alu(const char *m, int b)
{
    def(m, b + 0x09, IMM); def(m, b + 0x05, ZP); def(m, b + 0x15, ZPX); def(m, b + 0x0D, ABS);
    def(m, b + 0x1D, ABSX); def(m, b + 0x19, ABSY); def(m, b + 0x01, INDX); def(m, b + 0x11, INDY);
}

static void shift(const char *m, int b, int acc)
{
    if (acc)
        def(m, b + 0x0A, ACC);
    def(m, b + 0x06, ZP); def(m, b + 0x16, ZPX); def(m, b + 0x0E, ABS); def(m, b + 0x1E, ABSX);
}

static void init_ops(void)
{
    static const struct { const char *m; int op; } imp[] = {
        {"brk", 0x00}, {"clc", 0x18}, {"cld", 0xD8}, {"cli", 0x58}, {"clv", 0xB8}, {"dex", 0xCA}, {"dey", 0x88},
        {"inx", 0xE8}, {"iny", 0xC8}, {"nop", 0xEA}, {"pha", 0x48}, {"php", 0x08}, {"pla", 0x68}, {"plp", 0x28},
        {"rti", 0x40}, {"rts", 0x60}, {"sec", 0x38}, {"sed", 0xF8}, {"sei", 0x78}, {"tax", 0xAA}, {"tay", 0xA8},
        {"tsx", 0xBA}, {"txa", 0x8A}, {"txs", 0x9A}, {"tya", 0x98},
    };
    static const struct { const char *m; int op; } br[] = {
        {"bpl", 0x10}, {"bmi", 0x30}, {"bvc", 0x50}, {"bvs", 0x70}, {"bcc", 0x90}, {"bcs", 0xB0}, {"bne", 0xD0},
        {"beq", 0xF0},
    };
    size_t i;
    for (i = 0; i < sizeof imp / sizeof imp[0]; i++)
        def(imp[i].m, imp[i].op, IMP);
    for (i = 0; i < sizeof br / sizeof br[0]; i++)
        def(br[i].m, br[i].op, REL);
    alu("ora", 0x00); alu("and", 0x20); alu("eor", 0x40); alu("adc", 0x60); alu("cmp", 0xC0); alu("sbc", 0xE0);
    alu("lda", 0xA0);
    alu("sta", 0x80); ops[0x89].m = NULL; /* no sta #imm */
    shift("asl", 0x00, 1); shift("rol", 0x20, 1); shift("lsr", 0x40, 1); shift("ror", 0x60, 1);
    shift("dec", 0xC0, 0); shift("inc", 0xE0, 0);
    def("bit", 0x24, ZP); def("bit", 0x2C, ABS);
    def("cpx", 0xE0, IMM); def("cpx", 0xE4, ZP); def("cpx", 0xEC, ABS);
    def("cpy", 0xC0, IMM); def("cpy", 0xC4, ZP); def("cpy", 0xCC, ABS);
    def("jmp", 0x4C, ABS); def("jmp", 0x6C, IND); def("jsr", 0x20, ABS);
    def("ldx", 0xA2, IMM); def("ldx", 0xA6, ZP); def("ldx", 0xB6, ZPY); def("ldx", 0xAE, ABS); def("ldx", 0xBE, ABSY);
    def("ldy", 0xA0, IMM); def("ldy", 0xA4, ZP); def("ldy", 0xB4, ZPX); def("ldy", 0xAC, ABS); def("ldy", 0xBC, ABSX);
    def("stx", 0x86, ZP); def("stx", 0x96, ZPY); def("stx", 0x8E, ABS);
    def("sty", 0x84, ZP); def("sty", 0x94, ZPX); def("sty", 0x8C, ABS);
}

typedef struct { const char *m; int mode, n, v; } Insn;

static const uint8_t *mem;
static int cur_bank;

static int byte_at(int a) { return mem[a - 0x8000]; }

static int decode(int a, Insn *d)
{
    int o = a - 0x8000, op;
    if (o < 0 || o >= 0x8000)
        return 0;
    op = mem[o];
    if (!ops[op].m)
        return 0;
    d->m = ops[op].m;
    d->mode = ops[op].mode;
    d->n = mode_size[d->mode];
    if (o + d->n > 0x8000)
        return 0;
    if (d->n == 1)
        d->v = -1;
    else if (d->n == 2) {
        d->v = mem[o + 1];
        if (d->mode == REL)
            d->v = (a + 2 + (d->v >= 128 ? d->v - 256 : d->v)) & 0xFFFF;
    } else
        d->v = mem[o + 1] | mem[o + 2] << 8;
    return 1;
}

/* ---- growable text ---- */
typedef struct { char *s; size_t len, cap; } Buf;

static void bprintf(Buf *b, const char *fmt, ...)
{
    va_list ap;
    int n;
    for (;;) {
        size_t room = b->cap - b->len;
        va_start(ap, fmt);
        n = vsnprintf(b->s ? b->s + b->len : NULL, b->s ? room : 0, fmt, ap);
        va_end(ap);
        if (n < 0)
            exit(1);
        if (b->s && (size_t)n < room) {
            b->len += (size_t)n;
            return;
        }
        b->cap = b->cap * 2 + (size_t)n + 256;
        b->s = realloc(b->s, b->cap);
        if (!b->s)
            exit(1);
    }
}

static int is(const char *a, const char *b) { return strcmp(a, b) == 0; }

static int in_list(const char *m, const char *const *list)
{
    for (; *list; list++)
        if (is(m, *list))
            return 1;
    return 0;
}

static const char *const BRANCH[] = {"bpl", "bmi", "bvc", "bvs", "bcc", "bcs", "bne", "beq", NULL};
static const char *const COND[] = {"!(c->p & CPU_N)", "(c->p & CPU_N)", "!(c->p & CPU_V)", "(c->p & CPU_V)",
                                   "!(c->p & CPU_C)", "(c->p & CPU_C)", "!(c->p & CPU_Z)", "(c->p & CPU_Z)"};

static const char *alu_text(const char *m)
{
    static const char *const t[][2] = {
        {"ora", "c->a |= v; n_zn(c, c->a);"}, {"and", "c->a &= v; n_zn(c, c->a);"},
        {"eor", "c->a ^= v; n_zn(c, c->a);"}, {"adc", "n_adc(c, v);"}, {"sbc", "n_sbc(c, v);"},
        {"cmp", "n_cmp(c, c->a, v);"}, {"cpx", "n_cmp(c, c->x, v);"}, {"cpy", "n_cmp(c, c->y, v);"},
        {"bit", "n_bit(c, v);"}};
    size_t i;
    for (i = 0; i < sizeof t / sizeof t[0]; i++)
        if (is(m, t[i][0]))
            return t[i][1];
    return NULL;
}

static const char *implied_text(const char *m)
{
    static const char *const t[][2] = {
        {"tax", "c->x = c->a; n_zn(c, c->x);"}, {"txa", "c->a = c->x; n_zn(c, c->a);"},
        {"tay", "c->y = c->a; n_zn(c, c->y);"}, {"tya", "c->a = c->y; n_zn(c, c->a);"},
        {"tsx", "c->x = c->s; n_zn(c, c->x);"}, {"txs", "c->s = c->x;"},
        {"inx", "c->x++; n_zn(c, c->x);"}, {"dex", "c->x--; n_zn(c, c->x);"},
        {"iny", "c->y++; n_zn(c, c->y);"}, {"dey", "c->y--; n_zn(c, c->y);"},
        {"clc", "c->p &= (uint8_t)~CPU_C;"}, {"sec", "c->p |= CPU_C;"}, {"cli", "c->p &= (uint8_t)~CPU_I;"},
        {"sei", "c->p |= CPU_I;"}, {"clv", "c->p &= (uint8_t)~CPU_V;"}, {"cld", "c->p &= (uint8_t)~CPU_D;"},
        {"sed", "c->p |= CPU_D;"}, {"nop", ""},
        {"pha", "n_push(c, c->a);"}, {"php", "n_push(c, (uint8_t)(c->p | CPU_U | CPU_B));"},
        {"pla", "c->a = n_pop(c); n_zn(c, c->a);"}, {"plp", "c->p = n_pop(c);"}};
    size_t i;
    for (i = 0; i < sizeof t / sizeof t[0]; i++)
        if (is(m, t[i][0]))
            return t[i][1];
    return NULL;
}

static const char *rmw_fn(const char *m)
{
    static const char *const t[][2] = {{"asl", "n_asl"}, {"lsr", "n_lsr"}, {"rol", "n_rol"},
                                       {"ror", "n_ror"}, {"inc", "n_inc"}, {"dec", "n_dec"}};
    size_t i;
    for (i = 0; i < 6; i++)
        if (is(m, t[i][0]))
            return t[i][1];
    return NULL;
}

/* Each emitted instruction line is "    " + text + "\n". */
#define LINE(...) do { bprintf(out, "    "); bprintf(out, __VA_ARGS__); bprintf(out, "\n"); } while (0)

static void operand(Buf *out, const Insn *d, int lb)
{
    int v = d->v;
    char r = (d->mode == ZPX || d->mode == ABSX) ? 'x' : 'y';
    switch (d->mode) {
    case IMM: LINE("v = 0x%02X;", v); break;
    case ZP: LINE("c->bus = 0x%02X;", lb); LINE("v = n_rdram(c, 0x%02X);", v); break;
    case ZPX: case ZPY:
        LINE("c->bus = 0x%02X;", lb); LINE("v = n_rdram(c, (uint8_t)(0x%02X + c->%c));", v, r); break;
    case ABS: LINE("c->bus = 0x%02X;", lb); LINE("v = %s(c, 0x%04X);", v < 0x2000 ? "n_rdram" : "n_rd", v); break;
    case ABSX: case ABSY: {
        int ram = v + 0xFF < 0x2000;
        LINE("c->bus = 0x%02X;", lb);
        LINE("ea = (uint16_t)(0x%04X + c->%c);", v, r);
        LINE("if ((ea ^ 0x%04X) & 0x100) { %sn_cyc(c, 1); }", v, ram ? "" : "n_rd(c, (uint16_t)(ea ^ 0x100)); ");
        LINE("v = %s(c, ea);", ram ? "n_rdram" : "n_rdx");
        break;
    }
    case INDX:
        LINE("c->bus = 0x%02X;", lb); LINE("zp = (uint8_t)(0x%02X + c->x);", v);
        LINE("ea = n_rdram(c, zp); ea |= (uint16_t)(n_rdram(c, (uint8_t)(zp + 1)) << 8);");
        LINE("v = n_rdx(c, ea);");
        break;
    case INDY:
        LINE("c->bus = 0x%02X;", lb);
        LINE("base = n_rdram(c, 0x%02X); base |= (uint16_t)(n_rdram(c, 0x%02X) << 8);", v, (v + 1) & 0xFF);
        LINE("ea = (uint16_t)(base + c->y);");
        LINE("if ((ea ^ base) & 0x100) { n_rdx(c, (uint16_t)(ea ^ 0x100)); n_cyc(c, 1); }");
        LINE("v = n_rdx(c, ea);");
        break;
    }
}

/* returns static ea or -1; *ram = RAM-only access */
static int store_addr(Buf *out, const Insn *d, int lb, int *ram)
{
    int v = d->v;
    char r = (d->mode == ZPX || d->mode == ABSX) ? 'x' : 'y';
    LINE("c->bus = 0x%02X;", lb);
    switch (d->mode) {
    case ZP: *ram = 1; return v;
    case ZPX: case ZPY: *ram = 1; LINE("ea = (uint8_t)(0x%02X + c->%c);", v, r); return -1;
    case ABS: *ram = v < 0x2000; return v;
    case ABSX: case ABSY:
        *ram = v + 0xFF < 0x2000;
        LINE("ea = (uint16_t)(0x%04X + c->%c);", v, r);
        if (!*ram)
            LINE("n_rd(c, (uint16_t)((ea & 0x00FF) | 0x%04X));", v & 0xFF00);
        return -1;
    case INDX:
        *ram = 0;
        LINE("zp = (uint8_t)(0x%02X + c->x);", v);
        LINE("ea = n_rdram(c, zp); ea |= (uint16_t)(n_rdram(c, (uint8_t)(zp + 1)) << 8);");
        return -1;
    case INDY:
        *ram = 0;
        LINE("base = n_rdram(c, 0x%02X); base |= (uint16_t)(n_rdram(c, 0x%02X) << 8);", v, (v + 1) & 0xFF);
        LINE("ea = (uint16_t)(base + c->y);");
        LINE("n_rdx(c, (uint16_t)((ea & 0x00FF) | (base & 0xFF00)));");
        return -1;
    }
    *ram = 0;
    return -1;
}

static const uint8_t *in_routine; /* 64 KB map: 1 = instruction start of the routine being generated */

static void leave(Buf *out, char *dst, size_t cap, int t)
{
    (void)out;
    if (in_routine[t])
        snprintf(dst, cap, "goto L%04X;", t);
    else
        snprintf(dst, cap, "c->pc = 0x%04X; N_EXIT;", t);
}

/* Emits one instruction's lines; returns 0 if the interpreter must handle it. */
static int insn(Buf *out, int a, const Insn *d, int nxt)
{
    const char *m = d->m;
    int lb = byte_at(a + d->n - 1), v = d->v, ram, sea;
    char tmp[64];
    const char *t;
    if (is(m, "lda") || is(m, "ldx") || is(m, "ldy")) {
        operand(out, d, lb);
        LINE("c->%c = v; n_zn(c, v);", m[2]);
    } else if ((t = alu_text(m)) != NULL) {
        operand(out, d, lb);
        LINE("%s", t);
    } else if (is(m, "sta") || is(m, "stx") || is(m, "sty")) {
        char reg = m[2];
        sea = store_addr(out, d, lb, &ram);
        if (sea >= 0) {
            LINE("%s(c, 0x%04X, c->%c);", ram ? "n_wrram" : "n_wr", sea, reg);
            if (sea >= 0x4020)
                LINE("c->pc = 0x%04X; N_EXIT; /* mapper write: bank may change */", nxt);
        } else {
            LINE("%s(c, ea, c->%c);", ram ? "n_wrram" : "n_wrx", reg);
            if (!ram)
                LINE("if (NES(c)->prg_bank != %d) { c->pc = 0x%04X; N_EXIT; }", cur_bank, nxt);
        }
    } else if ((t = rmw_fn(m)) != NULL && d->mode == ACC) {
        LINE("c->a = %s(c, c->a);", t);
    } else if (t) {
        char e[16];
        sea = store_addr(out, d, lb, &ram);
        if (sea >= 0)
            snprintf(e, sizeof e, "0x%04X", sea);
        else
            snprintf(e, sizeof e, "ea");
        if (d->mode == ZP || d->mode == ZPX)
            LINE("v = n_rdram(c, %s); n_wrram(c, %s, %s(c, v));", e, e, t);
        else if (ram)
            LINE("v = n_rdram(c, %s); n_wrram(c, %s, v); n_wrram(c, %s, %s(c, v));", e, e, e, t);
        else {
            const char *rd_ = sea >= 0 ? "n_rd" : "n_rdx", *wr_ = sea >= 0 ? "n_wr" : "n_wrx";
            LINE("v = %s(c, %s); %s(c, %s, v); %s(c, %s, %s(c, v));", rd_, e, wr_, e, wr_, e, t);
            if (sea < 0 || sea >= 0x4020)
                LINE("if (NES(c)->prg_bank != %d) { c->pc = 0x%04X; N_EXIT; }", cur_bank, nxt);
        }
    } else if ((t = implied_text(m)) != NULL) {
        if (is(m, "pha") || is(m, "php") || is(m, "pla") || is(m, "plp"))
            LINE("c->bus = 0x%02X;", lb);
        LINE("%s", t);
    } else if (in_list(m, BRANCH)) {
        int k = 0, extra = ((nxt ^ v) & 0x100) ? 2 : 1;
        while (!is(BRANCH[k], m))
            k++;
        leave(out, tmp, sizeof tmp, v);
        LINE("if (%s) { n_cyc(c, %d); %s }", COND[k], extra, tmp);
    } else if (is(m, "jmp") && d->mode == ABS) {
        leave(out, tmp, sizeof tmp, v);
        LINE("%s", tmp);
    } else if (is(m, "jmp")) {
        int hi = ((v + 1) & 0x00FF) | (v & 0xFF00);
        const char *rdf = v < 0x2000 ? "n_rdram" : "n_rd";
        LINE("c->bus = 0x%02X;", lb);
        LINE("ea = %s(c, 0x%04X); ea |= (uint16_t)(%s(c, 0x%04X) << 8);", rdf, v, rdf, hi);
        LINE("c->pc = ea; N_EXIT;");
    } else if (is(m, "jsr")) {
        int ret = (a + 2) & 0xFFFF;
        LINE("c->bus = 0x%02X;", byte_at(a + 1));
        LINE("n_push(c, 0x%02X); n_push(c, 0x%02X);", ret >> 8, ret & 0xFF);
        LINE("c->bus = 0x%02X;", v >> 8);
        LINE("c->pc = 0x%04X; N_EXIT;", v);
    } else if (is(m, "rts")) {
        LINE("c->bus = 0x%02X;", lb);
        LINE("ea = n_pop(c); ea |= (uint16_t)(n_pop(c) << 8); c->pc = (uint16_t)(ea + 1); N_EXIT;");
    } else if (is(m, "rti")) {
        LINE("c->bus = 0x%02X;", lb);
        LINE("c->p = n_pop(c); c->p_before = c->p;");
        LINE("ea = n_pop(c); ea |= (uint16_t)(n_pop(c) << 8); c->pc = ea; N_EXIT;");
    } else
        return 0;
    return 1;
}

/* ---- meta ---- */
typedef struct { int bank, start, end; char name[64], conf[16]; } Routine;
typedef struct { int bank, addr; char kind[32]; } Timing;

static Routine *routines;
static int n_routines;
static Timing *timings;
static int n_timings;
static char meta_sha[41];

static int load_meta(const char *path)
{
    FILE *f = fopen(path, "r");
    char line[1024];
    int cap_r = 0, cap_t = 0;
    if (!f) {
        fprintf(stderr, "cannot open %s\n", path);
        exit(2);
    }
    while (fgets(line, sizeof line, f)) {
        char *tok[32], *p = line;
        int nt = 0;
        while (nt < 32) {
            while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')
                p++;
            if (!*p)
                break;
            tok[nt++] = p;
            while (*p && *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n')
                p++;
            if (*p)
                *p++ = 0;
        }
        if (!nt || tok[0][0] == '#')
            continue;
        if (is(tok[0], "rom")) {
            int i;
            for (i = 1; i < nt; i++)
                if (!strncmp(tok[i], "sha1=", 5))
                    snprintf(meta_sha, sizeof meta_sha, "%s", tok[i] + 5);
        } else if (is(tok[0], "code") && nt >= 5) {
            Routine *r;
            int i;
            if (n_routines == cap_r) {
                cap_r = cap_r * 2 + 256;
                routines = realloc(routines, (size_t)cap_r * sizeof *routines);
            }
            r = &routines[n_routines++];
            memset(r, 0, sizeof *r);
            r->bank = atoi(tok[1]);
            r->start = (int)strtol(tok[2] + 1, NULL, 16);
            r->end = (int)strtol(tok[3] + 1, NULL, 16);
            snprintf(r->name, sizeof r->name, "%s", tok[4]);
            for (i = 5; i < nt; i++)
                if (!strncmp(tok[i], "conf=", 5))
                    snprintf(r->conf, sizeof r->conf, "%s", tok[i] + 5);
        } else if (is(tok[0], "timing") && nt >= 4) {
            Timing *t;
            if (n_timings == cap_t) {
                cap_t = cap_t * 2 + 64;
                timings = realloc(timings, (size_t)cap_t * sizeof *timings);
            }
            t = &timings[n_timings++];
            t->bank = atoi(tok[1]);
            t->addr = (int)strtol(tok[2] + 1, NULL, 16);
            snprintf(t->kind, sizeof t->kind, "%s", tok[3]);
        }
    }
    fclose(f);
    return 0;
}

static int list_has(const char *list, const char *item)
{
    size_t n = strlen(item);
    const char *p = list;
    while (*p) {
        const char *e = strchr(p, ',');
        size_t len = e ? (size_t)(e - p) : strlen(p);
        if (len == n && !strncmp(p, item, n))
            return 1;
        if (!e)
            break;
        p = e + 1;
    }
    return 0;
}

static int cmp_str(const void *a, const void *b)
{
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

/* why a routine is left to the interpreter, or NULL */
static const char *skip_reason(const Routine *r, const char *confs, const char *excl, int excl_timing, char *buf,
                               size_t cap)
{
    int i;
    if (!list_has(confs, r->conf)) {
        snprintf(buf, cap, "conf=%s", r->conf);
        return buf;
    }
    if (excl && *excl) {
        const char *p = excl;
        while (*p) {
            int b;
            unsigned ad;
            if (sscanf(p, "%d:%x", &b, &ad) == 2 && b == r->bank && (int)ad == r->start) {
                snprintf(buf, cap, "excluded");
                return buf;
            }
            p = strchr(p, ',');
            if (!p)
                break;
            p++;
        }
    }
    if (excl_timing) {
        int hit = 0;
        for (i = 0; i < n_timings; i++)
            if (timings[i].bank == r->bank && timings[i].addr >= r->start && timings[i].addr <= r->end &&
                !is(timings[i].kind, "bank_switch"))
                hit = 1;
        if (hit) {
            const char *kinds[64];
            int nk = 0, k, j;
            for (i = 0; i < n_timings && nk < 64; i++) {
                if (timings[i].bank != r->bank || timings[i].addr < r->start || timings[i].addr > r->end)
                    continue;
                for (j = 0; j < nk; j++)
                    if (is(kinds[j], timings[i].kind))
                        break;
                if (j == nk)
                    kinds[nk++] = timings[i].kind;
            }
            qsort((void *)kinds, (size_t)nk, sizeof kinds[0], cmp_str);
            snprintf(buf, cap, "timing:");
            for (k = 0; k < nk; k++)
                snprintf(buf + strlen(buf), cap - strlen(buf), "%s%s", k ? "," : "", kinds[k]);
            return buf;
        }
    }
    return NULL;
}

/* Writes native_b0..7.c, native_all.c and skipped.txt for `rom` (whose file SHA-1 is sha1_hex, or NULL when the
 * caller already accepted it with rom_load, which only takes the one supported dump) into outdir.
 * Returns 0 on success; on failure writes a message to err. */
int btdd_gen_write(const Rom *rom_in, const char *sha1_hex, const char *meta_path, const char *outdir,
                   const char *confs, const char *excl, int excl_timing, char *err, size_t errcap)
{
    static uint8_t map[0x10000];
    char path[1024];
    int b, total_routines = 0, total_entries = 0, skipped_n = 0, unsupported_brk = 0;
    Buf skipped = {0};
    FILE *f;
    const Rom *rp = rom_in;
#define rom (*rp)

    n_routines = n_timings = 0;
    meta_sha[0] = 0;
    if (load_meta(meta_path) != 0) {
        snprintf(err, errcap, "cannot open meta %s", meta_path);
        return 1;
    }
    if (sha1_hex && strcmp(sha1_hex, meta_sha) != 0) { /* NULL: the caller verified the ROM with rom_load */
        snprintf(err, errcap, "ROM SHA-1 %s does not match the meta (%s)", sha1_hex, meta_sha);
        return 1;
    }
    init_ops();
    in_routine = map;
    memset(map, 0, sizeof map);

    for (b = 0; b < 8; b++) {
        Buf file = {0}, regs = {0};
        int r;
        mem = rom.prg[b];
        cur_bank = b;
        bprintf(&file, "/* GENERATED from the player's ROM by the btdd recompiler. ROM-derived: never commit or "
                       "distribute. */\n#include \"native_rt.h\"\n\n");
        for (r = 0; r < n_routines; r++) {
            const Routine *R = &routines[r];
            char why[256];
            Insn code[4096];
            int addrs[4096], nc = 0, a, k, ne = 0;
            Buf body = {0};
            if (R->bank != b)
                continue;
            if (skip_reason(R, confs, excl, excl_timing, why, sizeof why)) {
                bprintf(&skipped, "%d $%04X $%04X %s %s\n", b, R->start, R->end, R->name, why);
                skipped_n++;
                continue;
            }
            for (a = R->start; a <= R->end && nc < 4096;) {
                Insn d;
                if (!decode(a, &d) || a + d.n - 1 > R->end)
                    break;
                code[nc] = d;
                addrs[nc++] = a;
                a += d.n;
            }
            for (k = 0; k < nc; k++)
                map[addrs[k]] = 1;
            for (k = 0; k < nc; k++) {
                int at = addrs[k], nxt = (at + code[k].n) & 0xFFFF;
                size_t mark = body.len;
                int op = byte_at(at);
                bprintf(&body, "L%04X: if (c->budget <= 0 || c->irq) { c->pc = 0x%04X; return; }\n", at, at);
                bprintf(&body, "E%04X: c->p_before = c->p; n_cyc(c, %d); c->instructions++; c->last_op = 0x%02X; "
                               "native_insns++;\n", at, cpu_base_cycles[op], op);
                if (!insn(&body, at, &code[k], nxt)) {
                    body.len = mark;
                    bprintf(&body, "L%04X: c->pc = 0x%04X; return; /* %s: interpreter */\n", at, at, code[k].m);
                    if (is(code[k].m, "brk"))
                        unsupported_brk++;
                    continue;
                }
                addrs[ne++] = at; /* entries reuse the front of addrs (ne <= k) */
            }
            if (nc) {
                /* recompute the fall-out address from the last decoded instruction */
                int la = R->start, ln = 0;
                Insn d;
                for (a = R->start; a <= R->end;) {
                    if (!decode(a, &d) || a + d.n - 1 > R->end)
                        break;
                    la = a;
                    ln = d.n;
                    a += d.n;
                }
                bprintf(&body, "    c->pc = 0x%04X; N_EXIT; /* falls out of the routine */\n", (la + ln) & 0xFFFF);
            }
            for (a = R->start; a <= R->end; a++)
                map[a] = 0;
            bprintf(&file, "/* %s $%04X-$%04X */\nstatic void r%d_%04x(Cpu *c)\n{\n", R->name, R->start, R->end, b,
                    R->start);
            bprintf(&file, "    uint8_t v; uint16_t ea, base; uint8_t zp;\n    (void)v; (void)ea; (void)base; (void)zp;\n"
                           "    switch (c->pc) {\n");
            for (k = 0; k < ne; k++)
                bprintf(&file, "    case 0x%04X: goto E%04X;\n", addrs[k], addrs[k]);
            bprintf(&file, "    default: return;\n    }\n");
            if (body.len)
                bprintf(&file, "%.*s", (int)body.len, body.s);
            bprintf(&file, "}\nstatic const uint16_t r%d_%04x_at[] = {", b, R->start);
            for (k = 0; k < ne; k++)
                bprintf(&file, "%s0x%04X", k ? "," : "", addrs[k]);
            bprintf(&file, "};\n\n");
            if (ne)
                bprintf(&regs, "    native_register(%d, r%d_%04x, r%d_%04x_at, %d);\n", b, b, R->start, b, R->start, ne);
            total_routines++;
            total_entries += ne;
            free(body.s);
        }
        bprintf(&file, "void native_register_b%d(const NativeApi *api)\n{\n    n_api = api;\n", b);
        if (regs.len)
            bprintf(&file, "%.*s", (int)regs.len, regs.s);
        bprintf(&file, "}\n");
        snprintf(path, sizeof path, "%s/native_b%d.c", outdir, b);
        f = fopen(path, "wb");
        if (!f || fwrite(file.s, 1, file.len, f) != file.len) {
            fprintf(stderr, "cannot write %s\n", path);
            return 1;
        }
        fclose(f);
        free(file.s);
        free(regs.s);
    }
    snprintf(path, sizeof path, "%s/native_all.c", outdir);
    f = fopen(path, "wb");
    if (!f) {
        snprintf(err, errcap, "cannot write %s", path);
        return 1;
    }
    fprintf(f, "#include \"native_rt.h\"\n");
    for (b = 0; b < 8; b++)
        fprintf(f, "void native_register_b%d(const NativeApi *api);\n", b);
    fprintf(f, "NATIVE_EXPORT int native_register_all(const NativeApi *api)\n{\n");
    fprintf(f, "    if (api->version != NATIVE_API_VERSION)\n        return 0;\n");
    for (b = 0; b < 8; b++)
        fprintf(f, "    native_register_b%d(api);\n", b);
    fprintf(f, "    return 1;\n}\n");
    fprintf(f, "#define NATIVE_LAYOUT_ITEM(e) (size_t)(e),\n");
    fprintf(f, "NATIVE_EXPORT const size_t *native_layout(void)\n{\n");
    fprintf(f, "    static const size_t v[] = {NATIVE_LAYOUT_LIST(NATIVE_LAYOUT_ITEM) 0};\n    return v;\n}\n");
    fclose(f);
    snprintf(path, sizeof path, "%s/skipped.txt", outdir);
    f = fopen(path, "wb");
    if (!f) {
        snprintf(err, errcap, "cannot write %s", path);
        return 1;
    }
    if (skipped.len)
        fwrite(skipped.s, 1, skipped.len, f);
    fclose(f);
    snprintf(err, errcap, "%d routines, %d native entry points, %d left to the interpreter; unsupported: %s",
             total_routines, total_entries, skipped_n, unsupported_brk ? "brk" : "none");
    free(skipped.s);
    return 0;
#undef rom
}

#ifndef BTDD_GEN_NO_MAIN
int main(int argc, char **argv)
{
    static Rom rom;
    char hash[41], msg[512];
    const char *confs = "verified,static", *excl = "";
    int excl_timing = 0, i;
    RomResult rr;

    if (argc < 4) {
        fprintf(stderr, "usage: btdd_gen ROM META OUTDIR [--conf a,b] [--exclude-timing] [--exclude b:addr,...]\n");
        return 2;
    }
    for (i = 4; i < argc; i++) {
        if (is(argv[i], "--conf") && i + 1 < argc)
            confs = argv[++i];
        else if (is(argv[i], "--exclude") && i + 1 < argc)
            excl = argv[++i];
        else if (is(argv[i], "--exclude-timing"))
            excl_timing = 1;
        else {
            fprintf(stderr, "unknown option %s\n", argv[i]);
            return 2;
        }
    }
    rr = rom_load(argv[1], &rom, hash);
    if (rr != ROM_OK) {
        fprintf(stderr, "ROM rejected: %s\n", rom_result_text(rr));
        return 1;
    }
    if (btdd_gen_write(&rom, hash, argv[2], argv[3], confs, excl, excl_timing, msg, sizeof msg) != 0) {
        fprintf(stderr, "btdd_gen: %s\n", msg);
        return 1;
    }
    printf("btdd_gen: %s\n", msg);
    return 0;
}
#endif
