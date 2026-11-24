/* Coverage recorder (see coverage.h). */
#include "coverage.h"
#include <stdlib.h>
#include <string.h>

/* ---- opcode facts (official opcodes; unofficial ones jam the CPU before they could matter) ---- */

/* Addressing mode in FCEUX's debugger numbering: 0 none/implied/immediate/relative (and JSR),
 * 1 (zp,X), 2 zp, 3 abs (incl. JMP abs/ind), 4 (zp),Y, 5 zp,X, 6 abs,Y, 7 abs,X, 8 zp,Y. */
static int optype(uint8_t op)
{
    int b = (op >> 2) & 7, c = op & 3;
    if (c == 1) {
        static const int m[8] = {1, 2, 0, 3, 4, 5, 6, 7};
        return m[b];
    }
    if (c == 2) {
        switch (b) {
        case 1: return 2;
        case 3: return 3;
        case 5: return (op == 0x96 || op == 0xB6) ? 8 : 5;
        case 7: return (op == 0xBE || op == 0x9E) ? 6 : 7;
        default: return 0;
        }
    }
    if (c == 0) {
        if (op == 0x20)
            return 0;
        switch (b) {
        case 1: return 2;
        case 3: return 3;
        case 5: return 5;
        case 7: return 7;
        default: return 0;
        }
    }
    return 0;
}

static int opsize(uint8_t op)
{
    int b = (op >> 2) & 7, c = op & 3;
    if (op == 0x20)
        return 3;
    switch (optype(op)) {
    case 3: case 6: case 7: return 3;
    case 1: case 2: case 4: case 5: case 8: return 2;
    default: break;
    }
    /* mode 0: immediate and relative take one operand byte */
    if (c == 1 && b == 2)
        return 2;                      /* #imm, ALU group */
    if ((c == 0 || c == 2) && b == 0 && op >= 0x80)
        return 2;                      /* LDY/CPY/CPX/LDX #imm */
    if (c == 0 && b == 4)
        return 2;                      /* branches */
    return 1;
}

/* FCEUX's "this instruction writes memory" (stores and memory read-modify-writes). */
static int opwrites(uint8_t op)
{
    int a = op >> 5, b = (op >> 2) & 7, c = op & 3;
    if (optype(op) == 0)
        return 0;
    if (c == 1)
        return a == 4;                         /* STA */
    if (c == 0)
        return op == 0x84 || op == 0x8C || op == 0x94; /* STY */
    if (c == 2) {
        if (a == 4)
            return 1;                          /* STX */
        return a != 5 && (b == 1 || b == 3 || b == 5 || b == 7); /* ASL ROL LSR ROR DEC INC */
    }
    return 0;
}

static int is_jump(uint8_t op)
{
    return op == 0x4C || op == 0x6C || (op & 0x1F) == 0x10; /* JMP, branches */
}

/* ---- tables ---- */

static uint32_t crc32_buf(const uint8_t *p, size_t n)
{
    uint32_t c = 0xFFFFFFFFu;
    size_t i;
    int k;
    for (i = 0; i < n; i++) {
        c ^= p[i];
        for (k = 0; k < 8; k++)
            c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1)));
    }
    return ~c;
}

void cov_init(Coverage *cov, const Rom *rom)
{
    memset(cov, 0, sizeof *cov);
    cov->prg_crc = crc32_buf(&rom->prg[0][0], COV_PRG_SIZE);
}

void cov_free(Coverage *cov)
{
    free(cov->reads);
    cov->reads = NULL;
    cov->reads_cap = cov->reads_used = 0;
}

void cov_reset_tracking(Coverage *cov)
{
    cov->have_last = cov->indirect_next = cov->pending_entry = 0;
    cov->sp = 0;
}

static uint32_t hash_key(uint64_t k, uint16_t area)
{
    uint64_t h = (k ^ ((uint64_t)area << 53)) * 0x9E3779B97F4A7C15ull;
    return (uint32_t)(h >> 32);
}

