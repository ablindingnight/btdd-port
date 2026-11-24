/* M6 candidate API: everything a decompiled routine may use (private decompile, never shipped).
 *
 * A candidate is one function:  void routine(M6 *m)
 *   - registers on entry are in m->a, m->x, m->y and the flags m->c, m->z, m->n, m->v (0 or 1);
 *     set the ones the routine returns before returning (the test compares the routine's declared outputs)
 *   - RAM: RAM(addr) is the CPU RAM byte at addr ($0000-$07FF, mirrored up to $1FFF); names from the RAM map are
 *     #defined as addresses in m6_names.h, e.g. RAM(ent_hp + m->x)
 *   - anything else (PPU/APU/controller registers, ROM tables of the mapped bank, bank switching) goes through
 *     bus_read / bus_write, in the same order as the original code does it
 * No cycle counting (the test runs in cycle-free mode). No stack: locals are plain C variables.
 */
#ifndef BTDD_M6_API_H
#define BTDD_M6_API_H
#include <stdint.h>

typedef struct M6 {
    uint8_t a, x, y;
    uint8_t c, z, n, v;
    uint8_t *ram;
    void *ctx;
} M6;

uint8_t bus_read(M6 *m, uint16_t addr);
void bus_write(M6 *m, uint16_t addr, uint8_t value);
#define RAM(addr) (m->ram[(addr) & 0x7FF])

void routine(M6 *m);

/* For routines that call others: each callee is a C function of the same shape, e.g. sub_8624(m). Callees
 * without a passing C translation run as original code through m6_call (provided by the harness). */
void m6_call(M6 *m, uint16_t addr);

/* Far call (`ldx #slot / ldy #bank_index / jsr $FFCB`): sets m->x = slot, m->y = bank_index and runs the original
 * trampoline (bank switch, jlist dispatch, the target, switch back); registers and flags come back in m. */
void far_call(M6 *m, uint8_t bank_index, uint8_t slot);

/* Indirect jump (`jmp (ptr)`): runs the code at target with the registers in m, then the routine is finished
 * (write `jump_to(m, target); return;`). A tail jump to a known routine is just a call followed by return. */
void jump_to(M6 *m, uint16_t target);

#endif
