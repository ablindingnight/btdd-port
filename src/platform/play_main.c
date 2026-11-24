/* btdd_play: the game in a window, running on the reference interpreter (milestone M2).
 *
 *   btdd_play [ROM] [--movie X.fm2] [--frames N] [--ppu old|new] [--scale S] [--fullscreen] [--no-audio]
 *             [--dump-ram out.bin] [--selftest-gamepad] [--selftest-states]
 *
 * Keys (defaults, changeable in btdd.cfg or with F5): player 1 arrows, X = A, Z = B, Right Shift = Select,
 * Enter = Start; player 2 W/A/S/D, H = A, G = B, T = Select, Y = Start. Gamepads: the first two are
 * players 1 and 2 (east button = A, south button = B, Back = Select, Start, d-pad or left stick).
 * Hotkeys: Esc quit, P pause, F1 reset (Ctrl+F1 power cycle), F2 square pixels / 4:3, F3 filter
 * (nearest / sharp / bilinear), F4 scanlines, F5 redefine keys, F11 or Alt+Enter fullscreen, F12 screenshot.
 * Mods (M7, docs/M7_MODS.md): F8 no sprite flicker, F9 starting lives (mod_* keys in btdd.cfg, --mod ID[=N]).
 * States (M8): F6 save / F7 load the current slot, 0-9 or Shift+F6 / Shift+F7 pick the slot (10 slots, in
 * <settings folder>\states\<ROM SHA-1>); hold Backspace to rewind (rewind_seconds of history, sound muted),
 * hold Tab to fast-forward (ff_speed x; one frame's sound in ff_speed is played, so pacing stays on the sound
 * clock). Backspace, Tab and 0-9 yield to the player's key bindings if bound.
 * --dump-ram writes RAM after every newly emulated frame (tools/ramdiff.py format); --selftest-states
 * scripts save / rewind / load / fast-forward over a movie (see states_script).
 * --movie plays an fm2 instead of live input (--frames N quits after N frames, for automated checks);
 * --selftest-gamepad checks the gamepad path with a virtual pad.
 *
 * Timing: the emulator produces one frame (60.0988 Hz) whenever the audio queue holds less than ~3 frames
 * of sound, so sound never starves and the picture follows the sound card's clock. Without an audio
 * device it falls back to a timer. */
#include "config.h"
#include "nes/apu.h"
#include "nes/nes.h"
#include "nes/state.h"
#include "mods/mods.h"
#include "platform/png.h"
#include "platform/rewind.h"
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
    mods_attach(&nes);
}

static void say(const char *text);

