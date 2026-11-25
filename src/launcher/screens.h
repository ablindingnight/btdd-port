/* The launcher's screens (immediate mode): each frame draws the current screen and handles this frame's input. */
#ifndef BTDD_LAUNCHER_SCREENS_H
#define BTDD_LAUNCHER_SCREENS_H
#include "app.h"

typedef struct UiIn {
    int nav_x, nav_y;   /* -1 / +1 from arrows, d-pad, stick */
    int accept, back;   /* Enter/Space/A, Esc/B */
    int page;           /* -1 / +1: LB/RB, PageUp/PageDown, Q/E */
    int play;           /* Start button / F5: play from anywhere */
    float mx, my;       /* mouse in 1280x720 space */
    int mouse_moved, click, wheel;
    SDL_Scancode key;   /* a key pressed this frame (for rebinding), 0 = none */
    char text[64];      /* text typed this frame */
    int backspace, paste, enter;
    int pad;            /* last input came from a gamepad (hint glyphs) */
} UiIn;

void screens_set_window(SDL_Window *w);
void screens_draw(App *a, UiIn *in);
Screen screen_from_name(const char *name); /* SCR_COUNT if unknown */
const char *screen_name(Screen s);
void screens_drop_file(App *a, const char *path);
void screens_poll(App *a); /* file dialog result */

#endif
