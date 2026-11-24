/* Coverage recorder: which PRG bytes the game executes and reads, for btdd-decomp's code/data tools.
 *
 * Two code/data logs are kept per PRG byte (8 banks x 32 KB, file offset = bank * 0x8000 + addr - 0x8000):
 *  - FCEUX format (raw 256 KB, no header), with FCEUX 2.6's rules so the result can be compared and
 *    merged with logs FCEUX made: bit 0 code, bit 1 data, bits 2-3 = which $2000 quarter of $8000-$FFFF the
 *    byte was seen at, bit 4 = code reached through JMP (ind), bit 5 = data read through (zp,X)/(zp),Y.
 *    Logging happens once per instruction, before it runs, with the effective address computed the way
 *    FCEUX's debugger does (one data byte per instruction, JMP abs targets count as data, interrupt vectors
 *    count as data). FCEUX also clears a byte's flags when the program writes to it (AxROM bank switches);
 *    that is optional here (cov.fceux_clear_on_write) because it throws information away.
 *  - Mesen format ("CDLv2" + CRC32 of PRG + 256 KB): bit 0 code, 1 data, 2 jump target, 3 subroutine entry
 *    (JSR target or interrupt handler). btdd-decomp's cdl.py uses the entry bit to name sub_ vs loc_.
 *
 * Reads table: every data read from PRG (same effective address rule, plus the second byte of JMP (ind)
 * pointers, without JMP abs targets) as (byte, reading instruction, innermost routine, level $000D,
 * section $0015) -> count. The routine comes from a shadow call stack (JSR / interrupt push, unwound when
 * S rises above the entry level, so RTS, RTI and pla/pla exits all pop it).
 */
#ifndef BTDD_COVERAGE_H
#define BTDD_COVERAGE_H
#include "nes/nes.h"
#include <stdint.h>
#include <stdio.h>

#define COV_PRG_SIZE 0x40000

enum { MESEN_CODE = 1, MESEN_DATA = 2, MESEN_JUMP = 4, MESEN_ENTRY = 8 };

typedef struct CovRead {
    uint64_t key;  /* prg offset (18) | reader pc (16) | routine addr (16) | routine bank (3) */
    uint16_t area; /* level << 8 | section */
    uint32_t count;
} CovRead;

typedef struct CovFrame {
    uint16_t entry;
    uint8_t bank, s;
} CovFrame;

typedef struct Coverage {
    uint8_t fceux[COV_PRG_SIZE];
    uint8_t mesen[COV_PRG_SIZE];
    int fceux_clear_on_write;
    int log_reads;
    /* novelty counters (bytes that got a new code / data bit since cov_init or the last reset of these) */
    uint64_t new_code, new_data;
    uint64_t ram_exec;  /* instructions fetched below $8000 (the game runs some code from RAM) */
    uint8_t ram_pc_seen[0x800];      /* RAM addresses an instruction started at */
    const uint8_t *ram_pc_allowed;   /* optional: RAM PCs known to be legitimate (from the TAS) */
    uint64_t bad_exec;               /* instructions at a RAM PC not in ram_pc_allowed: crash sign */
    /* per-instruction tracking */
    int have_last, indirect_next, pending_entry;
    uint16_t last_pc;
    uint8_t last_op, last_size;
    CovFrame stack[256];
    int sp;
    /* reads table (open addressing) */
    CovRead *reads;
    uint32_t reads_cap, reads_used;
    uint32_t prg_crc;
    /* indirect targets: jmp (ptr) site -> target, and far calls through the $FFCB/$FFCD trampoline
     * (caller site -> bank Y, slot $14:X), key = kind(1) site bank(3) site pc(16) target bank(3) target pc(16) */
    int log_indirect;
    uint8_t last_bank;
    uint64_t *ind_key;
    uint32_t *ind_count;
    uint32_t ind_cap, ind_used;
} Coverage;

void cov_init(Coverage *cov, const Rom *rom);
void cov_free(Coverage *cov);
void cov_attach(Coverage *cov, Nes *nes);   /* installs the CPU hooks (nes->user = cov) */
void cov_detach(Nes *nes);
void cov_reset_tracking(Coverage *cov);     /* after loading a snapshot: forget the last instruction / stack */

/* OR an existing log into ours (FCEUX raw or Mesen "CDLv" file, as btdd-decomp's cdl.py reads them). */
int cov_merge_file(Coverage *cov, const char *path);
int cov_write_fceux(const Coverage *cov, const char *path);
int cov_write_mesen(const Coverage *cov, const char *path);
int cov_write_reads(const Coverage *cov, const char *path);
/* indirect targets: kind (jmpind / farcall), site bank, site pc, target bank, target pc, count;
 * a far call's target is the slot at $8000+X of bank Y, followed by the slot's own JMP target */
int cov_write_indirect(const Coverage *cov, const Rom *rom, const char *path);
/* RAM instruction addresses: one hex address per line */
int cov_write_ram_pcs(const Coverage *cov, const char *path);
int cov_load_ram_pcs(uint8_t map[0x800], const char *path);

/* instruction length in bytes (official opcodes) */
int cov_opsize(uint8_t op);

/* counts over the FCEUX-format log */
void cov_count(const uint8_t *fceux, int bank, long *code, long *data, long *both, long *seen);

#endif
