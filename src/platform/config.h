/* btdd_play settings: a "key = value" text file, btdd.cfg. Looked for next to the exe first, then in the
 * user's settings folder (%APPDATA%\btdd on Windows); written there with defaults if missing. */
#ifndef BTDD_CONFIG_H
#define BTDD_CONFIG_H
#include <SDL3/SDL.h>

enum { BTN_A, BTN_B, BTN_SELECT, BTN_START, BTN_UP, BTN_DOWN, BTN_LEFT, BTN_RIGHT, BTN_COUNT };

typedef struct Config {
    char rom[1024];
    int scale;            /* window size multiplier */
    int aspect;           /* 0 = square pixels, integer scaling; 1 = 8:7 pixel aspect (4:3 picture) */
    int fullscreen;
    int ppu_new;          /* 0 = FCEUX old-PPU timing (default), 1 = new-PPU timing */
    float volume;         /* 0..1 */
    int filter;           /* FILTER_* in video.h: nearest, sharp, bilinear */
    int scanlines;
    char palette[1024];   /* optional .pal file (64 or 512 RGB triplets); empty = generated palette */
    char renderer[64];    /* SDL render driver, "auto" = SDL's choice */
    SDL_Scancode keys[2][BTN_COUNT];
    char path[1024];      /* where the config was read from / written to */
} Config;

extern const char *const config_button_names[BTN_COUNT];

void config_defaults(Config *c);
/* Reads btdd.cfg (creating it with defaults when absent). Returns 0 on success, -1 if no file could be
 * read or written (defaults are still set). */
int config_load(Config *c);
int config_save(const Config *c); /* writes c->path; 0 on success */

#endif
