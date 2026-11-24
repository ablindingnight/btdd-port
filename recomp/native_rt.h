/* Helpers for generated native code (M5 prototype). Included by every generated file.
 *
 * Generated code must leave the machine in exactly the state the interpreter (src/runtime/cpu.c) would:
 * same registers and flags, same bus accesses in the same order at the same cycle counts, same open-bus
 * value before every data access. Only the opcode/operand fetches from ROM are skipped (they have no side
 * effect besides setting the open-bus value, which generated code sets directly where it matters).
 *
 * RAM ($0000-$1FFF) is accessed directly (same effect as the bus callback, including the RAM watch hook).
 * Everything else goes through the Cpu's read/write callbacks.
 */
#ifndef BTDD_NATIVE_RT_H
#define BTDD_NATIVE_RT_H
#include "nes/nes.h"
#include <stddef.h>
#include <stdint.h>

typedef void (*NativeFn)(Cpu *c);

/* What the host gives generated code at registration. Generated code references nothing else from the host,
 * so it can live in the exe, in a DLL built by any compiler, or in memory compiled by libtcc. */
typedef struct NativeApi {
    uint32_t version;                 /* NATIVE_API_VERSION */
    NativeFn (*table)[0x8000];        /* [bank][addr - $8000] */
    uint64_t *insns;                  /* native instruction counter */
    void (*reg)(int bank, NativeFn fn, const uint16_t *addrs, int count);
} NativeApi;
#define NATIVE_API_VERSION 1

#if defined(_WIN32)
#define NATIVE_EXPORT __declspec(dllexport)
#else
#define NATIVE_EXPORT __attribute__((visibility("default")))
#endif

/* Struct layout as seen by the compiler of the generated code; the host refuses code built with another. */
#define NATIVE_LAYOUT_LIST(X) X(sizeof(Cpu)) X(offsetof(Cpu, budget)) X(offsetof(Cpu, bus)) X(offsetof(Cpu, ctx))     X(offsetof(Cpu, write)) X(sizeof(Nes)) X(offsetof(Nes, ram)) X(offsetof(Nes, prg_bank)) X(offsetof(Nes, watch))     X(offsetof(Nes, watch_hook))

#ifndef NATIVE_HOST
/* generated code: the API pointer, one per translation unit */
static inline const NativeApi **n_api_slot(void)
{
    static const NativeApi *p;
    return &p;
}
#define n_api (*n_api_slot())
#define native_insns (*n_api->insns)
#define native_register(b, f, a, k) n_api->reg(b, f, a, k)
#define native_table (n_api->table)
#else
extern uint64_t native_insns;
extern NativeFn native_table[8][0x8000];
void native_register(int bank, NativeFn fn, const uint16_t *addrs, int count);
#endif

#define NES(c) ((Nes *)(c)->ctx)

static inline void n_cyc(Cpu *c, int n)
{
    c->cycles += (uint64_t)n;
    c->budget -= n * CPU_CYCLE_UNITS;
}

static inline uint8_t n_rd(Cpu *c, uint16_t a)
{
    c->bus = c->read(c->ctx, a);
    return c->bus;
}

static inline void n_wr(Cpu *c, uint16_t a, uint8_t v)
{
    c->write(c->ctx, a, v);
    c->bus = v;
}

static inline uint8_t n_rdram(Cpu *c, uint16_t a)
{
    c->bus = NES(c)->ram[a & 0x7FF];
    return c->bus;
}

static inline void n_wrram(Cpu *c, uint16_t a, uint8_t v)
{
    Nes *n = NES(c);
    n->ram[a & 0x7FF] = v;
    if (n->watch[a & 0x7FF] && n->watch_hook)
        n->watch_hook(n, (uint16_t)(a & 0x7FF), v);
    c->bus = v;
}

/* address only known at run time */
static inline uint8_t n_rdx(Cpu *c, uint16_t a)
{
    return a < 0x2000 ? n_rdram(c, a) : n_rd(c, a);
}

static inline void n_wrx(Cpu *c, uint16_t a, uint8_t v)
{
    if (a < 0x2000)
        n_wrram(c, a, v);
    else
        n_wr(c, a, v);
}

static inline void n_push(Cpu *c, uint8_t v)
{
    n_wrram(c, (uint16_t)(0x100 | c->s), v);
    c->s--;
}

static inline uint8_t n_pop(Cpu *c)
{
    c->s++;
    return n_rdram(c, (uint16_t)(0x100 | c->s));
}

static inline void n_zn(Cpu *c, uint8_t v)
{
    c->p = (uint8_t)((c->p & ~(CPU_Z | CPU_N)) | (v == 0 ? CPU_Z : 0) | (v & CPU_N));
}

