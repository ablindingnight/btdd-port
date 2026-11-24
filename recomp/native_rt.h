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
#include <stdint.h>

typedef void (*NativeFn)(Cpu *c);

/* counters kept by generated code (defined in recomp/native_cpu.c) */
extern uint64_t native_insns;

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

/* registration: generated code fills table[bank][addr - $8000] */
void native_register(int bank, NativeFn fn, const uint16_t *addrs, int count);

#endif
