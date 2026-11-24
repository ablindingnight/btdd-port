#ifndef BTDD_PNG_H
#define BTDD_PNG_H
#include <stdint.h>

/* Writes an 8-bit RGB PNG from 0x??RRGGBB pixels. Returns 0 on success. */
int png_write_rgb(const char *path, const uint32_t *argb, int w, int h, int pitch);

#endif
