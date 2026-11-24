/* btdd_static: static completion probe (docs/COVERAGE_PLAN.md way 6).
 *
 *   btdd_static ROM --known KNOWN.txt --snaps SNAPS.txt --out PREFIX [--ram-pcs F] [--limit 200000]
 *
 * 1. Merges the code/data logs listed in KNOWN.txt. Instruction starts = the first byte of every run of code
 *    bytes plus bytes flagged as jump target / entry; each run is decoded linearly.
 * 2. Candidates = targets of executed branches (taken target and fall-through), JSR and JMP abs that were
 *    never executed themselves. From each candidate, recursive descent decodes until RTS/RTI/JMP ind or
 *    already-known code, following branches, JSR and JMP abs. A candidate is rejected when it reaches an
 *    unofficial opcode or BRK, runs into bytes only ever seen as data, or leaves the bank.
 * 3. Each remaining candidate is probed: from up to 8 snapshots (different stages), the candidate's bank is
 *    mapped and the code is called with cpu_call; "clean" = it returns (RTS at entry level, or unwinds the
 *    stack) within --limit instructions without jamming or starting an instruction at an unknown RAM address.
 *
 * Output: PREFIX.tsv (every candidate: bank, address, source, decoded bytes, verdict per snapshot) and
 * PREFIX-m.cdl (Mesen format: the statically decoded bytes of clean candidates as code, entry bit on JSR
 * targets, jump-target bit on branch/JMP targets). Publishing that log makes btdd-decomp queue Carve tasks.
 */
#include "cov/coverage.h"
#include "cov/snapshot.h"
#include "nes/nes.h"
#include "rom/rom.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SZ COV_PRG_SIZE

enum { SRC_BRANCH = 1, SRC_FALL = 2, SRC_JSR = 4, SRC_JMP = 8 };

static Rom rom;
static Coverage known;
static uint8_t cand_src[SZ];    /* candidate roots and how they were referenced */
static int owner[SZ];           /* candidate id that decoded the byte first (0 = none) */

static uint8_t byte_at(int bank, uint16_t addr)
{
    return rom.prg[bank][addr & 0x7FFF];
}

static int is_code(long i) { return known.mesen[i] & MESEN_CODE; }
static int is_data_only(long i) { return (known.mesen[i] & (MESEN_CODE | MESEN_DATA)) == MESEN_DATA; }

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
            cap = cap ? cap * 2 : 64;
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

static void add_target(int bank, uint16_t t, int src)
{
    long j;
    if (t < 0x8000)
        return;
    j = (long)bank * 0x8000 + (t & 0x7FFF);
    if (!is_code(j))
        cand_src[j] |= (uint8_t)src;
}

/* Step 1: decode the known code linearly and collect never-executed targets. */
static void scan_known(void)
{
    int bank;
    for (bank = 0; bank < 8; bank++) {
        long base = (long)bank * 0x8000, i = 0;
        while (i < 0x8000) {
            if (!is_code(base + i)) {
                i++;
                continue;
            }
            /* a run of code: decode it from its first byte */
            while (i < 0x8000 && is_code(base + i)) {
                uint16_t pc = (uint16_t)(0x8000 + i);
                uint8_t op = byte_at(bank, pc);
                int n = cov_opsize(op), k, ok = cpu_official[op];
                for (k = 1; k < n && ok; k++)
                    ok = i + k < 0x8000 && is_code(base + i + k);
                if (!ok) { /* misaligned or data inside: resync at the next byte */
                    i++;
                    continue;
                }
                if ((op & 0x1F) == 0x10) {
                    int8_t rel = (int8_t)byte_at(bank, (uint16_t)(pc + 1));
                    add_target(bank, (uint16_t)(pc + 2 + rel), SRC_BRANCH);
                    add_target(bank, (uint16_t)(pc + 2), SRC_FALL);
                } else if (op == 0x20 || op == 0x4C) {
                    uint16_t t = (uint16_t)(byte_at(bank, (uint16_t)(pc + 1)) | byte_at(bank, (uint16_t)(pc + 2)) << 8);
                    add_target(bank, t, op == 0x20 ? SRC_JSR : SRC_JMP);
                }
                i += n;
            }
        }
    }
}

