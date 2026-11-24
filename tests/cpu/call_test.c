/* Checks cpu_call (run a routine in the interpreter from native code) on a tiny hand-written program. */
#include "runtime/cpu.h"
#include <stdio.h>

static uint8_t mem[0x10000];
static uint8_t rd(void *ctx, uint16_t a) { (void)ctx; return mem[a]; }
static void wr(void *ctx, uint16_t a, uint8_t v) { (void)ctx; mem[a] = v; }

static int fails;
static void check(int ok, const char *what)
{
    printf("%s: %s\n", ok ? "ok  " : "FAIL", what);
    fails += !ok;
}

int main(void)
{
    Cpu cpu;
    static const uint8_t prog[] = {
        /* $8000 */ 0xA9, 0x42,       /* lda #$42  */
        /* $8002 */ 0x85, 0x10,       /* sta $10   */
        /* $8004 */ 0x20, 0x10, 0x80, /* jsr $8010 */
        /* $8007 */ 0x60,             /* rts       */
    };
    static const uint8_t sub[] = {
        /* $8010 */ 0xE6, 0x10,       /* inc $10   */
        /* $8012 */ 0x60,             /* rts       */
    };
    static const uint8_t unwind[] = {
        /* $8020 */ 0x68, 0x68,       /* pla, pla: drop our return address */
        /* $8022 */ 0x60,             /* rts to whatever is above it */
    };
    CpuCallResult r;
    uint64_t c0;
    int i;

    for (i = 0; i < (int)sizeof prog; i++) mem[0x8000 + i] = prog[i];
    for (i = 0; i < (int)sizeof sub; i++) mem[0x8010 + i] = sub[i];
    for (i = 0; i < (int)sizeof unwind; i++) mem[0x8020 + i] = unwind[i];

    cpu_init(&cpu, NULL, rd, wr, NULL);
    cpu_power(&cpu);
    cpu.irq = 0;
    cpu.pc = 0x9000;
    cpu.s = 0xFD;
    c0 = cpu.cycles;
    r = cpu_call(&cpu, 0x8000, 0);
    check(r == CPU_CALL_RETURNED, "routine returns");
    check(cpu.pc == 0x9000 && cpu.s == 0xFD, "PC and S restored");
    check(mem[0x10] == 0x43, "routine and its callee ran");
    check(cpu.cycles - c0 == 2 + 3 + 6 + 5 + 6 + 6, "cycles charged (no JSR for the entry)");

    /* a routine that exits through its caller's frame */
    mem[0x1FD] = 0x12; mem[0x1FE] = 0x34; /* pretend caller return address $3412 sits above S */
    cpu.s = 0xFC;
    r = cpu_call(&cpu, 0x8020, 0);
    check(r == CPU_CALL_UNWOUND && cpu.pc == 0x3413 && cpu.s == 0xFE, "stack unwind reported");

    cpu.pc = 0x9000; cpu.s = 0xFD;
    mem[0x8030] = 0x4C; mem[0x8031] = 0x30; mem[0x8032] = 0x80; /* jmp $8030 forever */
    r = cpu_call(&cpu, 0x8030, 100);
    check(r == CPU_CALL_LIMIT, "instruction limit");
    return fails ? 1 : 0;
}
