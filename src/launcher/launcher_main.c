/* Pond to Pavement launcher (M11): takes the player's ROM, runs the first-run checklist, holds every setting of
 * btdd_play (shared btdd.cfg) and starts the game with PLAY. Look: docs/LAUNCHER_STYLE.md.
 *
 *   PondToPavement [--demo]                              the launcher
 *   PondToPavement --ui-shot <screen> out.png [w h] [--demo]  render one screen offscreen to a PNG and exit
 *   PondToPavement --launch-game                         start the game as PLAY does and exit
 *   PondToPavement --selftest [--rom X.nes|X.zip]       run the first-run checklist without a window
 * Screens: home, first-run, display, graphics, audio, controls, gameplay, mods, online, builds, logs, about.
 * --demo fills example values (verified ROM, used save slots) for screenshots; it never writes settings. */
#include "app.h"
#include "kit.h"
#include "screens.h"
#include "theme.h"
#include "platform/png.h"
#include <SDL3/SDL_main.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int ui_shot(App *a, SDL_Renderer *r, const char *name, const char *out, int w, int h)
{
    Screen s = screen_from_name(name);
    SDL_Texture *t;
    SDL_Surface *px, *conv;
    UiIn in;
    int rc = 1;
    if (s == SCR_COUNT) {
        SDL_Log("unknown screen \"%s\"", name);
        return 2;
    }
    t = SDL_CreateTexture(r, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_TARGET, w, h);
    if (!t)
        return 1;
    SDL_SetRenderTarget(r, t);
    SDL_SetRenderScale(r, (float)w / KIT_W, (float)h / KIT_H);
    kit_set_pixel_scale((float)w / KIT_W);
    a->screen = s;
    a->now = 0.4; /* a moment into the idle animations */
    memset(&in, 0, sizeof in);
    in.mx = in.my = -1;
    SDL_SetRenderDrawColor(r, 0, 0, 0, 255);
    SDL_RenderClear(r);
    screens_draw(a, &in);
    SDL_RenderClear(r); /* second frame: focus counts from the first one are known */
    screens_draw(a, &in);
    px = SDL_RenderReadPixels(r, NULL);
    if (px) {
        conv = SDL_ConvertSurface(px, SDL_PIXELFORMAT_XRGB8888);
        if (conv) {
            rc = png_write_rgb(out, (const uint32_t *)conv->pixels, conv->w, conv->h, conv->pitch / 4) ? 1 : 0;
            SDL_DestroySurface(conv);
        }
        SDL_DestroySurface(px);
    }
    SDL_SetRenderTarget(r, NULL);
    SDL_DestroyTexture(t);
    return rc;
}

static void stick(UiIn *in, int axis, int value, int *held)
{
    int dir = value > 20000 ? 1 : value < -20000 ? -1 : 0;
    if (dir && dir != *held) {
        if (axis == SDL_GAMEPAD_AXIS_LEFTX)
            in->nav_x = dir;
        else
            in->nav_y = dir;
    }
    *held = dir;
}