/* F8 / F9: toggle a mod, apply it at once, keep it in btdd.cfg */
static void toggle_mod(const char *id, Config *cfg, int have_cfg)
{
    int i = mods_find(id);
    char text[160], list[120];
    if (i < 0)
        return;
    mods_enable(i, !mods_enabled(i));
    mods_attach(&nes);
    screen_set_no_sprite_limit(mods_display_flags() & MODS_DISPLAY_NO_SPRITE_LIMIT);
    if (mods_def(i)->value_min != mods_def(i)->value_max)
        snprintf(text, sizeof text, "%s (%d): %s", mods_def(i)->name, mods_value(i), mods_enabled(i) ? "on" : "off");
    else
        snprintf(text, sizeof text, "%s: %s", mods_def(i)->name, mods_enabled(i) ? "on" : "off");
    say(text);
    SDL_Log("mods: %s", mods_describe(list, sizeof list));
    if (have_cfg)
        config_save(cfg);
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

/* ---- save states (F6/F7, 10 slots) and rewind (Backspace) ----
 * Slot files: <settings folder>\states\<ROM SHA-1>\slotN.state (nes/state.h format). Rewind keeps one
 * blob per emulated frame (machine state + displayed picture + frame number) as compressed deltas. */

static Rewind rw;
static uint8_t *rw_blob;
static int rw_on;
static char states_dir[1100];

static size_t blob_size(void)
{
    return nes_state_size() + sizeof(Screen) + 8;
}

static void blob_capture(uint8_t *b, long frame)
{
    int64_t f = frame;
    nes_state_capture(&nes, b);
    memcpy(b + nes_state_size(), &screen, sizeof screen);
    memcpy(b + nes_state_size() + sizeof screen, &f, 8);
}

static long blob_restore(const uint8_t *b)
{
    int64_t f;
    nes_state_restore(&nes, b);
    mods_forget_pending(); /* the routine frames of pending after-hooks are gone */
    memcpy(&screen, b + nes_state_size(), sizeof screen);
    memcpy(&f, b + nes_state_size() + sizeof screen, 8);
    return (long)f;
}

static void rewind_setup(const Config *cfg)
{
    rw_on = 0;
    if (cfg->rewind_seconds <= 0)
        return;
    rw_blob = malloc(blob_size());
    /* gameplay averages ~17 KB per frame; allow 1.5 MB per second of history */
    if (rw_blob && rewind_init(&rw, blob_size(), cfg->rewind_seconds * 60 + 1,
                               (size_t)cfg->rewind_seconds * 1536 * 1024) == 0)
        rw_on = 1;
    else
        SDL_Log("rewind disabled: out of memory");
}

static void rewind_record(long frame)
{
    if (!rw_on)
        return;
    blob_capture(rw_blob, frame);
    rewind_push(&rw, rw_blob);
}

/* <folder of btdd.cfg>\states\<sha1> */
static void states_setup(const Config *cfg, const char *sha1)
{
    char *slash;
    snprintf(states_dir, sizeof states_dir, "%s", cfg->path);
    slash = strrchr(states_dir, '\\');
    if (!slash || (strrchr(states_dir, '/') && strrchr(states_dir, '/') > slash))
        slash = strrchr(states_dir, '/');
    if (!slash) {
        states_dir[0] = 0;
        return;
    }
    snprintf(slash + 1, sizeof states_dir - (size_t)(slash + 1 - states_dir), "states/%.40s", sha1);
}

static void slot_path(int slot, char *out, size_t len)
{
    snprintf(out, len, "%s/slot%d.state", states_dir, slot);
}

static void slot_save(int slot, long frame, const char *sha1, char *msg, size_t len)
{
    char path[1200];
    int r;
    if (!states_dir[0] || !SDL_CreateDirectory(states_dir)) {
        snprintf(msg, len, "cannot create the states folder");
        return;
    }
    slot_path(slot, path, sizeof path);
    r = nes_state_save(&nes, (uint64_t)frame, sha1, path);
    if (r == NES_STATE_OK)
        snprintf(msg, len, "saved slot %d", slot);
    else
        snprintf(msg, len, "slot %d not saved: %s", slot, nes_state_error(r));
    SDL_Log("%s (%s)", msg, path);
}

static int slot_load(int slot, long *frame, const char *sha1, char *msg, size_t len)
{
    char path[1200];
    uint64_t f = 0;
    int r;
    slot_path(slot, path, sizeof path);
    r = states_dir[0] ? nes_state_load(&nes, &f, sha1, path) : NES_STATE_IO;
    if (r == NES_STATE_OK) {
        *frame = (long)f;
        mods_forget_pending();
        snprintf(msg, len, "loaded slot %d", slot);
    } else if (r == NES_STATE_IO) {
        snprintf(msg, len, "slot %d is empty", slot);
    } else {
        snprintf(msg, len, "slot %d: %s", slot, nes_state_error(r));
    }
    return r == NES_STATE_OK;
}

static void slot_describe(int slot, char *msg, size_t len)
{
    char path[1200];
    SDL_PathInfo info;
    slot_path(slot, path, sizeof path);
    if (states_dir[0] && SDL_GetPathInfo(path, &info))
        snprintf(msg, len, "slot %d (used)", slot);
    else
        snprintf(msg, len, "slot %d (empty)", slot);
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

/* hotkeys that could collide with a player's binding (Backspace, Tab, 0-9) yield to the binding */
static int key_is_bound(const Config *cfg, SDL_Scancode sc)
{
    int p, b;
    for (p = 0; p < 2; p++)
        for (b = 0; b < BTN_COUNT; b++)
            if (cfg->keys[p][b] == sc)
                return 1;
    return 0;
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
    int repeat = !strcmp(message, text) && SDL_GetTicksNS() < message_until;
    SDL_strlcpy(message, text, sizeof message);
    message_until = SDL_GetTicksNS() + 2500000000ull;
    if (!repeat) /* status lines repeated every frame (rewind, fast forward) are logged once */
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

/* --selftest-states (with --movie, unpaced): the same functions as the hotkeys, scripted. Save slot 9 at
 * frame 3000, play to 3600, rewind 300 frames (RAM must equal frame 3300's), play to 3500, load slot 9 (RAM
 * must equal frame 3000's), then the rest of the movie in fast-forward steps; --dump-ram then lets
 * tools/ramdiff.py check the whole run against FCEUX. Slots go to states-selftest\, not the user's slots.
 * Returns the next phase (4 = done). */
static int states_script(int phase, long *frame, const char *sha1, uint8_t *ram_a, uint8_t *ram_r, int *failed,
                         const Config *cfg)
{
    char msg[200];
    long k;
    (void)cfg;
    if (phase == 0 && *frame == 3000) {
        slot_save(9, *frame, sha1, msg, sizeof msg);
        printf("%s: %s\n", strstr(msg, "not") ? "FAIL" : "ok  ", msg);
        *failed |= strstr(msg, "not") != NULL;
        memcpy(ram_a, nes.ram, 0x800);
        return 1;
    }
    if (phase == 1 && *frame == 3300)
        memcpy(ram_r, nes.ram, 0x800);
    if (phase == 1 && *frame == 3600) {
        for (k = 0; k < 300 && rw_on && rewind_pop(&rw, rw_blob); k++)
            *frame = blob_restore(rw_blob);
        k = *frame == 3300 && !memcmp(nes.ram, ram_r, 0x800);
        printf("%s: rewind 300 frames from 3600 -> frame %ld, RAM %s\n", k ? "ok  " : "FAIL", *frame,
               k ? "equals frame 3300's" : "differs");
        *failed |= !k;
        return 2;
    }
    if (phase == 2 && *frame == 3500) {
        k = slot_load(9, frame, sha1, msg, sizeof msg) && *frame == 3000 && !memcmp(nes.ram, ram_a, 0x800);
        printf("%s: %s -> frame %ld, RAM %s\n", k ? "ok  " : "FAIL", msg, *frame,
               k ? "equals frame 3000's" : "differs");
        *failed |= !k;
        return 4;
    }
    return phase;
}

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
    const char *movie_path = NULL, *dump_path = NULL;
    long max_frames = -1, frame = 0, dumped = 0;
    int i, quit = 0, paused = 0, have_cfg, no_audio = 0, selftest_on = 0, seen[SELFTEST_STEPS] = {0};
    int slot = 0, states_test = 0, st_phase = 0, st_fail = 0, rewinding = 0, ff = 0, step, steps;
    static uint8_t st_ram_a[0x800], st_ram_r[0x800];
    FILE *dump = NULL;
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
        else if (!strcmp(argv[i], "--selftest-states"))
            states_test = 1;
        else if (!strcmp(argv[i], "--dump-ram") && i + 1 < argc)
            dump_path = argv[++i];
        else if (!strcmp(argv[i], "--mod") && i + 1 < argc) {
            if (mods_apply_arg(argv[++i]) != 0) {
                snprintf(msg, sizeof msg, "unknown mod: %s", argv[i]);
                return fail(msg);
            }
        }
        else if (!strcmp(argv[i], "--no-audio"))
            no_audio = 1;
        else if (!strcmp(argv[i], "--fullscreen"))
            cfg.fullscreen = 1;
        else if (argv[i][0] != '-')
            snprintf(cfg.rom, sizeof cfg.rom, "%s", argv[i]);
        else
            return fail("usage: btdd_play [ROM] [--movie X.fm2] [--frames N] [--ppu old|new] [--scale S] "
                        "[--fullscreen] [--no-audio] [--dump-ram out.bin] [--mod ID[=N]] [--selftest-gamepad] "
                        "[--selftest-states]");
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
    screen_set_no_sprite_limit(mods_display_flags() & MODS_DISPLAY_NO_SPRITE_LIMIT);
    {
        char list[120];
        SDL_Log("mods: %s", mods_describe(list, sizeof list));
    }

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

    if (states_test && !movie.frames)
        return fail("--selftest-states needs --movie");
    if (dump_path && !(dump = fopen(dump_path, "wb")))
        return fail("cannot write the --dump-ram file");
    states_setup(&cfg, hash);
    if (states_test) { /* never touch the user's slots: <folder>\states-selftest\<sha1> */
        char *p = strstr(states_dir, "states/");
        if (p) {
            char tail[64];
            snprintf(tail, sizeof tail, "%s", p + 7);
            snprintf(p, sizeof states_dir - (size_t)(p - states_dir), "states-selftest/%s", tail);
        }
        if (cfg.rewind_seconds < 10)
            cfg.rewind_seconds = 10;
    }
    rewind_setup(&cfg);
    power_on(cfg.ppu_new ? PPU_MODEL_NEW : PPU_MODEL_OLD);
    rewind_record(0);
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
                } else if (ev.key.key == SDLK_F8) {
                    toggle_mod("nolimit", &cfg, have_cfg);
                } else if (ev.key.key == SDLK_F9) {
                    toggle_mod("lives", &cfg, have_cfg);
                } else if (ev.key.key == SDLK_F12) {
                    save_screenshot(&cfg);
                } else if (ev.key.key == SDLK_F6 || ev.key.key == SDLK_F7) {
                    int is_f6 = ev.key.key == SDLK_F6;
                    if (ev.key.mod & SDL_KMOD_SHIFT) { /* Shift+F6 / Shift+F7: previous / next slot */
                        slot = (slot + (is_f6 ? 9 : 1)) % 10;
                        slot_describe(slot, msg, sizeof msg);
                    } else if (is_f6) {
                        slot_save(slot, frame, hash, msg, sizeof msg);
                    } else if (slot_load(slot, &frame, hash, msg, sizeof msg)) {
                        rewind_record(frame);
                        if (audio)
                            SDL_ClearAudioStream(audio);
                    }
                    say(msg);
                } else if (ev.key.key >= SDLK_0 && ev.key.key <= SDLK_9 && !(ev.key.mod & SDL_KMOD_ALT) &&
                           !key_is_bound(&cfg, ev.key.scancode)) {
                    slot = (int)(ev.key.key - SDLK_0);
                    slot_describe(slot, msg, sizeof msg);
                    say(msg);
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
        {
            const bool *keys = SDL_GetKeyboardState(NULL);
            int was_rewinding = rewinding, was_ff = ff;
            rewinding = !states_test && rw_on && keys[SDL_SCANCODE_BACKSPACE] &&
                        !key_is_bound(&cfg, SDL_SCANCODE_BACKSPACE);
            ff = !states_test && keys[SDL_SCANCODE_TAB] && !key_is_bound(&cfg, SDL_SCANCODE_TAB);
            if (rewinding != was_rewinding)
                SDL_Log("rewind %s at frame %ld", rewinding ? "start" : "stop", frame);
            if (ff != was_ff)
                SDL_Log("fast forward %s at frame %ld", ff ? "start" : "stop", frame);
        }

        if (rewinding) { /* one step back per frame period, sound muted */
            Uint64 now = SDL_GetTicksNS();
            if (audio)
                SDL_ClearAudioStream(audio);
            if (now < next_tick) {
                SDL_Delay(1);
                continue;
            }
            next_tick = (now - next_tick > 100000000u) ? now : next_tick + (Uint64)(1e9 / FRAME_HZ);
            if (rewind_pop(&rw, rw_blob)) {
                frame = blob_restore(rw_blob);
                say("<< rewind");
            } else {
                say("<< rewind: start of the history");
            }
            screen_to_rgb(&screen, pixels, PPU_W);
            video_present(&video, pixels, lines, overlay(&rb, &cfg, 0, lines));
            continue;
        }

        /* pacing: keep about QUEUE_TARGET_FRAMES of audio queued (fast-forward: ff_speed frames per frame of
         * sound, so the sound card still sets the pace and only one frame's sound in ff_speed is heard) */
        if (states_test) {
            run_frame = 1;
        } else if (audio) {
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
        if (ff)
            say(">> fast forward");

        for (step = 0, steps = ff ? cfg.ff_speed : 1; step < steps; step++) {
            size_t n;
            if (movie.frames) {
                if (frame >= movie.count) {
                    quit = 1;
                    break;
                }
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
                    if (frame > selftest[k].start && frame <= selftest[k].start + 10 &&
                        nes.ram[0x30] == selftest[k].expect)
                        seen[k] = 1;
            }
            apu_render(&nes.apu, nes.cpu.cycles);
            n = apu_take(&nes.apu);
            if (audio && n && step == steps - 1)
                SDL_PutAudioStreamData(audio, samples, (int)(n * sizeof(float)));
            apu_set_buffer(&nes.apu, samples, sizeof samples / sizeof samples[0]);
            rewind_record(frame);
            if (dump && frame > dumped) {
                uint8_t h[4] = {(uint8_t)frame, (uint8_t)(frame >> 8), (uint8_t)(frame >> 16), (uint8_t)(frame >> 24)};
                fwrite(h, 1, 4, dump);
                fwrite(nes.ram, 1, sizeof nes.ram, dump);
                dumped = frame;
            }
            if (states_test)
                st_phase = states_script(st_phase, &frame, hash, st_ram_a, st_ram_r, &st_fail, &cfg);
            if (states_test && st_phase == 4)
                steps = cfg.ff_speed; /* the rest of the movie with fast-forward's frame loop */
        }
        screen_to_rgb(&screen, pixels, PPU_W);
        video_present(&video, pixels, lines, overlay(&rb, &cfg, 0, lines));

        if (max_frames >= 0 && frame >= max_frames)
            quit = 1;
    }

    SDL_Log("ran %ld frames", frame);
    if (dump)
        fclose(dump);
    if (states_test) {
        if (st_phase != 4)
            st_fail = 1;
        printf("%s: states self-test (%s), ended at frame %ld\n", st_fail ? "FAIL" : "ok  ",
               st_phase == 4 ? "all steps ran" : "script did not finish", frame);
        if (st_fail)
            quit = 2;
    }
    if (rw_on)
        rewind_free(&rw);
    free(rw_blob);
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
