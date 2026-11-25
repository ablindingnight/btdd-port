#include "screens.h"
#include "theme.h"
#include "mods/mods.h"
#include "platform/gpu.h"
#include "platform/video.h"
#include "platform/menu.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------------------------------------------------------------- immediate-mode focus */
typedef struct It {
    int idx, focused, hot, activate, dx;
} It;

static struct {
    App *a;
    UiIn *in;
    int scr, n;
    int n_prev[SCR_COUNT];
    SDL_Window *win;
} U;

static char pending_rom[1024];
static SDL_AtomicInt pending_flag;

void screens_set_window(SDL_Window *w) { U.win = w; }

static const char *const SCR_NAMES[SCR_COUNT] = {"home", "first-run", "display", "graphics", "audio", "controls",
                                                 "gameplay", "mods", "online", "builds", "logs", "about", "in-game"};
Screen screen_from_name(const char *n)
{
    int i;
    for (i = 0; i < SCR_COUNT; i++)
        if (!strcmp(n, SCR_NAMES[i]))
            return (Screen)i;
    return SCR_COUNT;
}
const char *screen_name(Screen s) { return s < SCR_COUNT ? SCR_NAMES[s] : "?"; }

static int inside(float x, float y, float w, float h)
{
    return U.in->mx >= x && U.in->mx < x + w && U.in->my >= y && U.in->my < y + h;
}

static It item(float x, float y, float w, float h)
{
    It it;
    int *f = &U.a->focus[U.scr];
    it.idx = U.n++;
    it.hot = inside(x, y, w, h);
    if (it.hot && U.in->mouse_moved && !U.a->capture && !U.a->edit)
        *f = it.idx;
    it.focused = *f == it.idx;
    it.activate = (it.focused && U.in->accept) || (it.hot && U.in->click);
    it.dx = it.focused ? U.in->nav_x : 0;
    if (it.activate) {
        U.in->accept = 0;
        U.in->click = 0;
        *f = it.idx;
    }
    return it;
}

static void go(Screen s)
{
    U.a->screen = s;
    U.a->capture = 0;
    U.a->edit = 0;
    SDL_StopTextInput(U.win);
}

static double T(void) { return U.a->now; }

/* ---------------------------------------------------------------- shared pieces */
static void hints(const char *a_word)
{
    float x = KIT_W - 40, y = KIT_H - 44;
    const char *k_a = U.in->pad ? "A" : "ENTER", *k_b = U.in->pad ? "B" : "ESC";
    const char *k_l = U.in->pad ? "LB" : "PGUP", *k_r = U.in->pad ? "RB" : "PGDN";
    float w;
    if (U.scr >= SCR_FIRST_SETTINGS) {
        w = kit_text_width(FONT_LABEL, 16, "page");
        x -= w;
        kit_text(FONT_LABEL, 16, x, y + 2, "page", C_PAPER, 0.85f, ALIGN_LEFT);
        x -= theme_key_width(k_r, 22) + 6;
        theme_key(x, y, k_r, 22);
        x -= theme_key_width(k_l, 22) + 4;
        theme_key(x, y, k_l, 22);
        x -= 18;
    }
    w = kit_text_width(FONT_LABEL, 16, "back");
    x -= w;
    kit_text(FONT_LABEL, 16, x, y + 2, "back", C_PAPER, 0.85f, ALIGN_LEFT);
    x -= theme_key_width(k_b, 22) + 6;
    theme_key(x, y, k_b, 22);
    x -= 18;
    w = kit_text_width(FONT_LABEL, 16, a_word);
    x -= w;
    kit_text(FONT_LABEL, 16, x, y + 2, a_word, C_PAPER, 0.85f, ALIGN_LEFT);
    x -= theme_key_width(k_a, 22) + 6;
    theme_key(x, y, k_a, 22);
}

static void toast(void)
{
    if (U.a->toast[0] && U.a->now < U.a->toast_until) {
        float w = kit_text_width(FONT_LABEL, 18, U.a->toast) + 32;
        theme_box((KIT_W - w) / 2, 60, w, 38, C_CYAN, 3, 5);
        kit_text(FONT_LABEL, 18, KIT_W / 2, 67, U.a->toast, C_INK, 1, ALIGN_CENTER);
    }
}

static void game_running_banner(void)
{
    if (U.a->game) {
        const char *t = "GAME RUNNING  ·  close the game window to come back";
        float w = kit_text_width(FONT_LABEL, 18, t) + 36;
        theme_box((KIT_W - w) / 2, KIT_H - 92, w, 36, C_YELLOW, 3, 5);
        kit_text(FONT_LABEL, 18, KIT_W / 2, KIT_H - 85, t, C_INK, 1, ALIGN_CENTER);
    }
}

static void open_rom_dialog(void);

/* ---------------------------------------------------------------- HOME */
static void home_row(float y, float deg, const char *label, const char *value, const char *badge, Rgb badge_c,
                     const char *small, const char *action, int *act)
{
    float x = 70, w = 620, h = 62, bx;
    It it;
    kit_push();
    kit_rotate_at(deg, x + w / 2, y + h / 2);
    theme_box(x, y, w, h, C_PAPER, 3, 7);
    kit_text(FONT_LABEL, 18, x + 18, y + 19, label, C_RED, 1, ALIGN_LEFT);
    bx = x + 172 + kit_text(FONT_LABEL, 24, x + 172, y + 5, value, C_INK, 1, ALIGN_LEFT);
    if (badge)
        theme_badge(bx + 8, y + 9, badge, badge_c, badge_c == C_RED ? C_PAPER : C_INK);
    {
        char s[120];
        size_t n;
        snprintf(s, sizeof s, "%s", small);
        n = strlen(s);
        while (n > 4 && kit_text_width(FONT_BODY, 16, s) > 350) { /* keep the end of long paths */
            memmove(s + 3, s + 6, strlen(s + 6) + 1);
            s[0] = s[1] = s[2] = '.';
            n = strlen(s);
        }
        kit_text(FONT_BODY, 16, x + 172, y + 34, s, C_INK_DIM, 1, ALIGN_LEFT);
    }
    it = item(x + w - 120, y, 120, h);
    {
        float aw = kit_text_width(FONT_DISPLAY, 20, action), ax = x + w - 18 - aw;
        if (it.focused)
            kit_rect(ax - 6, y + 14, aw + 12, 34, C_YELLOW, 1);
        kit_text(FONT_DISPLAY, 20, ax, y + 18, action, C_INK, 1, ALIGN_LEFT);
        kit_rect(ax, y + 44, aw, 3, C_RED, 1);
    }
    kit_pop();
    *act = it.activate;
}