static void reads_grow(Coverage *cov)
{
    uint32_t ncap = cov->reads_cap ? cov->reads_cap * 2 : (1u << 16), i;
    CovRead *n = calloc(ncap, sizeof *n);
    if (!n) {
        fprintf(stderr, "coverage: out of memory for the reads table\n");
        exit(1);
    }
    for (i = 0; i < cov->reads_cap; i++) {
        CovRead *r = &cov->reads[i];
        if (r->count) {
            uint32_t h = hash_key(r->key, r->area) & (ncap - 1);
            while (n[h].count)
                h = (h + 1) & (ncap - 1);
            n[h] = *r;
        }
    }
    free(cov->reads);
    cov->reads = n;
    cov->reads_cap = ncap;
}

static void reads_add(Coverage *cov, uint64_t key, uint16_t area)
{
    uint32_t h;
    if ((cov->reads_used + 1) * 2 > cov->reads_cap)
        reads_grow(cov);
    h = hash_key(key, area) & (cov->reads_cap - 1);
    while (cov->reads[h].count) {
        if (cov->reads[h].key == key && cov->reads[h].area == area) {
            cov->reads[h].count++;
            return;
        }
        h = (h + 1) & (cov->reads_cap - 1);
    }
    cov->reads[h].key = key;
    cov->reads[h].area = area;
    cov->reads[h].count = 1;
    cov->reads_used++;
}

/* ---- hooks ---- */

static inline uint32_t prg_off(int bank, uint16_t addr)
{
    return (uint32_t)bank * 0x8000u + (addr & 0x7FFFu);
}

static void mark_data(Coverage *cov, Nes *n, uint16_t a, uint16_t pc)
{
    uint32_t j = prg_off(n->prg_bank, a);
    if (!(cov->mesen[j] & MESEN_DATA)) {
        cov->mesen[j] |= MESEN_DATA;
        cov->new_data++;
    }
    if (cov->log_reads) {
        uint64_t key = (uint64_t)j | (uint64_t)pc << 18;
        if (cov->sp > 0) {
            const CovFrame *f = &cov->stack[cov->sp - 1];
            key |= (uint64_t)f->entry << 34 | (uint64_t)(f->bank & 7) << 50;
        }
        reads_add(cov, key, (uint16_t)(n->ram[0x0D] << 8 | n->ram[0x15]));
    }
}

static void insn_hook(void *ctx, Cpu *c)
{
    Nes *n = ctx;
    Coverage *cov = n->user;
    uint16_t pc = c->pc, a = 0;
    uint8_t op = nes_peek(n, pc), o1, o2;
    int bank = n->prg_bank, size = opsize(op), type = optype(op), i;

    if (pc < 0x8000) {
        cov->ram_exec++;
        if (pc < 0x2000) {
            cov->ram_pc_seen[pc & 0x7FF] = 1;
            if (cov->ram_pc_allowed && !cov->ram_pc_allowed[pc & 0x7FF])
                cov->bad_exec++;
        } else
            cov->bad_exec++; /* open bus / PPU / APU space: never legitimate */
    }

    /* shadow call stack: drop routines whose frame S has risen above */
    while (cov->sp > 0 && c->s > cov->stack[cov->sp - 1].s)
        cov->sp--;
    if (cov->pending_entry || (cov->have_last && cov->last_op == 0x20)) {
        if (cov->sp < 256) {
            cov->stack[cov->sp].entry = pc;
            cov->stack[cov->sp].bank = (uint8_t)bank;
            cov->stack[cov->sp].s = c->s;
            cov->sp++;
        }
        if (pc >= 0x8000)
            cov->mesen[prg_off(bank, pc)] |= MESEN_ENTRY;
    } else if (cov->have_last && is_jump(cov->last_op) &&
               pc != (uint16_t)(cov->last_pc + cov->last_size) && pc >= 0x8000) {
        cov->mesen[prg_off(bank, pc)] |= MESEN_JUMP;
    }

    /* code bytes */
    for (i = 0; i < size; i++) {
        uint16_t p = (uint16_t)(pc + i);
        uint32_t j;
        if (p < 0x8000)
            continue;
        j = prg_off(bank, p);
        if (!(cov->mesen[j] & MESEN_CODE)) {
            cov->mesen[j] |= MESEN_CODE;
            cov->new_code++;
        }
        if (!(cov->fceux[j] & 1)) {
            cov->fceux[j] |= (uint8_t)(1 | ((p >> 11) & 0x0C) | (cov->indirect_next ? 0x10 : 0));
        }
    }
    cov->indirect_next = op == 0x6C;

    /* effective address, computed before the instruction runs (FCEUX's debugger rule) */
    o1 = nes_peek(n, (uint16_t)(pc + 1));
    o2 = nes_peek(n, (uint16_t)(pc + 2));
    switch (type) {
    case 1:
        a = (uint16_t)(n->ram[(o1 + c->x) & 0xFF] | n->ram[(o1 + c->x + 1) & 0xFF] << 8);
        break;
    case 2: a = o1; break;
    case 3: a = (uint16_t)(o1 | o2 << 8); break;
    case 4: a = (uint16_t)((n->ram[o1] | n->ram[(o1 + 1) & 0xFF] << 8) + c->y); break;
    case 5: a = (uint16_t)(o1 + c->x); break;
    case 6: a = (uint16_t)((o1 | o2 << 8) + c->y); break;
    case 7: a = (uint16_t)((o1 | o2 << 8) + c->x); break;
    case 8: a = (uint16_t)(o1 + c->y); break;
    default: break;
    }
    if (type && a >= 0x8000) {
        uint32_t j = prg_off(bank, a);
        if (opwrites(op)) {
            if (cov->fceux_clear_on_write)
                cov->fceux[j] = 0;
        } else {
            if (!(cov->fceux[j] & 2))
                cov->fceux[j] |= (uint8_t)(2 | ((a >> 11) & 0x0C) | ((type == 1 || type == 4) ? 0x20 : 0));
            if (op != 0x4C) /* a JMP abs target is not a data read (FCEUX logs it as one anyway) */
                mark_data(cov, n, a, pc);
            if (op == 0x6C) {
                uint16_t hi = (uint16_t)(((a + 1) & 0x00FF) | (a & 0xFF00));
                if (hi >= 0x8000)
                    mark_data(cov, n, hi, pc);
            }
        }
    }

    cov->last_pc = pc;
    cov->last_op = op;
    cov->last_size = (uint8_t)size;
    cov->have_last = 1;
    cov->pending_entry = 0;
}

