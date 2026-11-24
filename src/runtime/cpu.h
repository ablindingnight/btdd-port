/* 6502 (2A03 core, no decimal mode) interpreter: the reference CPU for the port.
 *
 * Timing model: the same one FCEUX 2.6 uses with its "old PPU", because the TAS movies that are our
 * ground truth only sync there. Every instruction charges its base cycle count when its opcode is
 * fetched; page-cross and branch extras are charged when they happen. Bus accesses then see the time
 * after those charges. Dummy reads/writes happen where FCEUX does them (indexed page-cross reads,
 * indexed store/RMW reads, RMW double write), since a dummy access to $2002/$2007 has side effects.
 *
 * The CPU runs against a budget measured in 1/16 PPU dots (one CPU cycle = 48). cpu_run() adds to the
 * budget and executes whole instructions while it is positive; the overshoot carries to the next call.
 */
#ifndef BTDD_CPU_H
#define BTDD_CPU_H
#include <stdint.h>

enum {
    CPU_C = 0x01, CPU_Z = 0x02, CPU_I = 0x04, CPU_D = 0x08,
    CPU_B = 0x10, CPU_U = 0x20, CPU_V = 0x40, CPU_N = 0x80,
};

/* Pending-interrupt bits in Cpu.irq. The low bits are level IRQ sources (any set = IRQ line low). */
enum {
    CPU_IRQ_FRAME = 0x01, /* APU frame counter */
    CPU_IRQ_DMC   = 0x02,
    CPU_IRQ_MASK  = 0x0F,
    CPU_INT_NMI   = 0x10, /* NMI taken before the next instruction */
    CPU_INT_NMI2  = 0x20, /* NMI taken one instruction later (enabled via $2000 during vblank) */
    CPU_INT_RESET = 0x40,
};

#define CPU_CYCLE_UNITS 48 /* budget units per CPU cycle (16 per PPU dot) */

typedef struct Cpu Cpu;
typedef uint8_t (*CpuRead)(void *ctx, uint16_t addr);
typedef void (*CpuWrite)(void *ctx, uint16_t addr, uint8_t value);
/* Called for an opcode outside the official set. Return nonzero to keep running (opcode treated as a
 * 1-byte NOP of its listed cycle count); zero jams the CPU. */
typedef int (*CpuTrap)(void *ctx, Cpu *cpu, uint16_t pc, uint8_t opcode);

struct Cpu {
    uint16_t pc;
    uint8_t a, x, y, s, p;
    uint8_t p_before; /* P as it was before the last instruction: IRQ polling uses its I flag */
    uint8_t irq;
    uint8_t jammed;
    uint8_t bus;      /* last value on the data bus (open bus) */
    int32_t budget;   /* see header comment */
    uint64_t cycles;  /* CPU cycles since power-on */
    uint64_t instructions;
    uint8_t last_op;  /* opcode of the last instruction executed */
    void *ctx;
    CpuRead read;
    CpuWrite write;
    CpuTrap trap;
    /* debugging: if set, called before each instruction (after its base cycles are charged, which is
     * where FCEUX's exec hooks fire) when exec_watch[pc] is nonzero */
    void (*exec_hook)(void *ctx, Cpu *cpu);
    const uint8_t *exec_watch; /* 64 KB map */
    /* coverage: if set, called before every instruction (before its opcode fetch, registers as they are
     * before it runs; FCEUX's DebugCycle point), and when an interrupt or reset reads its vector */
    void (*insn_hook)(void *ctx, Cpu *cpu);
    void (*vector_hook)(void *ctx, Cpu *cpu, uint16_t vector);
    /* APU: if set, called at the start of every instruction after its base cycles are charged (where FCEUX
     * advances its sound hardware); may stall the CPU (DMC fetch) or raise IRQ lines */
    void (*tick_hook)(void *ctx, Cpu *cpu);
    /* M7 mods: when an instruction is about to run in cpu_run (interrupts polled, budget left) and
     * pc_hook_map[pc] is nonzero, pc_hook is called first. It returns nonzero if it moved PC (a replaced
     * routine returned, ...); cpu_run then starts over at the new PC (budget, interrupts, hooks). Zero: the
     * instruction runs as usual. The native run loop (recomp/native_cpu.c) honours the same map. NULL = off. */
    const uint8_t *pc_hook_map; /* 64 KB */
    int (*pc_hook)(void *ctx, Cpu *cpu);
};

void cpu_init(Cpu *cpu, void *ctx, CpuRead read, CpuWrite write, CpuTrap trap);
void cpu_power(Cpu *cpu);              /* registers to FCEUX power-on values, reset pending */
void cpu_reset(Cpu *cpu);              /* reset pending; taken at the next cpu_run/cpu_step */
void cpu_run(Cpu *cpu, int32_t dots);  /* give the CPU `dots` more PPU dots of time and run */
void cpu_step(Cpu *cpu);               /* one interrupt entry or one instruction, ignoring the budget */
void cpu_add_cycles(Cpu *cpu, int n);  /* stall (DMA): charge cycles without executing */

/* ---- API for generated/native code sharing this CPU (load-time recompiler, overrides) ----
 * Native code keeps A/X/Y/S/P/PC in this struct, does its memory accesses through cpu_bus_read/write (same
 * side effects and open-bus value as the interpreter) and charges time with cpu_add_cycles. */
uint8_t cpu_bus_read(Cpu *cpu, uint16_t addr);
void cpu_bus_write(Cpu *cpu, uint16_t addr, uint8_t value);

typedef enum CpuCallResult {
    CPU_CALL_RETURNED = 0, /* the routine's RTS brought S back to the entry level */
    CPU_CALL_UNWOUND,      /* S rose above the entry level without that RTS (pla/pla tail exits etc.) */
    CPU_CALL_JAMMED,       /* unofficial opcode trapped */
    CPU_CALL_LIMIT,        /* max_instructions executed */
} CpuCallResult;

/* Runs the 6502 routine at `addr` in the interpreter as if called with JSR from the current PC: pushes
 * PC-1 (no cycles charged: the caller already paid for its JSR), then steps (interrupts included) until
 * the matching RTS. Afterwards PC is the caller's PC again. Steps ignore the frame budget, but every
 * cycle is still charged to `cycles`/`budget`. max_instructions 0 = no limit. */
CpuCallResult cpu_call(Cpu *cpu, uint16_t addr, uint64_t max_instructions);

/* Cycle count charged at fetch for each opcode (official ones match the 6502 datasheet). */
extern const uint8_t cpu_base_cycles[256];
/* 1 for the 151 official opcodes. */
extern const uint8_t cpu_official[256];

#endif
