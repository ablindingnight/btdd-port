/* The console as the game sees it: 2A03 CPU, 2 KB RAM, PPU, APU register sink, two standard
 * controllers, and the AxROM (mapper 7) cartridge with 8 x 32 KB PRG banks and 8 KB CHR-RAM. */
#ifndef BTDD_NES_H
#define BTDD_NES_H
#include "../rom/rom.h"
#include "../runtime/cpu.h"
#include "ppu.h"
#include <stdint.h>

/* Controller bits, in the order the game shifts them in (bit 0 = A, read first). */
enum {
    PAD_A = 0x01, PAD_B = 0x02, PAD_SELECT = 0x04, PAD_START = 0x08,
    PAD_UP = 0x10, PAD_DOWN = 0x20, PAD_LEFT = 0x40, PAD_RIGHT = 0x80,
};

/* Optional observer for APU register writes (M3 will synthesize sound from these). */
typedef void (*ApuWriteHook)(void *user, uint64_t cpu_cycle, uint16_t addr, uint8_t value);

typedef struct NesStats {
    uint64_t unofficial_ops;   /* unofficial opcodes hit (each is logged once per address) */
    uint64_t bus_conflicts;    /* mapper writes whose value differs from the ROM byte at that address */
    uint64_t apu_status_reads; /* $4015 reads (not modelled beyond a stub) */
    uint64_t dmc_enables;      /* $4015 writes with the DMC bit set (DMC DMA is not modelled) */
    uint64_t mapper_writes;
} NesStats;

typedef struct Nes Nes;
struct Nes {
    Cpu cpu;
    Ppu ppu;
    uint8_t ram[0x800];
    const Rom *rom;
    uint8_t prg_bank;
    uint8_t pad[2];          /* buttons held this frame, per port */
    uint8_t pad_bit[2];      /* next bit to shift out */
    uint8_t strobe;
    uint8_t apu_regs[0x18];
    ApuWriteHook apu_hook;
    void *apu_user;
    NesStats stats;
    int log_traps;           /* print unofficial opcodes to stderr */
    /* debugging: called on every CPU write to RAM addresses with watch[addr & 0x7FF] set */
    uint8_t watch[0x800];
    void (*watch_hook)(Nes *nes, uint16_t addr, uint8_t value);
};

void nes_init(Nes *nes, const Rom *rom);
void nes_power(Nes *nes);             /* FCEUX power-on state (RAM pattern 00x4 FFx4, two dead PPU frames) */
void nes_set_pads(Nes *nes, uint8_t pad1, uint8_t pad2);
void nes_run_frame(Nes *nes);         /* one frame, from the end of line 239 to the end of the next line 239 */
uint8_t nes_peek(Nes *nes, uint16_t addr); /* side-effect-free read for tools */

#endif
