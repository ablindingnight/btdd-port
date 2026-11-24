/* btdd_play: the game in a window, running on the reference interpreter (milestone M2).
 *
 *   btdd_play [ROM] [--movie X.fm2] [--frames N] [--ppu old|new] [--scale S] [--fullscreen] [--no-audio]
 *             [--selftest-gamepad]
 *
 * Keys (defaults, changeable in btdd.cfg or with F5): player 1 arrows, X = A, Z = B, Right Shift = Select,
 * Enter = Start; player 2 W/A/S/D, H = A, G = B, T = Select, Y = Start. Gamepads: the first two are
 * players 1 and 2 (east button = A, south button = B, Back = Select, Start, d-pad or left stick).
 * Hotkeys: Esc quit, P pause, F1 reset (Ctrl+F1 power cycle), F2 square pixels / 4:3, F3 filter
 * (nearest / sharp / bilinear), F4 scanlines, F5 redefine keys, F11 or Alt+Enter fullscreen, F12 screenshot.
 * --movie plays an fm2 instead of live input (--frames N quits after N frames, for automated checks);
 * --selftest-gamepad checks the gamepad path with a virtual pad.
 *
 * Timing: the emulator produces one frame (60.0988 Hz) whenever the audio queue holds less than ~3 frames
 * of sound, so sound never starves and the picture follows the sound card's clock. Without an audio
 * device it falls back to a timer. */
#include "config.h"
#include "nes/apu.h"
#include "nes/nes.h"
#include "platform/png.h"
#include "platform/screen.h"
#include "rom/rom.h"
#include "runtime/fm2.h"
#include "video.h"
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define AUDIO_RATE 48000
#define FRAME_HZ (APU_CPU_HZ / 29780.5)
#define QUEUE_TARGET_FRAMES 3
#define TITLE "Battletoads & Double Dragon"

static Rom rom;
static Nes nes;
static Screen screen;
static uint32_t pixels[PPU_W * PPU_H];
static float samples[AUDIO_RATE / 10];

/* ---- APU: owned by Nes (nes_enable_apu); the frame loop collects its samples ---- */

static void attach_apu(void)
{
    nes_enable_apu(&nes, AUDIO_RATE);
    apu_set_buffer(&nes.apu, samples, sizeof samples / sizeof samples[0]);
}

static void power_on(PpuModel model)
{
    nes_init(&nes, &rom);
    nes_power(&nes, model);
    screen_attach(&screen, &nes.ppu);
    attach_apu();
}

/* Reset button, as FCEUX does it: CPU takes the reset vector, PPU registers clear and it skips two
 * frames, the APU restarts; RAM, CHR-RAM, palette, OAM and the mapper bank are kept. */
static void soft_reset(void)
{
    Ppu *p = &nes.ppu;
    cpu_reset(&nes.cpu);
    p->read_buf = p->ctrl = p->mask = p->status = p->oam_addr = p->oam_low = p->latch = 0;
    p->v = p->t = 0;
    p->toggle = 0;
    p->dead_frames = 2;
    p->odd = 0;
    p->idle_synch = 1;
    memset(&p->sc, 0, sizeof p->sc);
    p->sl = 241;
    p->dot = 0;
    p->end_dot = 341;
    attach_apu();
}

/* ---- input ---- */

static const uint8_t pad_bits[BTN_COUNT] = {PAD_A, PAD_B, PAD_SELECT, PAD_START, PAD_UP, PAD_DOWN, PAD_LEFT, PAD_RIGHT};