static void draw_home(void)
{
    App *a = U.a;
    int act[3], i;
    static const char *const tabs[5] = {"SETTINGS", "CONTROLS", "ONLINE", "LOGS", "ABOUT"};
    static const Screen tab_to[5] = {SCR_DISPLAY, SCR_CONTROLS, SCR_ONLINE, SCR_LOGS, SCR_ABOUT};
    char build_small[160], game_small[1100];
    float x;
    const char *rom = a->cfg.rom;
    const char *base = strrchr(rom, '\\');
    if (!base || (strrchr(rom, '/') && strrchr(rom, '/') > base))
        base = strrchr(rom, '/');
    base = base ? base + 1 : rom;

    theme_backdrop(T(), 0);
    theme_titlebar(NULL, "launcher " LAUNCHER_VERSION);
    theme_sticker(70, 82, a->rom_ok ? (app_ready(a) ? "ROM VERIFIED · BUILD READY" : "ROM VERIFIED · FINISH SETUP")
                                    : "PICK YOUR ROM TO START",
                  -2);
    kit_push();
    kit_rotate_at(-2, 360, 210);
    kit_text_ink(FONT_DISPLAY, 66, 70, 120, "TWO BROTHERS,", C_PAPER, C_INK, 3, 6, 6, ALIGN_LEFT);
    kit_text_ink(FONT_DISPLAY, 66, 70, 183, "THREE TOADS...", C_PAPER, C_INK, 3, 6, 6, ALIGN_LEFT);
    kit_text_ink(FONT_DISPLAY, 66, 70, 246, "ZERO MERCY!", C_YELLOW, C_INK, 3, 6, 6, ALIGN_LEFT);
    kit_pop();

    snprintf(game_small, sizeof game_small, "%s · %s", rom[0] ? base : "no file yet", a->rom_msg);
    home_row(330, 0, "YOUR GAME", a->rom_ok ? "NES ROM, USA" : "No ROM yet", a->rom_ok ? "VERIFIED" : (rom[0] ? "CHECK" : NULL),
             a->rom_ok ? C_CYAN : C_RED, game_small, rom[0] ? "CHANGE" : "PICK", &act[0]);
    if (a->demo)
        snprintf(build_small, sizeof build_small, "reference engine · native cache ready · 24 Nov 2026");
    else {
        char c[64];
        app_cache_info(a, c, sizeof c);
        snprintf(build_small, sizeof build_small, "%s · native cache %s", app_ready(a) ? "self-check passed" : "setup not finished", c);
    }
    home_row(406, 0, "YOUR BUILD", "Pond to Pavement " LAUNCHER_VERSION, NULL, 0, build_small, app_ready(a) ? "REBUILD" : "SET UP", &act[1]);
    home_row(482, 0, "ONLINE", "2-player online", NULL, 0, "host a game, or paste a friend's join code", "HOST", &act[2]);
    if (act[0])
        open_rom_dialog();
    if (act[1]) {
        go(SCR_FIRST_RUN);
        if (app_ready(a))
            app_start_first_run(a, STEP_COMPILE);
        else
            app_start_first_run(a, STEP_VERIFY);
    }
    if (act[2])
        go(SCR_ONLINE);

    theme_emblem(1000, 206, 310);
    theme_logo(995, 368, 58, 92, 1);

    x = 70;
    for (i = 0; i < 5; i++) {
        float w = kit_text_width(FONT_LABEL, 19, tabs[i]) + 28;
        It it = item(x, 644, w, 38);
        theme_box(x, 644, w, 38, it.focused ? C_YELLOW : C_PAPER, 3, 3);
        kit_text(FONT_LABEL, 19, x + 14, 650, tabs[i], C_INK, 1, ALIGN_LEFT);
        if (it.activate)
            go(tab_to[i]);
        x += w + 12;
    }
    {
        It it = item(890, 572, 320, 134);
        int ready = app_ready(a);
        theme_burst(890, 572, 320, 134, "PLAY!", 64, ready, T());
        if (it.focused && ready)
            kit_frame(880, 562, 340, 154, 3, C_PAPER, 0.0f);
        if (it.activate) {
            if (ready)
                app_launch_game(a, "Play", NULL);
            else
                go(SCR_FIRST_RUN);
        }
    }
    {
        float hx = 640;
        hx += theme_key_width(U.in->pad ? "A" : "ENTER", 22) + 6;
        theme_key(640, 656, U.in->pad ? "A" : "ENTER", 22);
        kit_text(FONT_LABEL, 16, hx, 658, U.a->focus[SCR_HOME] == 8 ? "play" : "select", C_PAPER, 0.85f, ALIGN_LEFT);
    }
    game_running_banner();
}

/* ---------------------------------------------------------------- FIRST RUN */
static void draw_first_run(void)
{
    App *a = U.a;
    static const char *const titles[STEP_COUNT] = {"Pick your ROM", "Verify it", "Compile the native build",
                                                   "Self-check", "Enhance my game", "Ready to play"};
    Step st[STEP_COUNT];
    int i, cur = 0;
    float y;
    char cap[48];
    for (i = 0; i < STEP_COUNT; i++)
        app_step_get(a, i, &st[i]);
    if (!a->cfg.rom[0])
        st[STEP_PICK].state = STEP_WAIT;
    else if (st[STEP_PICK].state == STEP_WAIT)
        st[STEP_PICK].state = STEP_DONE;
    for (i = 0; i < STEP_COUNT; i++)
        if (st[i].state == STEP_RUN || st[i].state == STEP_WAIT || st[i].state == STEP_FAIL) {
            cur = i;
            break;
        } else
            cur = STEP_COUNT - 1;

    theme_backdrop(T(), 0);
    theme_titlebar("FIRST RUN", "launcher " LAUNCHER_VERSION);
    snprintf(cap, sizeof cap, "STEP %d OF %d", cur + 1, STEP_COUNT);
    theme_sticker(70, 72, cap, -2);
    kit_push();
    kit_rotate_at(-2, 300, 170);
    kit_text_ink(FONT_DISPLAY, 80, 66, 108, "LET'S BUILD", C_PAPER, C_INK, 3, 6, 6, ALIGN_LEFT);
    kit_text_ink(FONT_DISPLAY, 80, 66, 184, "YOUR GAME.", C_YELLOW, C_INK, 3, 6, 6, ALIGN_LEFT);
    kit_pop();

    theme_box(66, 268, 700, 400, C_PAPER, 3, 7);
    y = 282;
    for (i = 0; i < STEP_COUNT; i++) {
        float rowh = 64, alpha = st[i].state == STEP_WAIT ? 0.5f : 1;
        char num[4];
        const char *note = st[i].note[0] ? st[i].note : "";
        static const char *const def_notes[STEP_COUNT] = {
            "Your .nes (or a .zip that holds it). Drag it here or click the box on the right",
            "SHA-1 must match the USA cartridge",
            "Turns your ROM's code into a PC program, about a minute",
            "Boot to the title; native and reference frames must match",
            "Upscaled tiles or a sprite pack. Optional",
            "",
        };
        if (!note[0])
            note = i == STEP_PICK && a->cfg.rom[0] ? a->cfg.rom : def_notes[i];
        kit_push();
        kit_alpha(alpha);
        snprintf(num, sizeof num, "%d", i + 1);
        kit_push();
        kit_rotate_at(-4, 104, y + rowh / 2);
        theme_box(84, y + 12, 40, 40,
                  st[i].state == STEP_DONE ? C_CYAN : st[i].state == STEP_RUN ? C_YELLOW : st[i].state == STEP_FAIL ? C_RED : C_WHITE, 3, 0);
        kit_text(FONT_DISPLAY, 24, 104, y + 18, num, C_INK, 1, ALIGN_CENTER);
        kit_pop();
        kit_text(FONT_LABEL, 20, 146, y + 10, titles[i], C_INK, 1, ALIGN_LEFT);
        if (i == STEP_ENHANCE)
            theme_badge(146 + kit_text_width(FONT_LABEL, 20, titles[i]) + 10, y + 12, "OPTIONAL", C_GREY, C_INK);
        {
            char s[200];
            snprintf(s, sizeof s, "%s", note);
            while (strlen(s) > 8 && kit_text_width(FONT_BODY, 15, s) > 440) {
                memmove(s + 3, s + 6, strlen(s + 6) + 1);
                s[0] = s[1] = s[2] = '.';
            }
            kit_text(FONT_BODY, 15, 146, y + 34, s, C_INK_DIM, 1, ALIGN_LEFT);
        }
        if (st[i].state == STEP_RUN)
            theme_halftone_bar(146, y + 52, 420, 12, st[i].progress < 0 ? 1.0f : st[i].progress, T(), 1);
        switch (st[i].state) {
        case STEP_DONE: theme_stamp(744, y + rowh / 2, i == STEP_VERIFY ? "VERIFIED" : i == STEP_READY ? "GO!" : "DONE", 0x0e8a4a); break;
        case STEP_RUN: theme_stamp(744, y + rowh / 2, "WORKING", C_RED); break;
        case STEP_SKIP: theme_stamp(744, y + rowh / 2, "SKIPPED", 0x7a7468); break;
        case STEP_FAIL: theme_stamp(744, y + rowh / 2, "PROBLEM", C_RED); break;
        default: theme_stamp(744, y + rowh / 2, i == cur ? "NEXT" : "SOON", 0x9a9488); break;
        }
        kit_pop();
        if (i < STEP_COUNT - 1)
            kit_rect(84, y + rowh, 664, 2, C_INK, 0.15f);
        y += rowh;
    }

    {
        It it = item(830, 96, 380, 290);
        float dash;
        kit_rect(830, 96, 380, 290, C_INK, it.focused ? 0.35f : 0.22f);
        for (dash = 0; dash < 380; dash += 22) {
            kit_rect(830 + dash, 96, 12, 4, C_PAPER, 1);
            kit_rect(830 + dash, 382, 12, 4, C_PAPER, 1);
        }
        for (dash = 0; dash < 290; dash += 22) {
            kit_rect(830, 96 + dash, 4, 12, C_PAPER, 1);
            kit_rect(1206, 96 + dash, 4, 12, C_PAPER, 1);
        }
        kit_text_ink(FONT_DISPLAY, 34, 1020, 196, a->rom_ok ? "ROM ACCEPTED" : a->cfg.rom[0] ? "TRY ANOTHER ROM" : "DROP YOUR ROM HERE",
                     C_YELLOW, C_INK, 2, 0, 0, ALIGN_CENTER);
        kit_text_wrap(FONT_BODY, 16, 856, 248, 330, 20,
                      a->rom_ok ? "Drop a different .nes or .zip here, or click, to start over. Your file stays where it is; we only read it."
                      : a->cfg.rom[0] ? a->rom_msg
                                      : "Or click to pick it. You need your own backup of the USA cartridge; Pond to Pavement does not include the game.",
                      C_PAPER, 0.9f);
        if (it.activate)
            open_rom_dialog();
    }
    theme_logo(1010, 418, 44, 70, 1);
    {
        It it = item(900, 590, 280, 110);
        theme_burst(900, 590, 280, 110, "PLAY!", 52, app_ready(a), T());
        if (it.activate && app_ready(a)) {
            go(SCR_HOME);
            app_launch_game(a, "Play", NULL);
        }
    }
    if (U.in->back && app_ready(a))
        go(SCR_HOME);
    {
        float x = 70;
        theme_key(x, 684, U.in->pad ? "B" : "ESC", 22);
        x += theme_key_width(U.in->pad ? "B" : "ESC", 22) + 6;
        kit_text(FONT_LABEL, 16, x, 686, app_ready(a) ? "home      the log of this run is in Logs" : "the log of this run is in Logs", C_PAPER, 0.85f, ALIGN_LEFT);
    }
}

