/* Shared between ppu.c (old, scanline model + common registers) and ppu_new.c (dot model). */
#ifndef BTDD_PPU_INTERNAL_H
#define BTDD_PPU_INTERNAL_H
#include "ppu.h"

void ppun_power(Ppu *p);
void ppun_run_frame(Ppu *p);
uint8_t ppun_read_data(Ppu *p);
uint8_t ppun_read_oam(Ppu *p);
void ppun_write_data(Ppu *p, uint8_t v);
void ppun_write_oam(Ppu *p, uint8_t v);
/* the scroll-latch part of $2000/$2005/$2006 writes; second = the toggle was set (second write) */
void ppun_write_scroll(Ppu *p, int reg, uint8_t v, int second);

#endif