static uint8_t read_pad(const Config *cfg, int port, SDL_Gamepad *gp)
{
    const bool *keys = SDL_GetKeyboardState(NULL);
    uint8_t v = 0;
    int b;
    for (b = 0; b < BTN_COUNT; b++)
        if (keys[cfg->keys[port][b]])
            v |= pad_bits[b];
    if (gp) {
        Sint16 lx = SDL_GetGamepadAxis(gp, SDL_GAMEPAD_AXIS_LEFTX);
        Sint16 ly = SDL_GetGamepadAxis(gp, SDL_GAMEPAD_AXIS_LEFTY);
        if (SDL_GetGamepadButton(gp, SDL_GAMEPAD_BUTTON_EAST))
            v |= PAD_A;
        if (SDL_GetGamepadButton(gp, SDL_GAMEPAD_BUTTON_SOUTH))
            v |= PAD_B;
        if (SDL_GetGamepadButton(gp, SDL_GAMEPAD_BUTTON_BACK))
            v |= PAD_SELECT;
        if (SDL_GetGamepadButton(gp, SDL_GAMEPAD_BUTTON_START))
            v |= PAD_START;
        if (SDL_GetGamepadButton(gp, SDL_GAMEPAD_BUTTON_DPAD_UP) || ly < -16000)
            v |= PAD_UP;
        if (SDL_GetGamepadButton(gp, SDL_GAMEPAD_BUTTON_DPAD_DOWN) || ly > 16000)
            v |= PAD_DOWN;
        if (SDL_GetGamepadButton(gp, SDL_GAMEPAD_BUTTON_DPAD_LEFT) || lx < -16000)
            v |= PAD_LEFT;
        if (SDL_GetGamepadButton(gp, SDL_GAMEPAD_BUTTON_DPAD_RIGHT) || lx > 16000)
            v |= PAD_RIGHT;
    }
    /* a real d-pad cannot press opposite directions; some games misbehave if it happens */
    if ((v & (PAD_UP | PAD_DOWN)) == (PAD_UP | PAD_DOWN))
        v &= (uint8_t)~(PAD_UP | PAD_DOWN);
    if ((v & (PAD_LEFT | PAD_RIGHT)) == (PAD_LEFT | PAD_RIGHT))
        v &= (uint8_t)~(PAD_LEFT | PAD_RIGHT);
    return v;
}

static void open_gamepads(SDL_Gamepad *gp[2])
{
    int n = 0, i, slot = 0;
    SDL_JoystickID *ids = SDL_GetGamepads(&n);
    for (i = 0; i < 2; i++) {
        if (gp[i]) {
            SDL_CloseGamepad(gp[i]);
            gp[i] = NULL;
        }
    }
    for (i = 0; ids && i < n && slot < 2; i++)
        if ((gp[slot] = SDL_OpenGamepad(ids[i])) != NULL)
            slot++;
    SDL_free(ids);
}

/* ---- on-screen messages and key rebinding ---- */

static char message[160];
static Uint64 message_until;

static void say(const char *text)
{
    SDL_strlcpy(message, text, sizeof message);
    message_until = SDL_GetTicksNS() + 2500000000ull;
    SDL_Log("%s", text);
}

typedef struct Rebind {
    int active, step; /* step 0..2*BTN_COUNT-1: player = step / BTN_COUNT, button = step % BTN_COUNT */
    SDL_Scancode backup[2][BTN_COUNT];
} Rebind;

static const char *const button_labels[BTN_COUNT] = {"A", "B", "SELECT", "START", "UP", "DOWN", "LEFT", "RIGHT"};

/* Returns 1 if the key was consumed by the rebinding flow. */
static int rebind_key(Rebind *rb, Config *cfg, SDL_Scancode sc)
{
    char text[160];
    if (!rb->active)
        return 0;
    if (sc == SDL_SCANCODE_ESCAPE) {
        memcpy(cfg->keys, rb->backup, sizeof rb->backup);
        rb->active = 0;
        say("key setup cancelled");
        return 1;
    }
    if (sc != SDL_SCANCODE_BACKSPACE) /* Backspace keeps the current key */
        cfg->keys[rb->step / BTN_COUNT][rb->step % BTN_COUNT] = sc;
    if (++rb->step == 2 * BTN_COUNT) {
        rb->active = 0;
        if (config_save(cfg) == 0)
            snprintf(text, sizeof text, "keys saved to btdd.cfg");
        else
            snprintf(text, sizeof text, "keys set, but btdd.cfg could not be written");
        say(text);
    }
    return 1;
}

static int overlay(const Rebind *rb, const Config *cfg, int paused, const char *lines[4])
{
    static char l0[96], l1[96];
    int n = 0;
    if (rb->active) {
        int p = rb->step / BTN_COUNT, b = rb->step % BTN_COUNT;
        snprintf(l0, sizeof l0, "PLAYER %d: PRESS KEY FOR %s", p + 1, button_labels[b]);
        snprintf(l1, sizeof l1, "NOW: %s  BKSP=KEEP ESC=CANCEL", SDL_GetScancodeName(cfg->keys[p][b]));
        lines[n++] = l0;
        lines[n++] = l1;
    } else if (paused) {
        lines[n++] = "PAUSED (P)";
    }
    if (message_until > SDL_GetTicksNS())
        lines[n++] = message;
    return n;
}

/* ---- palette ---- */