/* ---------------------------------------------------------------- SETTINGS shell + rows */
static const char *const PAGE_TITLE[SCR_COUNT] = {"", "", "DISPLAY", "GRAPHICS", "AUDIO", "CONTROLS", "GAMEPLAY", "MODS",
                                                  "ONLINE PLAY", "BUILDS", "LOGS", "ABOUT", ""};
static const Icon PAGE_ICON[SCR_COUNT] = {ICON_HOME, ICON_HOME, ICON_DISPLAY, ICON_GRAPHICS, ICON_AUDIO, ICON_CONTROLS,
                                          ICON_GAMEPLAY, ICON_MODS, ICON_ONLINE, ICON_BUILDS, ICON_LOGS, ICON_ABOUT, ICON_HOME};

#define PX 330.0f   /* panel content left */
#define PR 1216.0f  /* panel content right */
static float RY;

static void settings_shell(void)
{
    int i;
    theme_backdrop(T(), 1);
    theme_titlebar("SETTINGS", "launcher " LAUNCHER_VERSION);
    for (i = SCR_FIRST_RUN; i <= SCR_LAST_SETTINGS; i++) {
        int s = i == SCR_FIRST_RUN ? SCR_HOME : i; /* first entry is HOME */
        float y = 76 + (float)(i - SCR_FIRST_RUN) * 47 + (i >= SCR_BUILDS ? 8 : 0), x = 34;
        int on = s == U.scr;
        const char *t = s == SCR_HOME ? "HOME" : PAGE_TITLE[s];
        int hot = inside(x, y, 236, 40);
        kit_push();
        if (on)
            kit_translate(10, 0);
        theme_box(x, y, 236, 40, on ? C_YELLOW : hot ? 0x1f7a29 : C_GREEN_DEEP, 3, on ? 5.0f : 3.0f);
        theme_icon(PAGE_ICON[s], x + 14, y + 10, 20, on ? C_INK : C_PAPER);
        kit_text(FONT_LABEL, 18, x + 46, y + 9, t, on ? C_INK : C_PAPER, 1, ALIGN_LEFT);
        kit_pop();
        if (hot && U.in->click) {
            U.in->click = 0;
            go((Screen)s);
        }
    }
    theme_box(300, 70, 946, 590, C_PAPER, 3, 7);
    kit_push();
    kit_rotate_at(-2, 300, 80);
    {
        float w = kit_text_width(FONT_DISPLAY, 44, PAGE_TITLE[U.scr]) + 34;
        theme_box(290, 54, w, 54, C_RED, 3, 0);
        kit_text_ink(FONT_DISPLAY, 44, 307, 56, PAGE_TITLE[U.scr], C_YELLOW, C_INK, 2.5f, 4, 4, ALIGN_LEFT);
    }
    kit_pop();
    RY = 128;
    /* page switching */
    if (U.in->page && !U.a->capture && !U.a->edit) {
        int s = U.scr + U.in->page;
        if (s < SCR_FIRST_SETTINGS)
            s = SCR_LAST_SETTINGS;
        if (s > SCR_LAST_SETTINGS)
            s = SCR_FIRST_SETTINGS;
        go((Screen)s);
    }
    if (U.in->back && !U.a->capture && !U.a->edit)
        go(SCR_HOME);
}

static void group(const char *t)
{
    kit_text(FONT_DISPLAY, 20, PX + 6, RY + 4, t, C_RED, 1, ALIGN_LEFT);
    RY += 30;
}

/* row frame; returns the item; draws label + hint */
static It row_begin(const char *label, const char *hint, int disabled)
{
    float h = hint && hint[0] ? 50.0f : 40.0f;
    It it = item(PX, RY, PR - PX, h);
    if (disabled) {
        it.activate = 0;
        it.dx = 0;
    }
    if (it.focused) {
        float p[6];
        kit_rect(PX, RY, PR - PX, h, C_YELLOW, 0.35f);
        kit_frame(PX, RY, PR - PX, h, 3, C_INK, 1);
        p[0] = PX - 24, p[1] = RY + h / 2 - 10, p[2] = PX - 8, p[3] = RY + h / 2, p[4] = PX - 24, p[5] = RY + h / 2 + 10;
        kit_poly(p, 3, C_RED, 1);
    }
    kit_push();
    if (disabled)
        kit_alpha(0.45f);
    kit_text(FONT_LABEL, 21, PX + 10, RY + 4, label, C_INK, 1, ALIGN_LEFT);
    if (hint && hint[0])
        kit_text(FONT_BODY, 15, PX + 10, RY + 28, hint, C_INK_DIM, 1, ALIGN_LEFT);
    kit_pop();
    return it;
}
static void row_end(const char *hint)
{
    float h = hint && hint[0] ? 50.0f : 40.0f;
    RY += h;
    kit_rect(PX, RY, PR - PX, 2, C_INK, 0.12f);
    RY += 2;
}

/* value control: returns -1 / +1 */
static float choice_w = 230;
static int ctl_choice(It it, float cy, const char *value, int disabled)
{
    float w = choice_w, x = PR - 10 - w, y = cy - 16;
    int d = it.dx;
    kit_push();
    if (disabled)
        kit_alpha(0.45f);
    theme_box(x, y, w, 32, C_WHITE, 3, 0);
    kit_text(FONT_LABEL, 20, x + 12, y + 2, "‹", C_RED, 1, ALIGN_LEFT);
    kit_text(FONT_LABEL, 20, x + w - 12, y + 2, "›", C_RED, 1, ALIGN_RIGHT);
    kit_text(FONT_LABEL, 18, x + w / 2, y + 5, value, C_INK, 1, ALIGN_CENTER);
    kit_pop();
    if (disabled)
        return 0;
    if (U.in->click && inside(x, y, w, 32)) {
        d = U.in->mx < x + w / 2 ? -1 : 1;
        U.in->click = 0;
    } else if (it.activate)
        d = 1;
    return d;
}
static int ctl_toggle(It it, float cy, int on, int disabled)
{
    float x = PR - 10 - 112, y = cy - 15;
    kit_push();
    if (disabled)
        kit_alpha(0.45f);
    kit_rect(x, y, 112, 30, C_INK, 1);
    kit_rect(x + 3, y + 3, 52, 24, on ? C_YELLOW : C_WHITE, 1);
    kit_rect(x + 57, y + 3, 52, 24, on ? C_WHITE : C_INK, 1);
    kit_text(FONT_LABEL, 16, x + 29, y + 5, "ON", on ? C_INK : 0xb8b4aa, 1, ALIGN_CENTER);
    kit_text(FONT_LABEL, 16, x + 83, y + 5, "OFF", on ? 0xb8b4aa : C_PAPER, 1, ALIGN_CENTER);
    kit_pop();
    if (disabled)
        return 0;
    return it.activate || (it.dx < 0 && !on) || (it.dx > 0 && on);
}

