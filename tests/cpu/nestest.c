/* CPU check against kevtris's nestest.nes "automation" log (registers and cycle count before every
 * instruction). Run: nestest_check path\to\nestest.nes path\to\nestest.log
 * The official-opcode part of the log must match exactly; the run stops at the first unofficial opcode,
 * since the reference CPU traps those instead of executing them. */
#include "runtime/cpu.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint8_t mem[0x10000];
static uint8_t prg[0x4000];

static uint8_t rd(void *ctx, uint16_t a)
{
    (void)ctx;
    if (a < 0x2000)
        return mem[a & 0x7FF];
    if (a >= 0x8000)
        return prg[a & 0x3FFF];
    return 0xFF;
}

static void wr(void *ctx, uint16_t a, uint8_t v)
{
    (void)ctx;
    if (a < 0x2000)
        mem[a & 0x7FF] = v;
}

static int trapped;
static int trap(void *ctx, Cpu *cpu, uint16_t pc, uint8_t op)
{
    (void)ctx;
    (void)cpu;
    printf("stopped at unofficial opcode $%02X at $%04X\n", op, pc);
    trapped = 1;
    return 0;
}

static int field_hex(const char *line, const char *key, unsigned *out)
{
    const char *p = strstr(line, key);
    if (!p)
        return 0;
    *out = (unsigned)strtoul(p + strlen(key), NULL, 16);
    return 1;
}

int main(int argc, char **argv)
{
    FILE *f;
    Cpu cpu;
    char line[256];
    long n = 0;
    unsigned pc, a = 0, x = 0, y = 0, p = 0, sp = 0;
    unsigned long long cyc;

    if (argc < 3) {
        fprintf(stderr, "usage: nestest_check nestest.nes nestest.log\n");
        return 2;
    }
    f = fopen(argv[1], "rb");
    if (!f || fseek(f, 16, SEEK_SET) != 0 || fread(prg, 1, sizeof prg, f) != sizeof prg) {
        fprintf(stderr, "cannot read %s\n", argv[1]);
        return 2;
    }
    fclose(f);
    f = fopen(argv[2], "r");
    if (!f) {
        fprintf(stderr, "cannot read %s\n", argv[2]);
        return 2;
    }

    cpu_init(&cpu, NULL, rd, wr, trap);
    cpu_power(&cpu);
    cpu.irq = 0;
    cpu.pc = 0xC000;
    cpu.p = 0x24;
    cpu.s = 0xFD;
    cpu.cycles = 7;

    while (fgets(line, sizeof line, f)) {
        const char *c;
        unsigned my_p = (cpu.p & 0xCF) | 0x20;
        pc = (unsigned)strtoul(line, NULL, 16);
        field_hex(line, "A:", &a);
        field_hex(line, "X:", &x);
        field_hex(line, "Y:", &y);
        field_hex(line, "P:", &p);
        field_hex(line, "SP:", &sp);
        c = strstr(line, "CYC:");
        cyc = c ? strtoull(c + 4, NULL, 10) : 0;
        n++;
        if (line[15] == '*') { /* unofficial opcode in the log: official part done */
            printf("reached the unofficial-opcode section at log line %ld\n", n);
            break;
        }
        if (pc != cpu.pc || a != cpu.a || x != cpu.x || y != cpu.y || p != my_p || sp != cpu.s ||
            cyc != cpu.cycles) {
            printf("MISMATCH at log line %ld:\n  log: %s", n, line);
            printf("  cpu: %04X A:%02X X:%02X Y:%02X P:%02X SP:%02X CYC:%llu\n", cpu.pc, cpu.a, cpu.x,
                   cpu.y, my_p, cpu.s, (unsigned long long)cpu.cycles);
            return 1;
        }
        cpu_step(&cpu);
        if (trapped)
            break;
    }
    fclose(f);
    printf("nestest: %ld log lines matched; result bytes $02=%02X $03=%02X (00 00 = official tests passed)\n",
           n - 1, mem[2], mem[3]);
    return (mem[2] == 0 && mem[3] == 0) ? 0 : 1;
}