static void load_palette(const char *path)
{
    static uint8_t buf[512 * 3 + 1];
    size_t n;
    char text[160];
    FILE *f;
    if (!path[0])
        return;
    f = fopen(path, "rb");
    n = f ? fread(buf, 1, sizeof buf, f) : 0;
    if (f)
        fclose(f);
    if (n == 64 * 3 || n == 512 * 3)
        snprintf(text, sizeof text, "palette: %.100s (%d colors)", path, (int)(n / 3));
    else
        snprintf(text, sizeof text, "palette %.100s not used (needs 192 or 1536 bytes)", path);
    if ((n == 64 * 3 || n == 512 * 3) && screen_set_palette(buf, (int)(n / 3)) == 0)
        SDL_Log("%s", text);
    else
        say(text);
}

static void save_screenshot(const Config *cfg)
{
    char path[1100], *slash, text[160];
    static int n;
    snprintf(path, sizeof path, "%s", cfg->path);
    slash = strrchr(path, '\\');
    if (!slash)
        slash = strrchr(path, '/');
    if (!slash)
        return;
    snprintf(slash + 1, sizeof path - (size_t)(slash + 1 - path), "shot_%llu_%d.png",
             (unsigned long long)nes.ppu.frame, n++);
    if (png_write_rgb(path, pixels, PPU_W, PPU_H, PPU_W) == 0) {
        snprintf(text, sizeof text, "screenshot saved (settings folder)");
        SDL_Log("screenshot: %s", path);
        say(text);
    }
}

/* --selftest-gamepad: a virtual SDL gamepad presses buttons; the game's own input byte ($30, player 1
 * buttons held, bit order A=$80 B=$40 Select=$20 Start=$10 Up=$08 Down=$04 Left=$02 Right=$01) must show
 * them. Exercises SDL gamepad detection, the mapping and the controller port. */
typedef struct SelfTest {
    long start; /* frame the step starts */
    SDL_GamepadButton button;
    uint8_t expect;
} SelfTest;
static const SelfTest selftest[] = {
    {300, SDL_GAMEPAD_BUTTON_START, 0x10},
    {330, SDL_GAMEPAD_BUTTON_EAST, 0x80},
    {360, SDL_GAMEPAD_BUTTON_SOUTH, 0x40},
    {390, SDL_GAMEPAD_BUTTON_DPAD_DOWN, 0x04},
    {420, SDL_GAMEPAD_BUTTON_DPAD_RIGHT, 0x01},
};
#define SELFTEST_STEPS (int)(sizeof selftest / sizeof selftest[0])

static int fail(const char *what)
{
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "btdd", what, NULL);
    fprintf(stderr, "%s\n", what);
    return 1;
}