static int row_choice(const char *label, const char *hint, const char *value, int disabled)
{
    It it = row_begin(label, hint, disabled);
    int d = ctl_choice(it, RY + (hint && hint[0] ? 25 : 20), value, disabled);
    row_end(hint);
    return d;
}
static int row_toggle(const char *label, const char *hint, int on, int disabled)
{
    It it = row_begin(label, hint, disabled);
    int r = ctl_toggle(it, RY + (hint && hint[0] ? 25 : 20), on, disabled);
    row_end(hint);
    return r;
}
static int row_button(const char *label, const char *hint, const char *btn, Rgb fill)
{
    It it = row_begin(label, hint, 0);
    float h = hint && hint[0] ? 50.0f : 40.0f, w = kit_text_width(FONT_DISPLAY, 20, btn) + 30;
    theme_button(PR - 10 - w, RY + h / 2 - 16, w, 32, btn, fill, C_INK, 0, 0);
    row_end(hint);
    return it.activate;
}
static void row_info(const char *label, const char *hint, const char *value, KitFont vf)
{
    float h = hint && hint[0] ? 50.0f : 40.0f;
    kit_text(FONT_LABEL, 21, PX + 10, RY + 4, label, C_INK, 1, ALIGN_LEFT);
    if (hint && hint[0])
        kit_text(FONT_BODY, 15, PX + 10, RY + 28, hint, C_INK_DIM, 1, ALIGN_LEFT);
    kit_text(vf, vf == FONT_MONO ? 14.0f : 17.0f, PR - 10, RY + h / 2 - 10, value, C_INK, 1, ALIGN_RIGHT);
    row_end(hint);
}

static int mod_toggle_row(const char *id, const char *label, const char *hint)
{
    int i = mods_find(id);
    if (i < 0)
        return 0;
    if (row_toggle(label, hint, mods_enabled(i), 0)) {
        mods_enable(i, !mods_enabled(i));
        return 1;
    }
    return 0;
}

/* ---------------------------------------------------------------- DISPLAY */
static void draw_display(void)
{
    App *a = U.a;
    Config *c = &a->cfg;
    char v[64];
    int d, ch = 0;
    snprintf(v, sizeof v, "%dx · %d × %d", c->scale, 256 * c->scale, 240 * c->scale);
    if ((d = row_choice("Window size", "in NES pixels; F11 or Alt+Enter switches to fullscreen in game", v, 0))) {
        c->scale = SDL_clamp(c->scale + d, 1, 10);
        ch = 1;
    }
    if (row_toggle("Fullscreen", "start the game fullscreen", c->fullscreen, 0))
        c->fullscreen = !c->fullscreen, ch = 1;
    if ((d = row_choice("Aspect", "square pixels, or the 4:3 picture a TV showed", c->aspect ? "4:3 TV" : "Square pixels", 0)))
        c->aspect = !c->aspect, ch = 1;
    RY += 8;
    group("WIDESCREEN");
    ch |= mod_toggle_row("widescreen", "Widescreen 16:9", "draws the level past the old screen edges (F10 in game)");
    ch |= mod_toggle_row("widesprites", "Sprites in the margins", "enemies and players stay visible in the wide area");
    ch |= mod_toggle_row("widehud", "Status bar at the edges", "each player's half moves outward");
    ch |= mod_toggle_row("widefade", "Fade outer edges", "hides enemies popping in at the far edges");
    if (ch)
        app_save_settings(a);
}

/* ---------------------------------------------------------------- GRAPHICS */
static SDL_Texture *swatch_tex[2];
static void preset_swatch(int preset, float x, float y, float w, float h)
{
    static const Rgb pat[4] = {C_RED, C_CYAN, C_YELLOW, 0x16206b};
    int linear = preset == GPU_PRESET_BILINEAR || preset == GPU_PRESET_SMOOTH || preset == GPU_PRESET_SMOOTH_CRT;
    int crt = preset == GPU_PRESET_CRT || preset == GPU_PRESET_SMOOTH_CRT;
    float yy;
    if (!swatch_tex[0]) {
        int k;
        for (k = 0; k < 2; k++) {
            SDL_Surface *s = SDL_CreateSurface(12, 6, SDL_PIXELFORMAT_XRGB8888);
            int px, py;
            if (!s)
                return;
            for (py = 0; py < 6; py++)
                for (px = 0; px < 12; px++) {
                    int q = ((px / 2 + py / 2) & 1) * 2 + ((px + py) & 1);
                    ((uint32_t *)((uint8_t *)s->pixels + py * s->pitch))[px] = pat[q];
                }
            swatch_tex[k] = SDL_CreateTextureFromSurface(kit_renderer(), s);
            SDL_DestroySurface(s);
            if (swatch_tex[k])
                SDL_SetTextureScaleMode(swatch_tex[k], k ? SDL_SCALEMODE_LINEAR : SDL_SCALEMODE_NEAREST);
        }
    }
    kit_texture(swatch_tex[linear], x, y, w, h, 1);
    if (preset == GPU_PRESET_SHARP)
        kit_frame(x, y, w, h, 1, C_INK, 0.2f);
    if (crt)
        for (yy = y; yy < y + h; yy += 4)
            kit_rect(x, yy + 2, w, 2, C_INK, 0.35f);
}

static void draw_graphics(void)
{
    App *a = U.a;
    Config *c = &a->cfg;
    int d, i, ch = 0;
    static const char *const pnames[GPU_PRESET_COUNT] = {"NEAREST", "SHARP", "BILINEAR", "CRT", "SMOOTH", "SMOOTH CRT"};
    if ((d = row_choice("Renderer", c->gpu ? "GPU runs the shader presets (Direct3D 12)" : "SDL renderer: filters and scanlines below",
                        c->gpu ? "GPU" : "SDL", 0)))
        c->gpu = !c->gpu, ch = 1;
    if (c->gpu) {
        It it;
        float x = PX, w = (PR - PX - 50) / 6;
        group("SHADER PRESET");
        it = item(PX, RY, PR - PX, 108);
        if (it.focused)
            kit_frame(PX - 6, RY - 6, PR - PX + 12, 120, 3, C_INK, 1);
        for (i = 0; i < GPU_PRESET_COUNT; i++) {
            int on = c->shader == i;
            float lift = on ? -3.0f : 0.0f;
            theme_box(x, RY + lift, w, 100, on ? C_YELLOW : C_WHITE, 3, on ? 4.0f : 0.0f);
            kit_frame(x + 6, RY + lift + 6, w - 12, 62, 2, C_INK, 1);
            preset_swatch(i, x + 8, RY + lift + 8, w - 16, 58);
            kit_text(FONT_LABEL, 14, x + w / 2, RY + lift + 74, pnames[i], C_INK, 1, ALIGN_CENTER);
            if (U.in->click && inside(x, RY, w, 100)) {
                c->shader = i;
                ch = 1;
                U.in->click = 0;
            }
            x += w + 10;
        }
        if (it.dx) {
            c->shader = (c->shader + it.dx + GPU_PRESET_COUNT) % GPU_PRESET_COUNT;
            ch = 1;
        }
        RY += 116;
    } else {
        if ((d = row_choice("Filter", "F3 in game", video_filter_names[c->filter], 0))) {
            c->filter = (c->filter + d + 3) % 3;
            ch = 1;
        }
        if (row_toggle("Scanlines", "F4 in game", c->scanlines, 0))
            c->scanlines = !c->scanlines, ch = 1;
    }
    ch |= mod_toggle_row("nolimit", "No sprite flicker", "draw every sprite on every line (F8); picture only");
    row_choice("Sprite pack", "your own art for the game's sprites; coming with HD packs (M10)", "Off", 1);
    if (ch)
        app_save_settings(a);
}

/* ---------------------------------------------------------------- AUDIO */
static void draw_audio(void)
{
    App *a = U.a;
    It it = row_begin("Volume", "left / right in steps of 5%", 0);
    float x = PR - 10 - 300, y = RY + 14, w = 230;
    char v[16];
    int pct = (int)(a->cfg.volume * 100 + 0.5f);
    theme_halftone_bar(x, y, w, 22, a->cfg.volume, T(), 0);
    snprintf(v, sizeof v, "%d%%", pct);
    kit_text(FONT_LABEL, 18, PR - 10, y, v, C_INK, 1, ALIGN_RIGHT);
    if (it.dx || (U.in->click && inside(x, y - 6, w, 34))) {
        if (it.dx)
            pct = SDL_clamp(pct + 5 * it.dx, 0, 100);
        else {
            pct = (int)((U.in->mx - x) / w * 100 + 0.5f);
            pct = SDL_clamp(pct, 0, 100);
            U.in->click = 0;
        }
        a->cfg.volume = (float)pct / 100.0f;
        app_save_settings(a);
    }
    row_end("x");
    row_choice("Per-channel mute", "pulse, triangle, noise and samples one by one: coming", "All on", 1);
}

