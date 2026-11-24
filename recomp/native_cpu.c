/* CPU run loop with native dispatch (M5 prototype).
 *
 * This file compiles the reference interpreter (src/runtime/cpu.c) into the same translation unit with its
 * cpu_run renamed, and provides a cpu_run that, before each instruction, looks up (PRG bank, PC) in a table of
 * generated native functions. A link of the test exe uses this file INSTEAD of src/runtime/cpu.c, so the
 * interpreter source stays untouched (owned by other work) and the native path still shares its exact
 * interrupt polling and execute().
 *
 * Contract of a native function f(c), entered with c->pc = an address it registered:
 *   - executes the instruction at c->pc unconditionally (the loop below has just done the budget/interrupt
 *     checks the interpreter does), then before every further instruction returns if budget <= 0 or an
 *     interrupt is pending, with c->pc = that instruction;
 *   - returns with c->pc = the next address on any control transfer it does not handle itself (JSR, RTS, RTI,
 *     indirect jumps, jumps out of the routine, after a store that may switch banks).
 * Environment: BTDD_NATIVE=0 turns dispatch off (pure interpreter, same binary).
 */
#define cpu_run cpu_run_interp
#include "../src/runtime/cpu.c"
#undef cpu_run

#include "native_rt.h"
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

uint64_t native_insns;
uint64_t native_calls;
NativeFn native_table[8][0x8000];
#define table native_table
static int enabled = -1;
static uint32_t *interp_count; /* BTDD_NATIVE_PROFILE=file: interpreted instructions per (bank, PC) */
static const char *profile_path;
static int timing;            /* BTDD_NATIVE_TIME=1: measure wall time spent inside cpu_run */
static double cpu_seconds;

#ifdef _WIN32
#include <windows.h>
static double now(void)
{
    static double scale;
    LARGE_INTEGER t;
    if (!scale) {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        scale = 1.0 / (double)f.QuadPart;
    }
    QueryPerformanceCounter(&t);
    return (double)t.QuadPart * scale;
}
#else
static double now(void)
{
    struct timespec ts;
    timespec_get(&ts, TIME_UTC);
    return (double)ts.tv_sec + ts.tv_nsec * 1e-9;
}
#endif

static void run(Cpu *cpu, int32_t dots);

void native_register(int bank, NativeFn fn, const uint16_t *addrs, int count)
{
    int i;
    for (i = 0; i < count; i++)
        table[bank & 7][addrs[i] - 0x8000] = fn;
}

void native_register_all(void); /* generated */

static void report(void)
{
    if (interp_count && profile_path) {
        FILE *f = fopen(profile_path, "w");
        int i;
        if (f) {
            for (i = 0; i < 8 * 0x10000; i++)
                if (interp_count[i])
                    fprintf(f, "%d %04X %lu\n", i >> 16, i & 0xFFFF, (unsigned long)interp_count[i]);
            fclose(f);
        }
    }
    if (getenv("BTDD_NATIVE_QUIET"))
        return;
    fprintf(stderr, "native: %s, %llu native instructions, %llu native calls\n", enabled ? "on" : "off",
            (unsigned long long)native_insns, (unsigned long long)native_calls);
    if (timing)
        fprintf(stderr, "native: %.2f s inside cpu_run (timer overhead included)\n", cpu_seconds);
}

static void setup(void)
{
    const char *e = getenv("BTDD_NATIVE");
    enabled = !(e && e[0] == '0');
    profile_path = getenv("BTDD_NATIVE_PROFILE");
    timing = getenv("BTDD_NATIVE_TIME") != NULL;
    if (profile_path)
        interp_count = calloc(8 * 0x10000, sizeof *interp_count);
    if (enabled)
        native_register_all();
    atexit(report);
}

void cpu_run(Cpu *cpu, int32_t dots)
{
    double t0;
    if (enabled < 0)
        setup();
    if (!timing) {
        run(cpu, dots);
        return;
    }
    t0 = now();
    run(cpu, dots);
    cpu_seconds += now() - t0;
}

static void run(Cpu *cpu, int32_t dots)
{
    Nes *n = (Nes *)cpu->ctx;
    if (!enabled || cpu->exec_hook) {
        cpu_run_interp(cpu, dots);
        return;
    }
    cpu->budget += dots * 16;
    while (cpu->budget > 0) {
        if (cpu->irq) {
            poll_interrupts(cpu);
            if (cpu->budget <= 0) {
                cpu->p_before = cpu->p;
                return;
            }
        }
        if (cpu->jammed) {
            add_cycles(cpu, cpu_base_cycles[0x02]);
            continue;
        }
        if (cpu->pc >= 0x8000) {
            NativeFn f = table[n->prg_bank][cpu->pc - 0x8000];
            if (f) {
                native_calls++;
                f(cpu);
                continue;
            }
        }
        if (interp_count)
            interp_count[n->prg_bank << 16 | cpu->pc]++;
        execute(cpu);
    }
}
