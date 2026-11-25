/* Pond to Pavement look (docs/LAUNCHER_STYLE.md): colour tokens and the drawn pieces every screen shares. */
#ifndef BTDD_LAUNCHER_THEME_H
#define BTDD_LAUNCHER_THEME_H
#include "kit.h"

#define C_GREEN      0x2f9a3a
#define C_GREEN_HI   0x3fb64a
#define C_GREEN_DEEP 0x17611f
#define C_HALFTONE   0x0b3d12
#define C_INK        0x0b0b14
#define C_PAPER      0xfff8e7
#define C_YELLOW     0xffd21f
#define C_RED        0xff4a1c
#define C_CYAN       0x5fd8ff
#define C_SPACE      0x24123f
#define C_GREY       0xc9c3b4
#define C_OK         0x22c46b
#define C_WARN       0xff8a1c
#define C_WHITE      0xffffff
#define C_INK_DIM    0x5d5a5a /* ink at ~63% on paper */

typedef enum Icon {
    ICON_HOME, ICON_DISPLAY, ICON_GRAPHICS, ICON_AUDIO, ICON_CONTROLS, ICON_GAMEPLAY, ICON_MODS, ICON_ONLINE,
    ICON_BUILDS, ICON_LOGS, ICON_ABOUT, ICON_CHECK, ICON_COUNT
} Icon;

int theme_init(void);  /* builds the backdrop texture; 0 on success */
void theme_quit(void);
/* the emblem artwork (res/art/emblem.png, RGBA, no title); 0 if loaded. Without it theme_emblem draws its own. */
int theme_load_logo(const char *path);
/* draws the emblem artwork fitted into the box (centred); returns 0 if there is no artwork */
int theme_logo_art(float x, float y, float w, float h);

void theme_backdrop(double t, int settings); /* field + halftone + turning rays */
void theme_titlebar(const char *crumb, const char *version);
void theme_box(float x, float y, float w, float h, Rgb fill, float border, float shadow); /* comic box */
void theme_sticker(float x, float y, const char *text, float deg);                        /* yellow caption */
void theme_badge(float x, float y, const char *text, Rgb fill, Rgb fg);                    /* returns nothing; ~20 px high */
float theme_badge_width(const char *text);
void theme_stamp(float rx, float cy, const char *text, Rgb c); /* right-aligned at rx, centred on cy */
void theme_emblem(float cx, float cy, float size);               /* the rocket fist on the space disc */
void theme_logo(float cx, float y, float s1, float s2, int tagline);
void theme_burst(float x, float y, float w, float h, const char *text, float text_size, int enabled, double t);
void theme_icon(Icon ic, float x, float y, float size, Rgb c);
void theme_key(float x, float y, const char *label, float h); /* keyboard / pad key chip, returns width via _w */
float theme_key_width(const char *label, float h);
void theme_button(float x, float y, float w, float h, const char *label, Rgb fill, Rgb fg, int focused, int disabled);
void theme_halftone_bar(float x, float y, float w, float h, float frac, double t, int stripes);

#endif