/* ---------------------------------------------------------------- CONTROLS */
static void draw_controls(void)
{
    App *a = U.a;
    static const char *const names[BTN_COUNT] = {"A", "B", "SELECT", "START", "UP", "DOWN", "LEFT", "RIGHT"};
    static const char *const pad[BTN_COUNT] = {"East", "South", "Back", "Start", "D-pad up", "D-pad down", "D-pad left", "D-pad right"};
    int p, b;
    for (p = 0; p < 2; p++) {
        float x = PX + (float)p * 446, y = RY, w = 430;
        theme_box(x, y, w, 420, C_WHITE, 3, 0);
        kit_text(FONT_DISPLAY, 28, x + 14, y + 6, p ? "PLAYER 2" : "PLAYER 1", C_INK, 1, ALIGN_LEFT);
        kit_text(FONT_LABEL, 14, x + w - 14, y + 16, p ? "keyboard · pad 2" : "keyboard · pad 1", C_INK_DIM, 1, ALIGN_RIGHT);
        kit_text(FONT_LABEL, 13, x + 130, y + 46, "KEY", C_RED, 1, ALIGN_LEFT);
        kit_text(FONT_LABEL, 13, x + 290, y + 46, "GAMEPAD", C_RED, 1, ALIGN_LEFT);
        for (b = 0; b < BTN_COUNT; b++) {
            float ry = y + 66 + (float)b * 43;
            int id = p * 16 + b + 1;
            It it = item(x + 6, ry, w - 12, 40);
            const char *kn = SDL_GetScancodeName(a->cfg.keys[p][b]);
            if (it.focused)
                kit_rect(x + 6, ry, w - 12, 40, C_YELLOW, 0.4f);
            kit_text(FONT_LABEL, 18, x + 16, ry + 9, names[b], C_INK, 1, ALIGN_LEFT);
            if (a->capture == id) {
                kit_rect(x + 128, ry + 6, 140, 28, ((int)(T() * 2.5) & 1) ? C_YELLOW : C_WHITE, 1);
                kit_frame(x + 128, ry + 6, 140, 28, 2, C_INK, 1);
                kit_text(FONT_LABEL, 16, x + 198, ry + 11, "PRESS A KEY", C_INK, 1, ALIGN_CENTER);
            } else
                theme_key(x + 128, ry + 6, kn && kn[0] ? kn : "none", 28);
            kit_text(FONT_LABEL, 16, x + 290, ry + 11, pad[b], C_INK_DIM, 1, ALIGN_LEFT);
            if (b < BTN_COUNT - 1)
                kit_rect(x + 10, ry + 41, w - 20, 1, C_INK, 0.15f);
            if (it.activate && !a->capture)
                a->capture = id;
        }
    }
    RY += 432;
    {
        It it = item(PX, RY, 240, 38);
        theme_button(PX, RY, 240, 38, "RESET TO DEFAULTS", C_PAPER, C_INK, it.focused, 0);
        if (it.activate) {
            Config d;
            config_defaults(&d);
            memcpy(a->cfg.keys, d.keys, sizeof d.keys);
            app_save_settings(a);
            app_toast(a, "Keys reset");
        }
        kit_text_wrap(FONT_BODY, 15, PX + 262, RY + 2, 600, 18,
                      "Enter on a key, then press the new key (Esc keeps the old one). In game: F6/F7 save/load, "
                      "Backspace rewind, Tab fast forward, F10 widescreen.", C_INK_DIM, 1);
    }
    /* key capture */
    if (a->capture && U.in->key) {
        int p2 = (a->capture - 1) / 16, b2 = (a->capture - 1) % 16;
        if (U.in->key != SDL_SCANCODE_ESCAPE) {
            a->cfg.keys[p2][b2] = U.in->key;
            app_save_settings(a);
        }
        a->capture = 0;
        U.in->back = 0;
    }
}

/* ---------------------------------------------------------------- GAMEPLAY */
static void draw_gameplay(void)
{
    App *a = U.a;
    Config *c = &a->cfg;
    int i, d, ch = 0;
    float x = PX, w = (PR - PX - 9 * 8) / 10;
    char v[32];
    group("SAVE STATES  ·  F6 saves, F7 loads, 0-9 picks a slot in game");
    for (i = 0; i < 10; i++) {
        char when[32] = "", n[4];
        int used = app_slot_info(a, i, when, sizeof when);
        theme_box(x, RY, w, 58, used ? C_CYAN : C_WHITE, 3, 0);
        snprintf(n, sizeof n, "%d", i);
        kit_text(FONT_DISPLAY, 24, x + w / 2, RY + 6, n, C_INK, 1, ALIGN_CENTER);
        if (used)
            kit_text(FONT_LABEL, 12, x + w / 2, RY + 36, when, C_INK, 1, ALIGN_CENTER);
        x += w + 8;
    }
    RY += 70;
    snprintf(v, sizeof v, c->rewind_seconds ? "%d s" : "off", c->rewind_seconds);
    if ((d = row_choice("Rewind length", "hold Backspace in game; about 1 MB of memory per second", v, 0))) {
        static const int steps[] = {0, 10, 30, 60, 120, 300, 600};
        int k = 0;
        while (k < 6 && steps[k] < c->rewind_seconds)
            k++;
        k = SDL_clamp(k + d, 0, 6);
        c->rewind_seconds = steps[k];
        ch = 1;
    }
    snprintf(v, sizeof v, "%dx", c->ff_speed);
    if ((d = row_choice("Fast forward", "speed while Tab is held", v, 0)))
        c->ff_speed = SDL_clamp(c->ff_speed * (d > 0 ? 2 : 1) / (d < 0 ? 2 : 1), 2, 16), ch = 1;
    {
        int li = mods_find("lives");
        if (li >= 0) {
            if (mods_enabled(li))
                snprintf(v, sizeof v, "%d", mods_value(li));
            else
                snprintf(v, sizeof v, "3 (normal)");
            if ((d = row_choice("Starting lives", "start and continue with this many (F9 in game)", v, 0))) {
                int val = mods_enabled(li) ? mods_value(li) : 3;
                val += d;
                if (val == 3 || val < 1 || val > 9)
                    mods_enable(li, 0);
                else {
                    mods_enable(li, 1);
                    mods_set_value(li, val);
                }
                if (val < 1)
                    mods_enable(li, 1), mods_set_value(li, 9);
                ch = 1;
            }
        }
    }
    RY += 6;
    group("CHEATS · THE GAME'S OWN CODES, ENTERED FOR YOU");
    ch |= mod_toggle_row("cheat_lives5", "Held code: 5 lives", "Up + A + B when you press Start, for every player");
    ch |= mod_toggle_row("cheat_code1", "Title code 1: 5 lives", "players joining later get 5 lives");
    ch |= mod_toggle_row("cheat_code2", "Title code 2: 10 lives", "players joining later get 10 lives");
    if (ch)
        app_save_settings(a);
}

/* ---------------------------------------------------------------- MODS */
static void draw_mods(void)
{
    App *a = U.a;
    int i, k = 0, ch = 0;
    for (i = 0; i < mods_count(); i++) {
        const ModDef *m = mods_def(i);
        float cw = (PR - PX - 16) / 2, x = PX + (float)(k % 2) * (cw + 16), y = RY + (float)(k / 2) * 112;
        It it;
        int on = mods_enabled(i);
        if (!strncmp(m->id, "cheat_", 6))
            continue;
        it = item(x, y, cw, 100);
        if (on)
            kit_rect(x + 5, y + 5, cw, 100, C_RED, 1);
        theme_box(x, y, cw, 100, it.focused ? 0xfff0b0 : C_WHITE, 3, 0);
        if (it.focused)
            kit_frame(x - 4, y - 4, cw + 8, 108, 3, C_INK, 1);
        kit_text(FONT_LABEL, 20, x + 14, y + 8, m->name, C_INK, 1, ALIGN_LEFT);
        kit_text_wrap(FONT_BODY, 16, x + 14, y + 36, cw - 150, 17, m->help, C_INK_DIM, 1);
        theme_badge(x + 14, y + 72, m->kind == MOD_DISPLAY ? "PICTURE" : "GAMEPLAY", m->kind == MOD_DISPLAY ? C_CYAN : C_RED,
                    m->kind == MOD_DISPLAY ? C_INK : C_PAPER);
        {
            float tx = x + cw - 126, ty = y + 12;
            char v[16];
            kit_rect(tx, ty, 112, 30, C_INK, 1);
            kit_rect(tx + 3, ty + 3, 52, 24, on ? C_YELLOW : C_WHITE, 1);
            kit_rect(tx + 57, ty + 3, 52, 24, on ? C_WHITE : C_INK, 1);
            kit_text(FONT_LABEL, 16, tx + 29, ty + 5, "ON", on ? C_INK : 0xb8b4aa, 1, ALIGN_CENTER);
            kit_text(FONT_LABEL, 16, tx + 83, ty + 5, "OFF", on ? 0xb8b4aa : C_PAPER, 1, ALIGN_CENTER);
            if (m->value_min != m->value_max && on) {
                snprintf(v, sizeof v, "‹ %d ›", mods_value(i));
                kit_text(FONT_LABEL, 18, tx + 56, ty + 40, v, C_INK, 1, ALIGN_CENTER);
            }
        }
        if (it.activate) {
            mods_enable(i, !on);
            ch = 1;
        } else if (it.dx && m->value_min != m->value_max && on) {
            mods_set_value(i, mods_value(i) + it.dx);
            ch = 1;
        }
        k++;
    }
    RY += (float)((k + 1) / 2) * 112 + 6;
    kit_text_wrap(FONT_BODY, 16, PX + 4, RY, PR - PX - 8, 20,
                  "Online play needs the same gameplay mods on both PCs; picture mods may differ. Cheats are on the "
                  "Gameplay page. Drop-in mod folders come later.", C_INK_DIM, 1);
    if (ch)
        app_save_settings(a);
}

