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
#include "runtime/cpu.c" /* found through -I<tree>/src, so a merged tree uses its own interpreter */
#undef cpu_run

#define NATIVE_HOST
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

#ifdef BTDD_NATIVE_STATIC
int native_register_all(const NativeApi *api); /* generated code linked into this exe */
#else
int native_jit_load(const Rom *rom, const NativeApi *api, char *err, size_t cap); /* recomp/native_jit.c */
extern double native_jit_seconds_gen, native_jit_seconds_compile, native_jit_seconds_load;
extern int native_jit_cache_hit;
#endif
static const NativeApi api = {NATIVE_API_VERSION, native_table, &native_insns, native_register};

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

static void setup(Cpu *cpu)
{
    const char *e = getenv("BTDD_NATIVE");
    enabled = !(e && e[0] == '0');
    profile_path = getenv("BTDD_NATIVE_PROFILE");
    timing = getenv("BTDD_NATIVE_TIME") != NULL;
    if (profile_path)
        interp_count = calloc(8 * 0x10000, sizeof *interp_count);
    if (enabled) {
#ifdef BTDD_NATIVE_STATIC
        (void)cpu;
        native_register_all(&api);
#else
        char msg[1400];
        if (native_jit_load(((Nes *)cpu->ctx)->rom, &api, msg, sizeof msg) != 0) {
            fprintf(stderr, "native: %s; running the interpreter only\n", msg);
            enabled = 0;
        } else if (!getenv("BTDD_NATIVE_QUIET"))
            fprintf(stderr, "native: %s (generate %.2f s, compile %.2f s, load+register %.3f s)\n", msg,
                    native_jit_seconds_gen, native_jit_seconds_compile, native_jit_seconds_load);
#endif
    }
    atexit(report);
}

void cpu_run(Cpu *cpu, int32_t dots)
{
    double t0;
    if (enabled < 0)
        setup(cpu);
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
        if (cpu->pc_hook_map && cpu->pc_hook_map[cpu->pc] && cpu->pc_hook(cpu->ctx, cpu))
            continue; /* M7 mods: same point as the interpreter's cpu_run */
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
