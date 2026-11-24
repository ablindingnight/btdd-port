/* Bus, mapper, controllers and APU sink. Read/write behaviour follows FCEUX 2.6 where real hardware
 * and FCEUX differ, because FCEUX is what the TAS movies were verified in. */
#include "nes.h"
#include <stdio.h>
#include <string.h>

/* ---- controllers ---- */

static uint8_t pad_read(Nes *n, int port)
{
    uint8_t bit = n->pad_bit[port];
    uint8_t ret = bit < 8 ? (uint8_t)((n->pad[port] >> bit) & 1) : 1; /* after 8 reads: 1s */
    n->pad_bit[port] = (uint8_t)(bit + 1); /* wraps like FCEUX's uint8 counter */
    return (uint8_t)(ret | (n->cpu.bus & 0xC0)); /* upper bits are open bus */
}

static void pad_strobe(Nes *n, uint8_t v)
{
    if ((n->strobe & 1) && !(v & 1))
        n->pad_bit[0] = n->pad_bit[1] = 0;
    n->strobe = v & 1;
}

/* ---- mapper 7 ---- */

static void mapper_write(Nes *n, uint16_t addr, uint8_t v)
{
    n->stats.mapper_writes++;
    if (addr >= 0x8000 && rom_read(n->rom, n->prg_bank, addr) != v)
        n->stats.bus_conflicts++; /* real AOROM would AND the value with the ROM byte; FCEUX does not */
    ppu_line_update(&n->ppu);
    n->prg_bank = v & 7;
    n->ppu.nt_page = (v >> 4) & 1;
}

/* ---- APU (sink only) ---- */

static void apu_write(Nes *n, uint16_t addr, uint8_t v)
{
    n->apu_regs[addr - 0x4000] = v;
    if (addr == 0x4015 && (v & 0x10))
        n->stats.dmc_enables++;
    if (n->apu_hook)
        n->apu_hook(n->apu_user, n->cpu.cycles, addr, v);
}

/* ---- CPU bus ---- */

static uint8_t bus_read(void *ctx, uint16_t addr)
{
    Nes *n = ctx;
    if (addr < 0x2000)
        return n->ram[addr & 0x7FF];
    if (addr < 0x4000)
        return ppu_read(&n->ppu, addr & 7);
    if (addr >= 0x8000)
        return rom_read(n->rom, n->prg_bank, addr);
    if (addr == 0x4016 || addr == 0x4017)
        return pad_read(n, addr & 1);
    if (addr == 0x4015) {
        n->stats.apu_status_reads++;
        return n->cpu.bus & 0x20; /* stub: no channel active, no IRQ flags */
    }
    return n->cpu.bus; /* open bus: $4000-$4014, $4018-$7FFF (no PRG-RAM) */
}

static void oam_dma(Nes *n, uint8_t page)
{
    int i;
    for (i = 0; i < 256; i++) {
        uint8_t v;
        cpu_add_cycles(&n->cpu, 1);
        v = bus_read(n, (uint16_t)(page << 8 | i));
        n->cpu.bus = v;
        cpu_add_cycles(&n->cpu, 1);
        ppu_write(&n->ppu, 4, v);
        n->cpu.bus = v;
    }
}

static void bus_write(void *ctx, uint16_t addr, uint8_t v)
{
    Nes *n = ctx;
    if (addr < 0x2000) {
        n->ram[addr & 0x7FF] = v;
        if (n->watch[addr & 0x7FF] && n->watch_hook)
            n->watch_hook(n, addr & 0x7FF, v);
    } else if (addr < 0x4000)
        ppu_write(&n->ppu, addr & 7, v);
    else if (addr == 0x4014)
        oam_dma(n, v);
    else if (addr == 0x4016)
        pad_strobe(n, v);
    else if (addr < 0x4018)
        apu_write(n, addr, v);
    else if (addr >= 0x4020)
        mapper_write(n, addr, v); /* FCEUX latches the whole $4020-$FFFF range for mapper 7 */
}

static int trap(void *ctx, Cpu *cpu, uint16_t pc, uint8_t op)
{
    Nes *n = ctx;
    n->stats.unofficial_ops++;
    if (n->log_traps)
        fprintf(stderr, "unofficial opcode $%02X at bank %d $%04X (cycle %llu): CPU jammed\n", op,
                n->prg_bank, pc, (unsigned long long)cpu->cycles);
    return 0;
}

/* ---- public ---- */

void nes_init(Nes *n, const Rom *rom)
{
    memset(n, 0, sizeof *n);
    n->rom = rom;
    n->log_traps = 1;
    cpu_init(&n->cpu, n, bus_read, bus_write, trap);
}

void nes_power(Nes *n, PpuModel model)
{
    int i;
    for (i = 0; i < 0x800; i++)
        n->ram[i] = (i & 4) ? 0xFF : 0x00;
    n->prg_bank = 0;
    memset(n->pad, 0, sizeof n->pad);
    memset(n->pad_bit, 0, sizeof n->pad_bit);
    n->strobe = 0;
    memset(n->apu_regs, 0, sizeof n->apu_regs);
    memset(&n->stats, 0, sizeof n->stats);
    ppu_power(&n->ppu, &n->cpu, model);
    cpu_power(&n->cpu);
}

void nes_set_pads(Nes *n, uint8_t pad1, uint8_t pad2)
{
    n->pad[0] = pad1;
    n->pad[1] = pad2;
}

void nes_run_frame(Nes *n)
{
    ppu_run_frame(&n->ppu);
}

uint8_t nes_peek(Nes *n, uint16_t addr)
{
    if (addr < 0x2000)
        return n->ram[addr & 0x7FF];
    if (addr >= 0x8000)
        return rom_read(n->rom, n->prg_bank, addr);
    return 0;
}