/* ---------------------------------------------------------------- ONLINE */
enum { EDIT_CODE = 1, EDIT_ADDR = 2 };

static void text_field(int id, float x, float y, float w, char *buf, size_t len, const char *placeholder)
{
    App *a = U.a;
    It it = item(x, y, w, 44);
    int editing = a->edit == id;
    theme_box(x, y, w, 44, editing ? C_WHITE : C_INK, 3, 0);
    if (it.focused && !editing)
        kit_frame(x - 4, y - 4, w + 8, 52, 3, C_RED, 1);
    if (buf[0] || editing) {
        char shown[200];
        snprintf(shown, sizeof shown, "%s%s", buf, editing && ((int)(T() * 2) & 1) ? "_" : "");
        kit_text(FONT_MONO, 20, x + 14, y + 10, shown, editing ? C_INK : C_YELLOW, 1, ALIGN_LEFT);
    } else
        kit_text(FONT_LABEL, 18, x + 14, y + 11, placeholder, 0x8a8a92, 1, ALIGN_LEFT);
    if (it.activate && !editing) {
        a->edit = id;
        SDL_StartTextInput(U.win);
    } else if (editing) {
        size_t n = strlen(buf);
        if (U.in->text[0] && n + strlen(U.in->text) < len - 1)
            strcat(buf, U.in->text);
        if (U.in->backspace && n)
            buf[n - 1] = 0;
        if (U.in->paste) {
            char *clip = SDL_GetClipboardText();
            if (clip) {
                char *s = clip;
                while (*s == ' ' || *s == '\n' || *s == '\r')
                    s++;
                snprintf(buf, len, "%s", s);
                buf[strcspn(buf, " \r\n")] = 0;
                SDL_free(clip);
            }
        }
        if (U.in->enter || U.in->back) {
            a->edit = 0;
            SDL_StopTextInput(U.win);
            U.in->back = 0;
            U.in->accept = 0;
        }
    }
}

static void draw_online(void)
{
    App *a = U.a;
    Config *c = &a->cfg;
    float lx = PX, rx = PX + 470, y0 = RY;
    char port[16], v[32];
    int d;
    It it;
    /* left: host / join */
    kit_text(FONT_DISPLAY, 20, lx + 2, y0, "HOST", C_RED, 1, ALIGN_LEFT);
    snprintf(port, sizeof port, "%d", c->net_port);
    it = item(lx, y0 + 30, 200, 40);
    theme_button(lx, y0 + 30, 200, 40, "HOST GAME", C_YELLOW, C_INK, it.focused, a->game != NULL);
    if (it.activate && app_ready(a)) {
        const char *ex[] = {"--host", port, NULL};
        if (app_launch_game(a, "Play · 2P online (host)", ex) == 0)
            app_toast(a, "Hosting: the join code is copied in the game");
    } else if (it.activate)
        app_toast(a, "Finish the first run first");
    kit_text_wrap(FONT_BODY, 15, lx + 214, y0 + 32, 230, 20, "You are player 1. The game copies your join code: send it to player 2.", C_INK_DIM, 1);

    kit_text(FONT_DISPLAY, 20, lx + 2, y0 + 92, "JOIN WITH A CODE", C_RED, 1, ALIGN_LEFT);
    text_field(EDIT_CODE, lx, y0 + 122, 440, a->join_code, sizeof a->join_code, "Enter, then Ctrl+V the host's code");
    it = item(lx, y0 + 178, 140, 38);
    theme_button(lx, y0 + 178, 140, 38, "JOIN", C_YELLOW, C_INK, it.focused, !a->join_code[0]);
    if (it.activate && a->join_code[0] && app_ready(a)) {
        const char *ex[] = {"--join-code", a->join_code, NULL};
        app_launch_game(a, "Play · 2P online (join code)", ex);
    }
    {
        It p = item(lx + 152, y0 + 178, 140, 38);
        theme_button(lx + 152, y0 + 178, 140, 38, "PASTE", C_CYAN, C_INK, p.focused, 0);
        if (p.activate) {
            char *clip = SDL_GetClipboardText();
            if (clip && clip[0]) {
                char *s = clip;
                while (*s == ' ' || *s == '\n' || *s == '\r')
                    s++;
                snprintf(a->join_code, sizeof a->join_code, "%s", s);
                a->join_code[strcspn(a->join_code, " \r\n")] = 0;
            } else
                app_toast(a, "The clipboard is empty");
            SDL_free(clip);
        }
    }
    kit_text(FONT_DISPLAY, 20, lx + 2, y0 + 240, "OR JOIN AN ADDRESS", C_RED, 1, ALIGN_LEFT);
    text_field(EDIT_ADDR, lx, y0 + 270, 300, c->net_join, sizeof c->net_join, "IP:port");
    it = item(lx + 312, y0 + 273, 128, 38);
    theme_button(lx + 312, y0 + 273, 128, 38, "JOIN", C_PAPER, C_INK, it.focused, !c->net_join[0]);
    if (it.activate && c->net_join[0] && app_ready(a)) {
        const char *ex[] = {"--join", c->net_join, NULL};
        app_save_settings(a);
        app_launch_game(a, "Play · 2P online (address)", ex);
    }

    /* right: settings + how it connects */
    choice_w = 150;
    kit_rect(rx - 20, y0, 3, 470, C_INK, 0.2f);
    {
        float save_px = RY;
        RY = y0;
        snprintf(v, sizeof v, "%d", c->net_delay);
        {
            /* narrow rows on the right half */
            It r = item(rx, RY, PR - rx, 50);
            if (r.focused)
                kit_rect(rx, RY, PR - rx, 50, C_YELLOW, 0.35f), kit_frame(rx, RY, PR - rx, 50, 3, C_INK, 1);
            kit_text(FONT_LABEL, 20, rx + 10, RY + 5, "Input delay", C_INK, 1, ALIGN_LEFT);
            kit_text(FONT_BODY, 15, rx + 10, RY + 29, "frames; 2-3 on slow links", C_INK_DIM, 1, ALIGN_LEFT);
            d = ctl_choice(r, RY + 25, v, 0);
            if (d)
                c->net_delay = SDL_clamp(c->net_delay + d, 0, 3), app_save_settings(a);
            RY += 56;
        }
        {
            It r = item(rx, RY, PR - rx, 50);
            if (r.focused)
                kit_rect(rx, RY, PR - rx, 50, C_YELLOW, 0.35f), kit_frame(rx, RY, PR - rx, 50, 3, C_INK, 1);
            kit_text(FONT_LABEL, 20, rx + 10, RY + 5, "Host port", C_INK, 1, ALIGN_LEFT);
            kit_text(FONT_BODY, 15, rx + 10, RY + 29, "UDP; forward only if UPnP fails", C_INK_DIM, 1, ALIGN_LEFT);
            d = ctl_choice(r, RY + 25, port, 0);
            if (d)
                c->net_port = SDL_clamp(c->net_port + d, 1024, 65535), app_save_settings(a);
            RY += 56;
        }
        {
            It r = item(rx, RY, PR - rx, 50);
            int on = c->net_stun[0] != 0;
            if (r.focused)
                kit_rect(rx, RY, PR - rx, 50, C_YELLOW, 0.35f), kit_frame(rx, RY, PR - rx, 50, 3, C_INK, 1);
            kit_text(FONT_LABEL, 20, rx + 10, RY + 5, "Join codes (STUN)", C_INK, 1, ALIGN_LEFT);
            kit_text(FONT_BODY, 15, rx + 10, RY + 29, "a public server sees your IP", C_INK_DIM, 1, ALIGN_LEFT);
            if (ctl_toggle(r, RY + 25, on, 0)) {
                snprintf(c->net_stun, sizeof c->net_stun, "%s", on ? "" : "stun.l.google.com:19302");
                app_save_settings(a);
            }
            RY += 62;
        }
        kit_text(FONT_DISPLAY, 20, rx + 2, RY, "HOW IT CONNECTS", C_RED, 1, ALIGN_LEFT);
        kit_text_wrap(FONT_BODY, 16, rx + 4, RY + 30, PR - rx - 10, 20,
                      "The host's game opens its port with UPnP and makes a join code. If nothing connects in about "
                      "20 s, player 2's code lands on their clipboard: send it back and the host pastes it. UPnP state, "
                      "ping and rollbacks show in the game window. Strict routers on both sides: forward the port "
                      "on the host, or join over a VPN address. Both players need the same ROM, build and gameplay mods.",
                      C_INK_DIM, 1);
        RY = save_px;
    }
    choice_w = 230;
}