/* Step 2: recursive descent from one candidate. Returns decoded byte count, or -1 if rejected. */
static int descend(int id, int bank, uint16_t start, uint8_t *mark, long *stack, int stack_cap)
{
    int sp = 0, count = 0;
    stack[sp++] = start;
    while (sp > 0) {
        uint16_t pc = (uint16_t)stack[--sp];
        for (;;) {
            long j;
            uint8_t op;
            int n, k;
            if (pc < 0x8000)
                return -1;
            j = (long)bank * 0x8000 + (pc & 0x7FFF);
            if (is_code(j) || mark[j])
                break; /* joined known or already decoded code */
            op = byte_at(bank, pc);
            if (!cpu_official[op] || op == 0x00)
                return -1;
            n = cov_opsize(op);
            if ((long)(pc & 0x7FFF) + n > 0x8000)
                return -1;
            for (k = 0; k < n; k++) {
                if (is_data_only(j + k) || is_code(j + k) != is_code(j))
                    return -1;
                mark[j + k] = 1;
                if (!owner[j + k])
                    owner[j + k] = id;
            }
            count += n;
            if ((op & 0x1F) == 0x10) {
                int8_t rel = (int8_t)byte_at(bank, (uint16_t)(pc + 1));
                if (sp < stack_cap)
                    stack[sp++] = (uint16_t)(pc + 2 + rel);
            } else if (op == 0x20 || op == 0x4C) {
                uint16_t t = (uint16_t)(byte_at(bank, (uint16_t)(pc + 1)) | byte_at(bank, (uint16_t)(pc + 2)) << 8);
                if (t < 0x8000)
                    return -1;
                if (sp < stack_cap)
                    stack[sp++] = t;
                if (op == 0x4C)
                    break;
            }
            if (op == 0x60 || op == 0x40 || op == 0x6C)
                break;
            pc = (uint16_t)(pc + n);
            if (count > 4096)
                return -1;
        }
    }
    return count;
}