static void vector_hook(void *ctx, Cpu *c, uint16_t vector)
{
    Nes *n = ctx;
    Coverage *cov = n->user;
    int k;
    (void)c;
    for (k = 0; k < 2; k++) {
        uint32_t j = prg_off(n->prg_bank, (uint16_t)(vector + k));
        if (!(cov->fceux[j] & 2))
            cov->fceux[j] |= 0x0E;
        if (!(cov->mesen[j] & MESEN_DATA)) {
            cov->mesen[j] |= MESEN_DATA;
            cov->new_data++;
        }
    }
    cov->pending_entry = 1;
}

void cov_attach(Coverage *cov, Nes *nes)
{
    nes->user = cov;
    nes->cpu.insn_hook = insn_hook;
    nes->cpu.vector_hook = vector_hook;
    cov_reset_tracking(cov);
}

void cov_detach(Nes *nes)
{
    nes->cpu.insn_hook = NULL;
    nes->cpu.vector_hook = NULL;
}

/* ---- files ---- */

int cov_merge_file(Coverage *cov, const char *path)
{
    FILE *f = fopen(path, "rb");
    uint8_t *buf;
    long len;
    size_t i;
    if (!f)
        return -1;
    fseek(f, 0, SEEK_END);
    len = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (len < COV_PRG_SIZE) {
        fclose(f);
        return -1;
    }
    buf = malloc((size_t)len);
    if (!buf || fread(buf, 1, (size_t)len, f) != (size_t)len) {
        free(buf);
        fclose(f);
        return -1;
    }
    fclose(f);
    if (len == COV_PRG_SIZE && memcmp(buf, "CDLv", 4) != 0) {
        for (i = 0; i < COV_PRG_SIZE; i++)
            cov->mesen[i] |= (uint8_t)((buf[i] & 0x11 ? MESEN_CODE : 0) | (buf[i] & 0x22 ? MESEN_DATA : 0));
    } else {
        const uint8_t *body = buf + (len - COV_PRG_SIZE);
        for (i = 0; i < COV_PRG_SIZE; i++)
            cov->mesen[i] |= body[i] & 0x0F;
    }
    free(buf);
    return 0;
}