/* ---------------------------------------------------------------- BUILDS */
static void draw_builds(void)
{
    App *a = U.a;
    char cache[96];
    int d;
    SDL_PathInfo pi;
    int have_play = SDL_GetPathInfo(a->play_exe, &pi), have_jit = SDL_GetPathInfo(a->jit_exe, &pi);
    app_cache_info(a, cache, sizeof cache);
    row_info("Game engine", "next to the launcher", have_play || a->demo ? "btdd_play · found" : "MISSING", FONT_LABEL);
    row_info("Native compiler", "compiles your ROM's code on first run (M5)", have_jit || a->demo ? "btdd_jit · found" : "not in this download", FONT_LABEL);
    if (row_button("Native cache", cache, "REBUILD", C_YELLOW)) {
        app_clear_cache(a);
        go(SCR_FIRST_RUN);
        app_start_first_run(a, STEP_COMPILE);
    }
    if (row_button("Self-check", "boots to the title in the background and compares frames", "RUN", C_PAPER)) {
        go(SCR_FIRST_RUN);
        app_start_first_run(a, STEP_CHECK);
    }
    if ((d = row_choice("Picture timing", "old = matches FCEUX (default); new = newer PPU model", a->cfg.ppu_new ? "New" : "Old", 0)))
        a->cfg.ppu_new = !a->cfg.ppu_new, app_save_settings(a);
    RY += 6;
    group("VERSIONS");
    row_info("Launcher", NULL, "Pond to Pavement " LAUNCHER_VERSION, FONT_MONO);
#ifdef BTDD_GIT_HASH
    row_info("Source", NULL, BTDD_GIT_HASH, FONT_MONO);
#endif
    row_info("Newest version", "update checks are off until there is an update host", "local build", FONT_LABEL);
}

/* ---------------------------------------------------------------- LOGS */
static int log_sel;
static void draw_logs(void)
{
    App *a = U.a;
    int i;
    float y;
    kit_text(FONT_LABEL, 13, PX + 10, RY, "WHEN", C_RED, 1, ALIGN_LEFT);
    kit_text(FONT_LABEL, 13, PX + 170, RY, "WHAT", C_RED, 1, ALIGN_LEFT);
    kit_text(FONT_LABEL, 13, PX + 560, RY, "RESULT", C_RED, 1, ALIGN_LEFT);
    kit_text(FONT_LABEL, 13, PX + 720, RY, "TIME", C_RED, 1, ALIGN_LEFT);
    kit_rect(PX, RY + 20, PR - PX, 3, C_INK, 1);
    y = RY + 26;
    if (!a->n_logs)
        kit_text(FONT_BODY, 15, PX + 10, y + 10, "No runs yet. Every game start and first run gets a log here.", C_INK_DIM, 1, ALIGN_LEFT);
    for (i = 0; i < a->n_logs && i < 6; i++) {
        LogEntry *e = &a->logs[i];
        It it = item(PX, y, PR - PX, 32);
        int ok = !strcmp(e->result, "ok");
        if (it.focused)
            log_sel = i;
        if (i == log_sel)
            kit_rect(PX, y, PR - PX, 32, C_YELLOW, 0.4f);
        kit_text(FONT_LABEL, 16, PX + 10, y + 6, e->when, C_INK, 1, ALIGN_LEFT);
        kit_text(FONT_LABEL, 16, PX + 170, y + 6, e->what[0] ? e->what : "?", C_INK, 1, ALIGN_LEFT);
        theme_badge(PX + 560, y + 6, e->result, ok ? C_CYAN : !strcmp(e->result, "running") ? C_YELLOW : C_RED, ok ? C_INK : C_PAPER);
        kit_text(FONT_LABEL, 16, PX + 720, y + 6, e->took, C_INK, 1, ALIGN_LEFT);
        kit_rect(PX, y + 32, PR - PX, 1, C_INK, 0.15f);
        y += 34;
    }
    /* tail of the selected log */
    y = RY + 236;
    theme_box(PX, y, PR - PX, 200, C_INK, 3, 0);
    if (a->demo) {
        static const char *const ex[] = {"# what: Play · 2P online (host)", "# started: 24 Nov 17:31",
                                         "net    upnp: port 7845 open, join code copied",
                                         "net    player 2 connected, ping 38 ms, settings match",
                                         "stop   frames 151,210  rollbacks avg 1.4  max 5", "# took: 42 min", "# result: ok"};
        int k;
        for (k = 0; k < 7; k++)
            kit_text(FONT_MONO, 12.5f, PX + 14, y + 12 + (float)k * 20, ex[k], ex[k][0] == '#' ? C_YELLOW : 0xd8d6e8, 1, ALIGN_LEFT);
    } else if (log_sel < a->n_logs) {
        size_t n = 0;
        char *buf = (char *)SDL_LoadFile(a->logs[log_sel].file, &n);
        if (buf) {
            const char *lines[9];
            int nl = 0, k;
            char *s = buf + n;
            const char *prof = SDL_getenv("USERPROFILE");
            while (s > buf && nl < 9) {
                s--;
                if (*s == '\n' || s == buf) {
                    if (s != buf)
                        *s = 0;
                    if (s[s == buf ? 0 : 1])
                        lines[nl++] = s == buf ? s : s + 1;
                }
            }
            for (k = nl - 1; k >= 0; k--) {
                char ln[220];
                const char *src = lines[k];
                size_t pl = prof ? strlen(prof) : 0;
                char *hit;
                snprintf(ln, sizeof ln, "%s", src);
                ln[strcspn(ln, "\r")] = 0;
                if (pl > 3 && (hit = strstr(ln, prof)) != NULL) {
                    char tail[220];
                    snprintf(tail, sizeof tail, "%s", hit + pl);
                    snprintf(hit, sizeof ln - (size_t)(hit - ln), "%%USERPROFILE%%%s", tail);
                }
                if (strlen(ln) > 110)
                    ln[110] = 0;
                kit_text(FONT_MONO, 12.5f, PX + 14, y + 12 + (float)(nl - 1 - k) * 20, ln,
                         ln[0] == '#' ? C_YELLOW : 0xd8d6e8, 1, ALIGN_LEFT);
            }
            SDL_free(buf);
        }
    }
    y += 214;
    {
        It s = item(PX, y, 190, 38), o = item(PX + 204, y, 190, 38);
        theme_button(PX, y, 190, 38, "SAVE REPORT", C_YELLOW, C_INK, s.focused, 0);
        theme_button(PX + 204, y, 190, 38, "OPEN FOLDER", C_PAPER, C_INK, o.focused, 0);
        kit_text_wrap(FONT_BODY, 15, PX + 414, y + 2, 460, 17,
                      "A report is a folder with recent logs and your settings, user name removed. Nothing is sent anywhere.",
                      C_INK_DIM, 1);
        if (s.activate) {
            char dir[1300];
            if (app_save_report(a, dir, sizeof dir) == 0) {
                app_open_folder(dir);
                app_toast(a, "Report saved");
            } else
                app_toast(a, "Could not save the report");
        }
        if (o.activate) {
            char dir[1100];
            snprintf(dir, sizeof dir, "%s/logs", a->data_dir);
            app_open_folder(dir);
        }
    }
}

