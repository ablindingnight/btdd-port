/* btdd_play: the game in a window, running on the reference interpreter (milestone M2).
 *
 *   btdd_play [ROM] [--movie X.fm2] [--frames N] [--ppu old|new] [--scale S] [--fullscreen] [--no-audio]
 *
 * Keys (defaults, changeable in btdd.cfg): player 1 arrows, X = A, Z = B, Right Shift = Select,
 * Enter = Start; player 2 W/A/S/D, H = A, G = B, T = Select, Y = Start. Gamepads: the first two are
 * players 1 and 2 (east button = A, south button = B, Back = Select, Start, d-pad or left stick).
 * Esc quits, P pauses, F11 or Alt+Enter toggles fullscreen, F2 toggles square pixels / 4:3, F12 saves a
 * screenshot in the settings folder. --movie plays an fm2 instead of live input (--frames N quits after
 * N frames, for automated checks). --selftest-gamepad checks the gamepad path with a virtual pad.
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
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define AUDIO_RATE 48000
#define FRAME_HZ (APU_CPU_HZ / 29780.5)
#define QUEUE_TARGET_FRAMES 3

static Rom rom;
static Nes nes;
static Apu apu;
static Screen screen;
static uint8_t watch_all[0x10000];
static uint32_t pixels[PPU_W * PPU_H];
static float samples[AUDIO_RATE / 10];

/* ---- APU attached to the interpreter (same wiring as btdd_apu_check) ---- */

static void on_instruction(void *ctx, Cpu *cpu)
{
    (void)ctx;
    apu_tick(&apu, cpu->cycles);
}

static void on_apu_write(void *user, uint64_t cycle, uint16_t addr, uint8_t value)
{
    (void)user;
    apu_write(&apu, cycle, addr, value);
}

static uint8_t host_read(void *ctx, uint16_t addr)
{
    return cpu_bus_read(&((Nes *)ctx)->cpu, addr);
}

static void host_stall(void *ctx, int cycles)
{
    cpu_add_cycles(&((Nes *)ctx)->cpu, cycles);
}

static void host_irq(void *ctx, int frame_irq, int dmc_irq)
{
    Cpu *c = &((Nes *)ctx)->cpu;
    c->irq = (uint8_t)((c->irq & ~(CPU_IRQ_FRAME | CPU_IRQ_DMC)) | (frame_irq ? CPU_IRQ_FRAME : 0) |
                       (dmc_irq ? CPU_IRQ_DMC : 0));
}