int main(int argc, char **argv)
{
    static Nes nes, base[8];
    static Coverage pc_cov;
    static uint8_t allowed[0x800], mark[SZ], out[SZ];
    const char *rom_path = NULL, *known_list = NULL, *snaps_list = NULL, *prefix = NULL, *ram_pcs = NULL;
    long limit = 200000, i;
    char **kl, **sl, hash[41], p[1024];
    int nk, ns, nb = 0, k, id = 0, n_cand = 0, n_rej = 0, n_clean = 0;
    long clean_bytes = 0;
    long *stack = malloc(sizeof(long) * 4096);
    FILE *tsv;

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--known") && i + 1 < argc) known_list = argv[++i];
        else if (!strcmp(argv[i], "--snaps") && i + 1 < argc) snaps_list = argv[++i];
        else if (!strcmp(argv[i], "--out") && i + 1 < argc) prefix = argv[++i];
        else if (!strcmp(argv[i], "--ram-pcs") && i + 1 < argc) ram_pcs = argv[++i];
        else if (!strcmp(argv[i], "--limit") && i + 1 < argc) limit = strtol(argv[++i], NULL, 10);
        else if (argv[i][0] != '-' && !rom_path) rom_path = argv[i];
        else {
            fprintf(stderr, "btdd_static: bad argument %s\n", argv[i]);
            return 2;
        }
    }
    if (!rom_path || !known_list || !snaps_list || !prefix) {
        fprintf(stderr, "usage: btdd_static ROM --known KNOWN.txt --snaps SNAPS.txt --out PREFIX\n");
        return 2;
    }
    if (rom_load(rom_path, &rom, hash) != ROM_OK)
        return 1;
    cov_init(&known, &rom);
    nk = load_list(known_list, &kl);
    for (k = 0; k < nk; k++)
        cov_merge_file(&known, kl[k]);
    ns = load_list(snaps_list, &sl);
    for (k = 0; k < ns && nb < 8; k++) {
        nes_init(&base[nb], &rom);
        if (snap_load(&base[nb], NULL, sl[k]) == 0)
            nb++;
    }
    if (ram_pcs && cov_load_ram_pcs(allowed, ram_pcs) == 0)
        pc_cov.ram_pc_allowed = allowed;
    nes_init(&nes, &rom);
    nes_power(&nes, PPU_MODEL_OLD); /* the snapshots bring their own model */
    cov_init(&pc_cov, &rom);
    if (ram_pcs)
        pc_cov.ram_pc_allowed = allowed;
    cov_attach(&pc_cov, &nes);
    nes.log_traps = 0;

    scan_known();
    snprintf(p, sizeof p, "%s.tsv", prefix);
    tsv = fopen(p, "w");
    if (!tsv)
        return 1;
    fprintf(tsv, "bank\taddr\tsource\tdecoded_bytes\tverdicts\tclean\n");
    for (i = 0; i < SZ; i++) {
        int bank = (int)(i >> 15), cnt, clean = 0, s;
        uint16_t addr = (uint16_t)(0x8000 + (i & 0x7FFF));
        char verd[128] = "", src[16] = "";
        if (!cand_src[i] || is_code(i))
            continue;
        if (mark[i]) /* already decoded as part of another candidate: that one's verdict covers it */
            continue;
        n_cand++;
        id++;
        snprintf(src, sizeof src, "%s%s%s%s", cand_src[i] & SRC_BRANCH ? "b" : "", cand_src[i] & SRC_FALL ? "f" : "",
                 cand_src[i] & SRC_JSR ? "j" : "", cand_src[i] & SRC_JMP ? "J" : "");
        {
            static uint8_t tmp[SZ];
            long z;
            memcpy(tmp, mark, SZ);
            cnt = descend(id, bank, addr, tmp, stack, 4096);
            if (cnt < 0) {
                n_rej++;
                fprintf(tsv, "%d\t$%04X\t%s\t-\trejected (not decodable as code)\t0\n", bank, addr, src);
                continue;
            }
            for (s = 0; s < nb; s++) {
                CpuCallResult r;
                snap_copy(&nes, &base[s]);
                cov_reset_tracking(&pc_cov);
                pc_cov.bad_exec = 0;
                nes.prg_bank = (uint8_t)bank;
                nes.cpu.irq = 0; /* no interrupt pending into the probe */
                r = cpu_call(&nes.cpu, addr, (uint64_t)limit);
                if (nes.cpu.jammed || pc_cov.bad_exec)
                    strcat(verd, "crash ");
                else if (r == CPU_CALL_RETURNED)
                    strcat(verd, "rts "), clean = 1;
                else if (r == CPU_CALL_UNWOUND)
                    strcat(verd, "unwind "), clean = 1;
                else
                    strcat(verd, "limit ");
            }
            if (clean) {
                n_clean++;
                clean_bytes += cnt;
                for (z = 0; z < SZ; z++)
                    if (tmp[z] && !mark[z])
                        out[z] |= MESEN_CODE;
                memcpy(mark, tmp, SZ);
                out[i] |= (cand_src[i] & SRC_JSR) ? MESEN_ENTRY : MESEN_JUMP;
            }
        }
        fprintf(tsv, "%d\t$%04X\t%s\t%d\t%s\t%d\n", bank, addr, src, cnt, verd, clean);
    }
    fclose(tsv);
    snprintf(p, sizeof p, "%s-m.cdl", prefix);
    {
        FILE *f = fopen(p, "wb");
        uint8_t hdr[9] = {'C', 'D', 'L', 'v', '2'};
        hdr[5] = (uint8_t)known.prg_crc;
        hdr[6] = (uint8_t)(known.prg_crc >> 8);
        hdr[7] = (uint8_t)(known.prg_crc >> 16);
        hdr[8] = (uint8_t)(known.prg_crc >> 24);
        if (!f)
            return 1;
        fwrite(hdr, 1, 9, f);
        fwrite(out, 1, SZ, f);
        fclose(f);
    }
    printf("candidates %d: rejected %d, clean %d (%ld bytes decoded as code), unclean %d; probe snapshots %d\n",
           n_cand, n_rej, n_clean, clean_bytes, n_cand - n_rej - n_clean, nb);
    free(stack);
    return 0;
}