int main(int argc, char **argv)
{
    static App app;
    App *a = &app;
    SDL_Window *win;
    SDL_Renderer *r;
    const char *shot = NULL, *shot_out = NULL, *rom_arg = NULL;
    int shot_w = KIT_W, shot_h = KIT_H, demo = 0, launch = 0, i, rc = 0;
    int held_x = 0, held_y = 0, pad = 0, selftest = 0, win_w = KIT_W, win_h = KIT_H;

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--ui-shot") && i + 2 < argc) {
            shot = argv[++i];
            shot_out = argv[++i];
            if (i + 2 < argc && argv[i + 1][0] != '-') {
                shot_w = atoi(argv[++i]);
                shot_h = atoi(argv[++i]);
            }
        } else if (!strcmp(argv[i], "--demo"))
            demo = 1;
        else if (!strcmp(argv[i], "--launch-game"))
            launch = 1;
        else if (!strcmp(argv[i], "--selftest"))
            selftest = 1;
        else if (!strcmp(argv[i], "--rom") && i + 1 < argc)
            rom_arg = argv[++i];
    }
    if (shot_w < 64 || shot_h < 36)
        shot_w = KIT_W, shot_h = KIT_H;

    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) {
        SDL_Log("SDL_Init: %s", SDL_GetError());
        return 1;
    }
    app_init(a, argv[0], demo);
    if (selftest) { /* the first-run checklist without a window; prints each step (tests, check_all) */
        static const char *const st[] = {"wait", "run", "done", "skipped", "FAIL"};
        int fails = 0;
        if (rom_arg) /* picks this ROM (.nes or .zip) as the dialog would; saves it to btdd.cfg */
            app_set_rom(a, rom_arg);
        else
            app_start_first_run(a, STEP_VERIFY);
        if (a->worker)
            SDL_WaitThread(a->worker, NULL);
        a->worker = NULL;
        for (i = STEP_VERIFY; i < STEP_COUNT; i++) {
            Step s;
            app_step_get(a, i, &s);
            printf("step %d: %s  %s\n", i + 1, st[s.state], s.note);
            fails += s.state == STEP_FAIL || s.state == STEP_WAIT;
        }
        printf("ready: %s\n", app_ready(a) ? "yes" : "no");
        app_quit(a);
        SDL_Quit();
        return fails || !app_ready(a) ? 1 : 0;
    }
    if (launch) {
        rc = app_ready(a) ? app_launch_game(a, "Play (from --launch-game)", NULL) : 1;
        if (rc)
            SDL_Log("not ready: pick and check the ROM in the launcher first");
        a->game = NULL; /* leave it running */
        SDL_Quit();
        return rc ? 1 : 0;
    }
    {
        /* opens at the 1280x720 design size (smaller only on small desktops); text stays sharp when resized */
        SDL_Rect ub;
        float f = 1.0f;
        if (!shot && SDL_GetDisplayUsableBounds(SDL_GetPrimaryDisplay(), &ub)) {
            float fx = 0.95f * (float)ub.w / KIT_W, fy = 0.95f * (float)ub.h / KIT_H;
            f = fx < fy ? fx : fy;
            f = f > 1.0f ? 1.0f : f < 0.5f ? 0.5f : f;
        }
        win_w = (int)(KIT_W * f);
        win_h = (int)(KIT_H * f);
    }
    if (!SDL_CreateWindowAndRenderer("Pond to Pavement", win_w, win_h,
                                     SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY | (shot ? SDL_WINDOW_HIDDEN : 0), &win, &r)) {
        SDL_Log("window: %s", SDL_GetError());
        SDL_Quit();
        return 1;
    }
    SDL_SetWindowMinimumSize(win, 640, 360);
    SDL_SetRenderVSync(r, 1);
    screens_set_window(win);
    if (kit_init(r, a->font_dir) != 0)
        SDL_Log("%s", kit_error());
    theme_init();
    a->focus[SCR_HOME] = 8; /* PLAY */

    if (shot) {
        rc = ui_shot(a, r, shot, shot_out, shot_w, shot_h);
        if (rc == 0)
            printf("wrote %s\n", shot_out);
    } else {
        SDL_SetRenderLogicalPresentation(r, KIT_W, KIT_H, SDL_LOGICAL_PRESENTATION_LETTERBOX);
        if (!app_ready(a)) {
            a->screen = SCR_FIRST_RUN;
            if (a->cfg.rom[0])
                app_start_first_run(a, STEP_VERIFY);
        }
        while (!a->quit) {
            SDL_Event e;
            UiIn in;
            int got;
            memset(&in, 0, sizeof in);
            in.pad = pad;
            got = SDL_WaitEventTimeout(&e, a->game ? 100 : 15);
            while (got) {
                SDL_ConvertEventToRenderCoordinates(r, &e);
                switch (e.type) {
                case SDL_EVENT_QUIT: a->quit = 1; break;
                case SDL_EVENT_KEY_DOWN: {
                    SDL_Scancode sc = e.key.scancode;
                    pad = 0;
                    if (!e.key.repeat)
                        in.key = sc;
                    if (a->capture)
                        break;
                    switch (sc) {
                    case SDL_SCANCODE_UP: in.nav_y = -1; break;
                    case SDL_SCANCODE_DOWN: in.nav_y = 1; break;
                    case SDL_SCANCODE_LEFT: in.nav_x = -1; break;
                    case SDL_SCANCODE_RIGHT: in.nav_x = 1; break;
                    case SDL_SCANCODE_RETURN: case SDL_SCANCODE_KP_ENTER: in.accept = 1; in.enter = 1; break;
                    case SDL_SCANCODE_SPACE: if (!a->edit) in.accept = 1; break;
                    case SDL_SCANCODE_ESCAPE: in.back = 1; break;
                    case SDL_SCANCODE_PAGEUP: in.page = -1; break;
                    case SDL_SCANCODE_PAGEDOWN: in.page = 1; break;
                    case SDL_SCANCODE_F5: in.play = 1; break;
                    case SDL_SCANCODE_BACKSPACE: in.backspace = 1; break;
                    case SDL_SCANCODE_V: if (e.key.mod & SDL_KMOD_CTRL) in.paste = 1; break;
                    default: break;
                    }
                    break;
                }
                case SDL_EVENT_TEXT_INPUT:
                    if (strlen(in.text) + strlen(e.text.text) < sizeof in.text)
                        strcat(in.text, e.text.text);
                    break;
                case SDL_EVENT_MOUSE_MOTION:
                    in.mouse_moved = 1;
                    break;
                case SDL_EVENT_MOUSE_BUTTON_DOWN:
                    if (e.button.button == SDL_BUTTON_LEFT)
                        in.click = 1;
                    break;
                case SDL_EVENT_GAMEPAD_ADDED: SDL_OpenGamepad(e.gdevice.which); break;
                case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
                    pad = in.pad = 1;
                    switch (e.gbutton.button) {
                    case SDL_GAMEPAD_BUTTON_DPAD_UP: in.nav_y = -1; break;
                    case SDL_GAMEPAD_BUTTON_DPAD_DOWN: in.nav_y = 1; break;
                    case SDL_GAMEPAD_BUTTON_DPAD_LEFT: in.nav_x = -1; break;
                    case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: in.nav_x = 1; break;
                    case SDL_GAMEPAD_BUTTON_SOUTH: in.accept = 1; in.enter = 1; break;
                    case SDL_GAMEPAD_BUTTON_EAST: in.back = 1; break;
                    case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER: in.page = -1; break;
                    case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER: in.page = 1; break;
                    case SDL_GAMEPAD_BUTTON_START: in.play = 1; break;
                    default: break;
                    }
                    break;
                case SDL_EVENT_GAMEPAD_AXIS_MOTION:
                    if (e.gaxis.axis == SDL_GAMEPAD_AXIS_LEFTX)
                        stick(&in, e.gaxis.axis, e.gaxis.value, &held_x);
                    else if (e.gaxis.axis == SDL_GAMEPAD_AXIS_LEFTY)
                        stick(&in, e.gaxis.axis, e.gaxis.value, &held_y);
                    break;
                case SDL_EVENT_DROP_FILE:
                    if (e.drop.data)
                        screens_drop_file(a, e.drop.data);
                    break;
                default: break;
                }
                got = SDL_PollEvent(&e);
            }
            {
                float mx, my;
                SDL_GetMouseState(&mx, &my);
                SDL_RenderCoordinatesFromWindow(r, mx, my, &in.mx, &in.my);
            }
            a->now = (double)SDL_GetTicks() / 1000.0;
            app_poll(a);
            screens_poll(a);
            {
                int ow = KIT_W, oh = KIT_H;
                float sx, sy;
                SDL_GetCurrentRenderOutputSize(r, &ow, &oh);
                sx = (float)ow / KIT_W;
                sy = (float)oh / KIT_H;
                kit_set_pixel_scale(sx < sy ? sx : sy);
            }
            SDL_SetRenderDrawColor(r, 0x0b, 0x0b, 0x14, 255);
            SDL_RenderClear(r);
            screens_draw(a, &in);
            SDL_RenderPresent(r);
        }
    }
    theme_quit();
    kit_quit();
    app_quit(a);
    SDL_DestroyRenderer(r);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return rc;
}