static int write_file(const char *path, const void *hdr, size_t hlen, const uint8_t *body)
{
    FILE *f = fopen(path, "wb");
    int ok;
    if (!f)
        return -1;
    ok = (!hlen || fwrite(hdr, 1, hlen, f) == hlen) && fwrite(body, 1, COV_PRG_SIZE, f) == COV_PRG_SIZE;
    return (fclose(f) == 0 && ok) ? 0 : -1;
}

int cov_write_fceux(const Coverage *cov, const char *path)
{
    return write_file(path, NULL, 0, cov->fceux);
}

int cov_write_mesen(const Coverage *cov, const char *path)
{
    uint8_t hdr[9] = {'C', 'D', 'L', 'v', '2'};
    hdr[5] = (uint8_t)cov->prg_crc;
    hdr[6] = (uint8_t)(cov->prg_crc >> 8);
    hdr[7] = (uint8_t)(cov->prg_crc >> 16);
    hdr[8] = (uint8_t)(cov->prg_crc >> 24);
    return write_file(path, hdr, sizeof hdr, cov->mesen);
}

static int read_cmp(const void *pa, const void *pb)
{
    const CovRead *a = pa, *b = pb;
    uint64_t ka = (a->key & 0x3FFFFu) << 40 | (a->key >> 18 & 0xFFFF) << 24 | a->area;
    uint64_t kb = (b->key & 0x3FFFFu) << 40 | (b->key >> 18 & 0xFFFF) << 24 | b->area;
    if (ka != kb)
        return ka < kb ? -1 : 1;
    return a->key < b->key ? -1 : a->key > b->key;
}

int cov_write_reads(const Coverage *cov, const char *path)
{
    FILE *f;
    CovRead *v;
    uint32_t i, n = 0;
    int ok;
    v = malloc((cov->reads_used + 1) * sizeof *v);
    if (!v)
        return -1;
    for (i = 0; i < cov->reads_cap; i++)
        if (cov->reads[i].count)
            v[n++] = cov->reads[i];
    qsort(v, n, sizeof *v, read_cmp);
    f = fopen(path, "w");
    if (!f) {
        free(v);
        return -1;
    }
    fprintf(f, "bank\taddr\treader_pc\troutine\tlevel\tsection\tcount\n");
    for (i = 0; i < n; i++) {
        uint32_t off = (uint32_t)(v[i].key & 0x3FFFF);
        unsigned pc = (unsigned)(v[i].key >> 18 & 0xFFFF), rt = (unsigned)(v[i].key >> 34 & 0xFFFF);
        unsigned rb = (unsigned)(v[i].key >> 50 & 7);
        char rts[16];
        if (rt)
            snprintf(rts, sizeof rts, "%u:$%04X", rb, rt);
        else
            snprintf(rts, sizeof rts, "-");
        fprintf(f, "%u\t$%04X\t$%04X\t%s\t%02X\t%02X\t%u\n", off >> 15, 0x8000 + (off & 0x7FFF), pc, rts,
                v[i].area >> 8, v[i].area & 0xFF, v[i].count);
    }
    ok = !ferror(f);
    free(v);
    return (fclose(f) == 0 && ok) ? 0 : -1;
}

void cov_count(const uint8_t *fc, int bank, long *code, long *data, long *both, long *seen)
{
    long i;
    *code = *data = *both = *seen = 0;
    for (i = 0; i < 0x8000; i++) {
        uint8_t b = fc[bank * 0x8000 + i];
        int c = (b & 0x11) != 0, d = (b & 0x22) != 0;
        *code += c && !d;
        *data += d && !c;
        *both += c && d;
        *seen += c || d;
    }
}

int cov_write_ram_pcs(const Coverage *cov, const char *path)
{
    FILE *f = fopen(path, "w");
    int a;
    if (!f)
        return -1;
    for (a = 0; a < 0x800; a++)
        if (cov->ram_pc_seen[a])
            fprintf(f, "%04X\n", a);
    return fclose(f) == 0 ? 0 : -1;
}

int cov_load_ram_pcs(uint8_t map[0x800], const char *path)
{
    FILE *f = fopen(path, "r");
    unsigned a;
    if (!f)
        return -1;
    memset(map, 0, 0x800);
    while (fscanf(f, "%x", &a) == 1)
        map[a & 0x7FF] = 1;
    fclose(f);
    return 0;
}

int cov_opsize(uint8_t op)
{
    return opsize(op);
}
