/* PPU, milestone M1 level: registers, VRAM, OAM, vblank/NMI timing, sprite-0 hit and sprite overflow.
 *
 * The timing follows FCEUX 2.6's "old PPU" (the TAS movies only sync there): the frame is a fixed
 * schedule of CPU time slices per scanline, and the background of the current scanline is drawn
 * lazily, up to the CPU's current dot, whenever the program touches a PPU register or the mapper.
 * Sprite-0 hit is the first opaque sprite-0 pixel over an opaque background pixel among the pixels
 * drawn so far. This is an approximation of real hardware that FCEUX made; we copy its behaviour,
 * not its code.
 *
 * The background line buffer is kept (palette index, bit 6 = transparent background pixel) so M2 can
 * build the real renderer on top of it. Sprites are only evaluated, not drawn, for now.
 */
#ifndef BTDD_PPU_H
#define BTDD_PPU_H
#include <stdint.h>

struct Cpu;

#define PPU_W 256
#define PPU_H 240

typedef struct PpuSprite {
    uint8_t lo, hi, attr, x; /* pattern bytes for the line, attributes, X */
} PpuSprite;

typedef struct Ppu {
    /* registers */
    uint8_t ctrl, mask, status, oam_addr;
    uint8_t oam_low;   /* FCEUX quirk: separate index for the first 8 OAM bytes after a $2003 write */
    uint8_t latch;     /* last value written to / read from a PPU register */
    uint8_t read_buf;  /* $2007 read buffer */
    uint8_t fine_x, toggle;
    uint32_t v, t;     /* VRAM address / temporary address (wider than 15 bits on purpose, as in FCEUX) */

    /* memory */
    uint8_t chr[0x2000];   /* CHR-RAM */
    uint8_t nt[2][0x400];  /* CIRAM, single-screen: nt_page selects which 1 KB is seen everywhere */
    uint8_t nt_page;
    uint8_t pal[32];       /* entries 0/4/8/C always equal (background color) */
    uint8_t pal_hidden[3]; /* what $3F04/$3F08/$3F0C (and $3F14/18/1C) store */
    uint8_t oam[256];

    /* frame schedule */
    int scanline;
    int odd;               /* FCEUX "kook": every other frame is one dot shorter */
    int dead_frames;       /* frames after power-on with no vblank */
    uint64_t frame;

    /* lazy background line */
    uint8_t fb[(PPU_H + 2) * PPU_W];
    int line_active;
    int line_base;         /* index in fb of the current line */
    int line_pos;          /* next pixel to write */
    int first_tile;
    uint32_t pshift[2], at_latch;
    int fix_pending;       /* the line's vertical-scroll increment has not happened yet */
    int64_t line_start;    /* time of the line start, in 1/16 dots */

    /* sprites for the next line */
    PpuSprite line_sprites[8];
    int line_sprite_count;
    int sprite0_on_line;
    int hit_x;             /* 0x100 = no sprite-0 hit pending on this line */
    uint8_t hit_mask;

    struct Cpu *cpu;
} Ppu;

void ppu_power(Ppu *ppu, struct Cpu *cpu);
uint8_t ppu_read(Ppu *ppu, uint16_t reg);           /* reg = address & 7 */
void ppu_write(Ppu *ppu, uint16_t reg, uint8_t v);
void ppu_line_update(Ppu *ppu);                     /* draw up to now (called before mapper changes) */
void ppu_run_frame(Ppu *ppu);                       /* runs the CPU for one whole frame */

#endif