/* ---------------------------------------------------------------- ABOUT */
static void draw_about(void)
{
    App *a = U.a;
    float x = PX + 270, y = RY;
    theme_emblem(PX + 120, RY + 110, 210);
    kit_text(FONT_DISPLAY, 30, PX + 120, RY + 226, "POND TO PAVEMENT", C_INK, 1, ALIGN_CENTER);
    kit_text(FONT_LABEL, 16, PX + 120, RY + 262, "YOUR ROM. OUR ENGINE.", C_RED, 1, ALIGN_CENTER);
    y += kit_text_wrap(FONT_BODY, 15, x, y, PR - x - 10, 22,
                       "Pond to Pavement rebuilds a 1993 NES game as a native PC program from your own ROM. It ships no "
                       "game code, graphics, music or names; everything you see before you press PLAY is our own work.",
                       C_INK, 1) + 10;
    y += kit_text_wrap(FONT_BODY, 15, x, y, PR - x - 10, 22,
                       "Not affiliated with or endorsed by the game's publishers or by Nintendo. It needs a legally "
                       "obtained ROM of Battletoads & Double Dragon (USA).",
                       C_INK, 1) + 14;
    {
        static const char *const k[4] = {"CODE", "ART", "FONTS", "LIBRARIES"};
        static const char *const v[4] = {"MIT licence", "launcher art and logo: our own, MIT",
                                         "Bangers, Barlow, Barlow Condensed, JetBrains Mono (SIL OFL 1.1)",
                                         "SDL3 (zlib), stb_truetype (MIT), miniupnpc (BSD), TinyCC (LGPL, separate DLL)"};
        int i;
        for (i = 0; i < 4; i++) {
            kit_text(FONT_LABEL, 16, x, y, k[i], C_RED, 1, ALIGN_LEFT);
            kit_text(FONT_LABEL, 16, x + 110, y, v[i], C_INK, 1, ALIGN_LEFT);
            y += 26;
        }
    }
    y += 16;
    {
        It l = item(x, y, 180, 38), p = item(x + 194, y, 200, 38);
        theme_button(x, y, 180, 38, "LICENCES", C_PAPER, C_INK, l.focused, 0);
        theme_button(x + 194, y, 200, 38, "PROJECT PAGE", C_PAPER, C_INK, p.focused, 0);
        if (l.activate)
            app_open_folder(a->font_dir);
        if (p.activate)
            SDL_OpenURL("https://github.com/ablindingnight/btdd-port");
    }
}

/* ---------------------------------------------------------------- IN-GAME (screenshots only) */
static void draw_ingame(void)
{
    static Menu m;
    SDL_Renderer *r = kit_renderer();
    float sx = 1, sy = 1, gx = (KIT_W - 768) / 2.0f, stripe;
    const char *toast[1] = {"SAVED TO SLOT 3"};
    int n;
    kit_rect(0, 0, KIT_W, KIT_H, 0x050508, 1);
    kit_rect(gx, 0, 768, 720, 0x1b1f3c, 1); /* placeholder for the game picture (never the real one here) */
    for (stripe = -720; stripe < 768; stripe += 48) {
        float p[8];
        p[0] = gx + stripe, p[1] = 0, p[2] = gx + stripe + 24, p[3] = 0;
        p[4] = gx + stripe + 24 + 720, p[5] = 720, p[6] = gx + stripe + 720, p[7] = 720;
        kit_quad(p, 0x161a34, 1);
    }
    kit_rect(0, 0, gx, KIT_H, 0x050508, 1);
    kit_rect(gx + 768, 0, gx, KIT_H, 0x050508, 1);
    kit_text(FONT_LABEL, 18, gx + 384, 600, "the game picture is here (256 x 240, shown at 3x)", 0x8890b0, 1, ALIGN_CENTER);
    menu_open(&m, 0);
    menu_begin(&m, "SETTINGS");
    menu_add(&m, ITEM_ACTION, 1, "Resume", "", "Esc closes this menu");
    menu_add(&m, ITEM_PAGE, 2, "Save states", "", "");
    menu_add(&m, ITEM_PAGE, 3, "Display", "", "window, fullscreen, aspect, widescreen");
    menu_add(&m, ITEM_PAGE, 4, "Graphics", "", "");
    menu_add(&m, ITEM_PAGE, 5, "Audio", "", "");
    menu_add(&m, ITEM_PAGE, 6, "Controls", "", "");
    menu_add(&m, ITEM_PAGE, 7, "Gameplay", "", "");
    menu_add(&m, ITEM_PAGE, 8, "Mods", "", "");
    menu_add(&m, ITEM_PAGE, 9, "Cheats", "", "the game's own codes, entered for you");
    menu_add(&m, ITEM_PAGE, 10, "Online 2-player", "", "play with a friend over the internet");
    menu_add(&m, ITEM_ACTION, 11, "Reset", "", "F1 in game");
    menu_add(&m, ITEM_ACTION, 12, "Power cycle", "", "Ctrl+F1 in game");
    menu_add(&m, ITEM_ACTION, 13, "Quit", "", "settings are saved");
    m.cursor[0] = 2;
    n = menu_render(&m);
    SDL_GetRenderScale(r, &sx, &sy);
    SDL_SetRenderScale(r, sx * 3, sy * 3);
    video_draw_overlay(r, gx / 3, 0, m.lines, n, 1);
    video_draw_overlay(r, gx / 3 + 150, 214, toast, 1, 0);
    SDL_SetRenderScale(r, sx, sy);
}

/* ---------------------------------------------------------------- ROM picking */
static void SDLCALL dialog_cb(void *ud, const char *const *files, int filter)
{
    (void)ud;
    (void)filter;
    if (files && files[0]) {
        snprintf(pending_rom, sizeof pending_rom, "%s", files[0]);
        SDL_SetAtomicInt(&pending_flag, 1);
    }
}

static void open_rom_dialog(void)
{
    static const SDL_DialogFileFilter f[] = {{"NES ROM (.nes or .zip)", "nes;zip"}, {"All files", "*"}};
    if (U.a->demo)
        return;
    SDL_ShowOpenFileDialog(dialog_cb, NULL, U.win, f, 2, NULL, false);
}

void screens_drop_file(App *a, const char *path)
{
    if (a->demo)
        return;
    a->screen = SCR_FIRST_RUN;
    app_set_rom(a, path);
}

void screens_poll(App *a)
{
    if (SDL_GetAtomicInt(&pending_flag)) {
        SDL_SetAtomicInt(&pending_flag, 0);
        screens_drop_file(a, pending_rom);
    }
}

/* ---------------------------------------------------------------- frame */
void screens_draw(App *a, UiIn *in)
{
    int *f;
    U.a = a;
    U.in = in;
    U.scr = a->screen;
    U.n = 0;
    f = &a->focus[U.scr];
    /* keyboard / pad focus moves (not while typing or capturing a key) */
    if (!a->capture && !a->edit && U.n_prev[U.scr] > 0) {
        int n = U.n_prev[U.scr], step = in->nav_y;
        if (U.scr == SCR_HOME && in->nav_x)
            step = in->nav_x;
        if (step) {
            *f = ((*f + step) % n + n) % n;
            if (U.scr == SCR_HOME)
                in->nav_x = 0; /* home: left/right move focus; elsewhere they change the focused value */
        }
        if (*f >= n)
            *f = n - 1;
    }
    if (a->capture || a->edit)
        in->nav_x = in->nav_y = 0;
    if (in->play && app_ready(a) && !a->edit && !a->capture)
        app_launch_game(a, "Play", NULL);
    switch (a->screen) {
    case SCR_HOME:
        if (in->back)
            in->back = 0; /* nothing above home */
        draw_home();
        break;
    case SCR_FIRST_RUN: draw_first_run(); break;
    case SCR_INGAME: draw_ingame(); break;
    default:
        settings_shell();
        if ((int)a->screen != U.scr) /* page changed in the shell */
            break;
        switch (a->screen) {
        case SCR_DISPLAY: draw_display(); break;
        case SCR_GRAPHICS: draw_graphics(); break;
        case SCR_AUDIO: draw_audio(); break;
        case SCR_CONTROLS: draw_controls(); break;
        case SCR_GAMEPLAY: draw_gameplay(); break;
        case SCR_MODS: draw_mods(); break;
        case SCR_ONLINE: draw_online(); break;
        case SCR_BUILDS: draw_builds(); break;
        case SCR_LOGS: draw_logs(); break;
        case SCR_ABOUT: draw_about(); break;
        default: break;
        }
        hints(U.scr == SCR_CONTROLS ? "rebind" : "change");
        game_running_banner();
        break;
    }
    toast();
    U.n_prev[U.scr] = U.n;
}