static void power_on(PpuModel model)
{
    ApuHost host = {&nes, host_read, host_stall, host_irq};
    nes_init(&nes, &rom);
    nes_power(&nes, model);
    screen_attach(&screen, &nes.ppu);
    apu_power(&apu, AUDIO_RATE, &host);
    apu_set_buffer(&apu, samples, sizeof samples / sizeof samples[0]);
    nes.apu_hook = on_apu_write;
    memset(watch_all, 1, sizeof watch_all);
    nes.cpu.exec_watch = watch_all;
    nes.cpu.exec_hook = on_instruction;
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

/* ---- video ---- */

static void apply_presentation(SDL_Renderer *r, const Config *cfg)
{
    if (cfg->aspect)
        SDL_SetRenderLogicalPresentation(r, 292, 240, SDL_LOGICAL_PRESENTATION_LETTERBOX);
    else
        SDL_SetRenderLogicalPresentation(r, PPU_W, PPU_H, SDL_LOGICAL_PRESENTATION_INTEGER_SCALE);
}

static void save_screenshot(const Config *cfg)
{
    char path[1100], *slash;
    static int n;
    snprintf(path, sizeof path, "%s", cfg->path);
    slash = strrchr(path, '\\');
    if (!slash)
        slash = strrchr(path, '/');
    if (!slash)
        return;
    snprintf(slash + 1, sizeof path - (size_t)(slash + 1 - path), "shot_%llu_%d.png",
             (unsigned long long)nes.ppu.frame, n++);
    if (png_write_rgb(path, pixels, PPU_W, PPU_H, PPU_W) == 0)
        SDL_Log("screenshot: %s", path);
}

/* --selftest-gamepad: a virtual SDL gamepad presses buttons; the game's own input byte ($30, player 1
 * buttons held, bit order A=$80 B=$40 Select=$20 Start=$10 Up=$08 Down=$04 Left=$02 Right=$01) must show
 * them. Exercises SDL gamepad detection, the mapping and the controller port. */
typedef struct SelfTest {
    long start;               /* frame the step starts */
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
    const char *movie_path = NULL;
    long max_frames = -1, frame = 0;
    int i, quit = 0, paused = 0, have_cfg, no_audio = 0, selftest_on = 0, seen[SELFTEST_STEPS] = {0};
    SDL_Joystick *vjoy = NULL;
    char hash[41], msg[1400];
    Fm2 movie = {0};
    SDL_Window *win;
    SDL_Renderer *ren;
    SDL_Texture *tex;
    SDL_AudioStream *audio;
    SDL_AudioSpec spec = {SDL_AUDIO_F32, 1, AUDIO_RATE};
    SDL_Gamepad *gp[2] = {NULL, NULL};
    Uint64 next_tick;
    RomResult rr;

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
                        "[--fullscreen]");
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

    win = SDL_CreateWindow("Battletoads & Double Dragon", (cfg.aspect ? 292 : PPU_W) * cfg.scale,
                           PPU_H * cfg.scale, SDL_WINDOW_RESIZABLE | (cfg.fullscreen ? SDL_WINDOW_FULLSCREEN : 0));
    ren = win ? SDL_CreateRenderer(win, NULL) : NULL;
    tex = ren ? SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, PPU_W, PPU_H) : NULL;
    if (!tex) {
        snprintf(msg, sizeof msg, "cannot open the window: %s", SDL_GetError());
        return fail(msg);
    }
    SDL_SetTextureScaleMode(tex, SDL_SCALEMODE_NEAREST);
    apply_presentation(ren, &cfg);
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
        int run_frame;
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
                if (ev.key.repeat)
                    break;
                if (ev.key.key == SDLK_ESCAPE)
                    quit = 1;
                else if (ev.key.key == SDLK_P) {
                    paused = !paused;
                    SDL_SetWindowTitle(win, paused ? "Battletoads & Double Dragon (paused)"
                                                   : "Battletoads & Double Dragon");
                    if (audio)
                        SDL_ClearAudioStream(audio);
                    next_tick = SDL_GetTicksNS();
                } else if (ev.key.key == SDLK_F11 ||
                           (ev.key.key == SDLK_RETURN && (ev.key.mod & SDL_KMOD_ALT))) {
                    cfg.fullscreen = !cfg.fullscreen;
                    SDL_SetWindowFullscreen(win, cfg.fullscreen);
                } else if (ev.key.key == SDLK_F2) {
                    cfg.aspect = !cfg.aspect;
                    apply_presentation(ren, &cfg);
                } else if (ev.key.key == SDLK_F12) {
                    save_screenshot(&cfg);
                }
                break;
            default:
                break;
            }
        }
        if (paused) {
            SDL_Delay(10);
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

        apu_render(&apu, nes.cpu.cycles);
        {
            size_t n = apu_take(&apu);
            if (audio && n)
                SDL_PutAudioStreamData(audio, samples, (int)(n * sizeof(float)));
            apu_set_buffer(&apu, samples, sizeof samples / sizeof samples[0]);
        }
        screen_to_rgb(&screen, pixels, PPU_W);
        SDL_UpdateTexture(tex, NULL, pixels, PPU_W * (int)sizeof(uint32_t));
        SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
        SDL_RenderClear(ren);
        SDL_RenderTexture(ren, tex, NULL, NULL);
        SDL_RenderPresent(ren);

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
    SDL_DestroyTexture(tex);
    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    fm2_free(&movie);
    SDL_Quit();
    return quit == 2 ? 1 : 0;
}