int main(int argc, char **argv)
{
    Config cfg;
    Video video;
    Rebind rb;
    const char *movie_path = NULL;
    long max_frames = -1, frame = 0;
    int i, quit = 0, paused = 0, have_cfg, no_audio = 0, selftest_on = 0, seen[SELFTEST_STEPS] = {0};
    SDL_Joystick *vjoy = NULL;
    char hash[41], msg[1400];
    Fm2 movie = {0};
    SDL_AudioStream *audio;
    SDL_AudioSpec spec = {SDL_AUDIO_F32, 1, AUDIO_RATE};
    SDL_Gamepad *gp[2] = {NULL, NULL};
    Uint64 next_tick;
    RomResult rr;

    memset(&rb, 0, sizeof rb);
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMEPAD)) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    have_cfg = config_load(&cfg) == 0;
    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--movie") && i + 1 < argc)
            movie_path = argv[++i];
        else if (!strcmp(argv[i], "--frames") && i + 1 < argc)
            max_frames = strtol(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--ppu") && i + 1 < argc)
            cfg.ppu_new = !strcmp(argv[++i], "new");
        else if (!strcmp(argv[i], "--scale") && i + 1 < argc)
            cfg.scale = SDL_clamp(atoi(argv[++i]), 1, 10);
        else if (!strcmp(argv[i], "--selftest-gamepad"))
            selftest_on = 1;
        else if (!strcmp(argv[i], "--no-audio"))
            no_audio = 1;
        else if (!strcmp(argv[i], "--fullscreen"))
            cfg.fullscreen = 1;
        else if (argv[i][0] != '-')
            snprintf(cfg.rom, sizeof cfg.rom, "%s", argv[i]);
        else
            return fail("usage: btdd_play [ROM] [--movie X.fm2] [--frames N] [--ppu old|new] [--scale S] "
                        "[--fullscreen] [--no-audio] [--selftest-gamepad]");
    }
    if (!have_cfg)
        SDL_Log("could not read or create btdd.cfg; using defaults");
    else
        SDL_Log("settings: %s", cfg.path);
    if (!cfg.rom[0]) {
        snprintf(msg, sizeof msg, "No ROM given. Pass the path to your Battletoads-Double Dragon (USA).nes "
                                  "on the command line or set rom = ... in\n%s", cfg.path);
        return fail(msg);
    }
    rr = rom_load(cfg.rom, &rom, hash);
    if (rr != ROM_OK) {
        snprintf(msg, sizeof msg, "%s:\n%s", cfg.rom, rom_result_text(rr));
        return fail(msg);
    }
    if (movie_path) {
        if (fm2_load(movie_path, &movie, msg, sizeof msg) != 0)
            return fail(msg);
        if (movie.new_ppu)
            cfg.ppu_new = 1;
    }
    load_palette(cfg.palette);

    if (video_open(&video, TITLE, cfg.scale, cfg.aspect, cfg.fullscreen, cfg.filter, cfg.scanlines,
                   cfg.renderer) != 0) {
        snprintf(msg, sizeof msg, "cannot open the window: %s", SDL_GetError());
        return fail(msg);
    }
    audio = no_audio ? NULL : SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, NULL, NULL);
    if (audio) {
        SDL_SetAudioStreamGain(audio, cfg.volume);
        SDL_ResumeAudioStreamDevice(audio);
    } else {
        SDL_Log("no audio (%s); pacing by timer", SDL_GetError());
    }
    if (selftest_on) {
        SDL_VirtualJoystickDesc d;
        SDL_JoystickID id;
        /* the test window is usually not focused; SDL drops pad input for unfocused apps otherwise */
        SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
        SDL_INIT_INTERFACE(&d);
        d.type = SDL_JOYSTICK_TYPE_GAMEPAD;
        d.naxes = SDL_GAMEPAD_AXIS_COUNT;
        d.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
        d.name = "btdd self-test pad";
        d.button_mask = (1u << SDL_GAMEPAD_BUTTON_COUNT) - 1; /* standard gamepad layout */
        d.axis_mask = (1u << SDL_GAMEPAD_AXIS_COUNT) - 1;
        id = SDL_AttachVirtualJoystick(&d);
        vjoy = id ? SDL_OpenJoystick(id) : NULL;
        if (!vjoy)
            return fail("cannot create the virtual gamepad");
        max_frames = 450;
    }
    open_gamepads(gp);
    if (vjoy) { /* the self-test pad is player 1 even if real pads are connected */
        if (gp[0])
            SDL_CloseGamepad(gp[0]);
        gp[0] = SDL_OpenGamepad(SDL_GetJoystickID(vjoy));
        SDL_Log("self-test pad: %s", gp[0] ? SDL_GetGamepadName(gp[0]) : SDL_GetError());
    }

    power_on(cfg.ppu_new ? PPU_MODEL_NEW : PPU_MODEL_OLD);
    next_tick = SDL_GetTicksNS();

    while (!quit) {
        SDL_Event ev;
        int run_frame, hold = paused || rb.active;
        const char *lines[4];
        while (SDL_PollEvent(&ev)) {
            switch (ev.type) {
            case SDL_EVENT_QUIT:
                quit = 1;
                break;
            case SDL_EVENT_GAMEPAD_ADDED:
            case SDL_EVENT_GAMEPAD_REMOVED:
                if (!vjoy)
                    open_gamepads(gp);
                break;
            case SDL_EVENT_KEY_DOWN:
                if (ev.key.repeat || rebind_key(&rb, &cfg, ev.key.scancode))
                    break;
                if (ev.key.key == SDLK_ESCAPE) {
                    quit = 1;
                } else if (ev.key.key == SDLK_P) {
                    paused = !paused;
                    SDL_SetWindowTitle(video.win, paused ? TITLE " (paused)" : TITLE);
                } else if (ev.key.key == SDLK_F1) {
                    if (ev.key.mod & SDL_KMOD_CTRL) {
                        power_on(nes.ppu.model);
                        say("power cycle");
                    } else {
                        soft_reset();
                        say("reset");
                    }
                } else if (ev.key.key == SDLK_F11 ||
                           (ev.key.key == SDLK_RETURN && (ev.key.mod & SDL_KMOD_ALT))) {
                    cfg.fullscreen = !cfg.fullscreen;
                    SDL_SetWindowFullscreen(video.win, cfg.fullscreen);
                } else if (ev.key.key == SDLK_F2) {
                    video.aspect = cfg.aspect = !cfg.aspect;
                    say(cfg.aspect ? "4:3 picture" : "square pixels");
                } else if (ev.key.key == SDLK_F3) {
                    video.filter = cfg.filter = (cfg.filter + 1) % FILTER_COUNT;
                    snprintf(msg, sizeof msg, "filter: %s", video_filter_names[cfg.filter]);
                    say(msg);
                } else if (ev.key.key == SDLK_F4) {
                    video.scanlines = cfg.scanlines = !cfg.scanlines;
                    say(cfg.scanlines ? "scanlines on" : "scanlines off");
                } else if (ev.key.key == SDLK_F5) {
                    rb.active = 1;
                    rb.step = 0;
                    memcpy(rb.backup, cfg.keys, sizeof rb.backup);
                } else if (ev.key.key == SDLK_F12) {
                    save_screenshot(&cfg);
                }
                break;
            default:
                break;
            }
        }
        if (hold) { /* paused or redefining keys: keep drawing the last picture with the overlay */
            if (audio)
                SDL_ClearAudioStream(audio);
            video_present(&video, pixels, lines, overlay(&rb, &cfg, paused, lines));
            next_tick = SDL_GetTicksNS();
            SDL_Delay(15);
            continue;
        }

        /* pacing: keep about QUEUE_TARGET_FRAMES of audio queued */
        if (audio) {
            int queued = SDL_GetAudioStreamQueued(audio);
            run_frame = queued < (int)(QUEUE_TARGET_FRAMES * AUDIO_RATE / FRAME_HZ) * (int)sizeof(float);
        } else {
            run_frame = SDL_GetTicksNS() >= next_tick;
            if (run_frame)
                next_tick += (Uint64)(1e9 / FRAME_HZ);
        }
        if (!run_frame) {
            SDL_Delay(1);
            continue;
        }

        if (movie.frames) {
            if (frame >= movie.count)
                break;
            nes_set_pads(&nes, movie.frames[frame].pad[0], movie.frames[frame].pad[1]);
        } else {
            if (vjoy) {
                int k;
                for (k = 0; k < SELFTEST_STEPS; k++)
                    SDL_SetJoystickVirtualButton(vjoy, selftest[k].button,
                                                 frame >= selftest[k].start && frame < selftest[k].start + 8);
                SDL_UpdateJoysticks();
            }
            nes_set_pads(&nes, read_pad(&cfg, 0, gp[0]), read_pad(&cfg, 1, gp[1]));
        }
        nes_run_frame(&nes);
        frame++;
        if (vjoy) {
            int k;
            for (k = 0; k < SELFTEST_STEPS; k++)
                if (frame > selftest[k].start && frame <= selftest[k].start + 10 && nes.ram[0x30] == selftest[k].expect)
                    seen[k] = 1;
        }

        apu_render(&nes.apu, nes.cpu.cycles);
        {
            size_t n = apu_take(&nes.apu);
            if (audio && n)
                SDL_PutAudioStreamData(audio, samples, (int)(n * sizeof(float)));
            apu_set_buffer(&nes.apu, samples, sizeof samples / sizeof samples[0]);
        }
        screen_to_rgb(&screen, pixels, PPU_W);
        video_present(&video, pixels, lines, overlay(&rb, &cfg, 0, lines));

        if (max_frames >= 0 && frame >= max_frames)
            quit = 1;
    }

    SDL_Log("ran %ld frames", frame);
    if (vjoy) {
        int k, ok = 1;
        for (k = 0; k < SELFTEST_STEPS; k++) {
            printf("%s: gamepad %s -> game input byte $%02X\n", seen[k] ? "ok  " : "FAIL",
                   SDL_GetGamepadStringForButton(selftest[k].button), selftest[k].expect);
            ok &= seen[k];
        }
        SDL_CloseJoystick(vjoy);
        if (!ok)
            quit = 2;
    }
    for (i = 0; i < 2; i++)
        if (gp[i])
            SDL_CloseGamepad(gp[i]);
    if (audio)
        SDL_DestroyAudioStream(audio);
    video_close(&video);
    fm2_free(&movie);
    SDL_Quit();
    return quit == 2 ? 1 : 0;
}