static inline void n_adc(Cpu *c, uint8_t v)
{
    unsigned sum = (unsigned)c->a + v + (c->p & CPU_C);
    c->p &= (uint8_t)~(CPU_C | CPU_V);
    if (~(c->a ^ v) & (c->a ^ sum) & 0x80)
        c->p |= CPU_V;
    if (sum > 0xFF)
        c->p |= CPU_C;
    c->a = (uint8_t)sum;
    n_zn(c, c->a);
}

static inline void n_sbc(Cpu *c, uint8_t v)
{
    n_adc(c, (uint8_t)~v);
}

static inline void n_cmp(Cpu *c, uint8_t reg, uint8_t v)
{
    c->p = (uint8_t)((c->p & ~CPU_C) | (reg >= v ? CPU_C : 0));
    n_zn(c, (uint8_t)(reg - v));
}

static inline void n_bit(Cpu *c, uint8_t v)
{
    c->p = (uint8_t)((c->p & ~(CPU_Z | CPU_V | CPU_N)) | (v & (CPU_V | CPU_N)) | ((c->a & v) ? 0 : CPU_Z));
}

static inline uint8_t n_asl(Cpu *c, uint8_t v)
{
    c->p = (uint8_t)((c->p & ~CPU_C) | (v >> 7));
    v = (uint8_t)(v << 1);
    n_zn(c, v);
    return v;
}

static inline uint8_t n_lsr(Cpu *c, uint8_t v)
{
    c->p = (uint8_t)((c->p & ~CPU_C) | (v & 1));
    v >>= 1;
    n_zn(c, v);
    return v;
}

static inline uint8_t n_rol(Cpu *c, uint8_t v)
{
    uint8_t carry = c->p & CPU_C;
    c->p = (uint8_t)((c->p & ~CPU_C) | (v >> 7));
    v = (uint8_t)(v << 1 | carry);
    n_zn(c, v);
    return v;
}

static inline uint8_t n_ror(Cpu *c, uint8_t v)
{
    uint8_t carry = c->p & CPU_C;
    c->p = (uint8_t)((c->p & ~CPU_C) | (v & 1));
    v = (uint8_t)(v >> 1 | carry << 7);
    n_zn(c, v);
    return v;
}

static inline uint8_t n_inc(Cpu *c, uint8_t v)
{
    v++;
    n_zn(c, v);
    return v;
}

static inline uint8_t n_dec(Cpu *c, uint8_t v)
{
    v--;
    n_zn(c, v);
    return v;
}

/* The native function for c->pc if the run loop would call it right now (budget left, no interrupt
 * pending), else NULL. */
static inline NativeFn n_next(Cpu *c)
{
    if (c->budget <= 0 || c->irq || c->pc < 0x8000 || (c->pc_hook_map && c->pc_hook_map[c->pc]))
        return 0; /* a hooked address (M7 mods) always goes back through the run loop */
    return native_table[NES(c)->prg_bank][c->pc - 0x8000];
}

/* Leave the current function with c->pc set. With clang and -DBTDD_CHAIN, chain straight into the next native function
 * (a guaranteed tail call, so the C stack does not grow); otherwise return to the run loop. */
#if defined(__clang__) && defined(BTDD_CHAIN)
#define N_EXIT do { NativeFn f_ = n_next(c); if (f_) { __attribute__((musttail)) return f_(c); } return; } while (0)
#else
#define N_EXIT return
#endif

/* Per-instruction CPU hooks, at the same points as execute() in src/runtime/cpu.c:
 *   N_PRE  before the instruction (Cpu.insn_hook: coverage), N_TICK after its base cycles (Cpu.tick_hook: APU).
 * Both set c->pc to the instruction first, as the interpreter has it there. Trees whose Cpu has these hooks are
 * built with -DBTDD_CPU_HOOKS (the build scripts and CMake detect it from cpu.h); elsewhere they cost nothing.
 * Fast blocks are only taken with no hook set (a hook may raise an interrupt or stall in the middle). */
#ifdef BTDD_CPU_HOOKS
#define N_PRE(c, a) do { if ((c)->insn_hook) { (c)->pc = (a); (c)->insn_hook((c)->ctx, (c)); } } while (0)
#define N_TICK(c, a) do { if ((c)->tick_hook) { (c)->pc = (a); (c)->tick_hook((c)->ctx, (c)); } } while (0)
#define N_FASTOK(c) (!(c)->tick_hook && !(c)->insn_hook)
#else
#define N_PRE(c, a) ((void)0)
#define N_TICK(c, a) ((void)0)
#define N_FASTOK(c) 1
#endif

#endif
