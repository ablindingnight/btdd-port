/* btdd_play: the game in a window, running on the reference interpreter (milestone M2).
 *
 *   btdd_play [ROM] [--movie X.fm2] [--frames N] [--ppu old|new] [--scale S] [--fullscreen] [--no-audio]
 *             [--dump-ram out.bin] [--selftest-gamepad] [--selftest-states]
 *
 * Keys (defaults, changeable in btdd.cfg or with F5): player 1 arrows, X = A, Z = B, Right Shift = Select,
 * Enter = Start; player 2 W/A/S/D, H = A, G = B, T = Select, Y = Start. Gamepads: the first two are
 * players 1 and 2 (east button = A, south button = B, Back = Select, Start, d-pad or left stick).
 * Hotkeys: Esc settings menu (gamepad: Guide, or Back+Start), P pause, F1 reset (Ctrl+F1 power cycle), F2 square pixels / 4:3, F3 filter
 * (nearest / sharp / bilinear), F4 scanlines, F5 redefine keys, F11 or Alt+Enter fullscreen, F12 screenshot.
 * Mods (M7, docs/M7_MODS.md): F8 no sprite flicker, F9 starting lives, F10 widescreen (M9) (mod_* keys in btdd.cfg, --mod ID[=N]).
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
#include "net/netplay.h"
#include "platform/menu.h"
#include "platform/udp.h"
#include "platform/upnp.h"
#include "net/joincode.h"
#include "platform/png.h"
#include "platform/rewind.h"
#include "platform/screen.h"
#include "platform/wide.h"
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
static uint32_t pixels[WIDE_W * PPU_H]; /* 256 wide, or WIDE_W with the widescreen mod */
static int pixels_w = PPU_W;
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
/* The finished frame as RGB in `pixels`: the game's 256x240 picture, or the 424x240 one with the widescreen mod
 * (M9, src/platform/wide.c) when the video path supports it. */
static void make_picture(Video *v)
{
    static WideState ws;
    static uint8_t widx[PPU_H][WIDE_W];
    int f = mods_display_flags();
    if ((f & MODS_DISPLAY_WIDESCREEN) && video_set_width(v, WIDE_W) == 0) {
        ws.sprites = (f & MODS_DISPLAY_WIDE_SPRITES) != 0;
        ws.hud = (f & MODS_DISPLAY_WIDE_HUD) != 0;
        wide_render(&ws, &nes, &screen, widx);
        wide_to_rgb(&screen, widx, pixels, WIDE_W, (f & MODS_DISPLAY_WIDE_FADE) != 0);
        pixels_w = WIDE_W;
    } else {
        video_set_width(v, PPU_W);
        screen_to_rgb(&screen, pixels, PPU_W);
        pixels_w = PPU_W;
    }
}

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
    if (png_write_rgb(path, pixels, pixels_w, PPU_H, pixels_w) == 0) {
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

/* ---- M13 online 2-player (src/net/netplay.c over src/platform/udp.c; docs/M13_NETPLAY.md) ---- */

static NetSession net;
static Udp *net_udp;
static int net_on;
static SDL_AudioStream *net_audio;
static FILE *net_dump; /* --dump-ram while online: RAM after every confirmed frame */
static long net_dumped;
static char net_last_msg[160];
static int net_logged_desync = -1;

/* one frame for the netplay session; re-simulated frames (rollback) make no sound */
static void net_frame(void *ctx, Nes *n, uint8_t p1, uint8_t p2, int resim)
{
    size_t k;
    (void)ctx;
    nes_set_pads(n, p1, p2);
    nes_run_frame(n);
    apu_render(&n->apu, n->cpu.cycles);
    k = apu_take(&n->apu);
    if (!resim && net_audio && k)
        SDL_PutAudioStreamData(net_audio, samples, (int)(k * sizeof(float)));
    apu_set_buffer(&n->apu, samples, sizeof samples / sizeof samples[0]);
}

static void net_confirmed(void *ctx, uint32_t f, const uint8_t *ram)
{
    (void)ctx;
    if (net_dump && (long)f + 1 > net_dumped) {
        uint8_t h[4] = {(uint8_t)(f + 1), (uint8_t)((f + 1) >> 8), (uint8_t)((f + 1) >> 16), (uint8_t)((f + 1) >> 24)};
        fwrite(h, 1, 4, net_dump);
        fwrite(ram, 1, 0x800, net_dump);
        net_dumped = (long)f + 1;
    }
}

/* What both sides must share: protocol, core layout, PPU model, CPU path, logic mods and cheats, ROM. Display
 * settings (widescreen, shaders, no flicker) may differ. */
static void net_summary(const Config *cfg, const char *sha1_hex, char *out, size_t len)
{
    char mods[48] = "none";
#ifdef BTDD_HAVE_MODS
    {
        int i;
        size_t n = 0;
        for (i = 0; i < mods_count() && n + 16 < sizeof mods; i++)
            if (mods_enabled(i) && mods_def(i)->kind == MOD_LOGIC)
                n += (size_t)snprintf(mods + n, sizeof mods - n, "%s%.12s=%d", n ? "," : "", mods_def(i)->id,
                                      mods_value(i));
        if (!n)
            snprintf(mods, sizeof mods, "none");
    }
#endif
    snprintf(out, len, "p%d core %08lx ppu %s cpu interp mods %s rom %.12s", NET_PROTOCOL,
             (unsigned long)nes_state_fingerprint(), cfg->ppu_new ? "new" : "old", mods, sha1_hex);
}

static void net_stop(void)
{
    if (!net_on)
        return;
    net_free(&net);
    udp_close(net_udp);
    net_udp = NULL;
    net_on = 0;
    upnp_unmap(); /* close the router port we opened, if any */
}

/* Connection helpers that run every frame until connected: STUN (our public endpoint -> our join code), punching
 * towards the other side's endpoints, and plain-language help when nothing answers. */
static char net_code[JOINCODE_LEN];   /* our join code, once known */
static char net_upnp[160];            /* UPnP result line */
static char net_upnp_ip[40];
static int net_mode;                  /* 0 direct join, 1 host, 2 loopback host (tests), 3 join by code */
static Uint64 net_started_ns;
static int net_help_shown;

static void net_make_code(const Config *cfg)
{
    JoinEndpoints e;
    uint32_t ip = 0;
    uint16_t port = 0;
    memset(&e, 0, sizeof e);
    if (udp_local_endpoint(net_udp, &e.local_ip, &e.local_port) != 0)
        return;
    if (udp_stun_result(net_udp, &ip, &port)) {
        e.public_ip = ip;
        e.public_port = port;
    } else if (net_upnp_ip[0] && SDL_GetTicksNS() - net_started_ns > 3000000000ull) { /* STUN silent: UPnP's */
        unsigned a, b, c2, d;
        if (sscanf(net_upnp_ip, "%u.%u.%u.%u", &a, &b, &c2, &d) == 4)
            e.public_ip = a << 24 | b << 16 | c2 << 8 | d;
        e.public_port = (uint16_t)cfg->net_port;
    } else if (SDL_GetTicksNS() - net_started_ns < 4000000000ull) {
        return; /* wait a little for STUN */
    }
    joincode_encode(&e, net_code);
    SDL_SetClipboardText(net_code);
    SDL_Log("netplay: our join code %s (copied to the clipboard)%s", net_code,
            e.public_ip ? "" : "; no public address found: only works on the same network");
}

static void net_conn_tick(const Config *cfg)
{
    if (!net_on || !net_udp)
        return;
    udp_stun_poll(net_udp);
    if (!udp_peer_known(net_udp))
        udp_punch(net_udp);
    if (!net_code[0] && net_mode != 0 && net_mode != 2)
        net_make_code(cfg);
    if (net.state == NET_HANDSHAKE && !net_help_shown && SDL_GetTicksNS() - net_started_ns > 20000000000ull) {
        net_help_shown = 1;
        if (net.local == 1)
            snprintf(net.message, sizeof net.message,
                     "no answer from the host yet: ask them to paste YOUR join code (Online menu), or use Tailscale /"
                     " ZeroTier, or forward UDP port %d on their router", cfg->net_port);
        else
            snprintf(net.message, sizeof net.message,
                     "nobody connected yet: send your join code to player 2 (and paste theirs if they send one back)");
    }
}

/* mode 1: host on cfg->net_port (UPnP + join code), 2: host on 127.0.0.1 only (tests), 0: join the address `addr`,
 * 3: join with the join code `addr` */
static int net_start(int mode, const char *addr, const Config *cfg, const char *sha1_hex, char *msg, size_t len)
{
    char summary[NET_SUMMARY], err[160];
    JoinEndpoints e = {0};
    int host = mode == 1 || mode == 2;
    net_stop();
    net_code[0] = 0;
    net_upnp[0] = net_upnp_ip[0] = 0;
    net_help_shown = 0;
    if (mode == 3 && joincode_decode(addr, &e) != 0) {
        snprintf(msg, len, "that is not a join code (they look like BT1-XXXXX-XXXXX-XXXXX-XXXXXX)");
        return -1;
    }
    if (host)
        net_udp = udp_host(cfg->net_port, mode == 2, msg, len);
    else if (mode == 3)
        net_udp = udp_open(0, 0, msg, len);
    else
        net_udp = udp_join(addr, cfg->net_port, msg, len);
    if (!net_udp)
        return -1;
    if (mode == 3) {
        udp_add_candidate(net_udp, e.public_ip, e.public_port);
        udp_add_candidate(net_udp, e.local_ip, e.local_port);
    }
    if (mode == 1) /* ask the router to forward our port (often off; then the join code / a VPN) */
        upnp_map(cfg->net_port, net_upnp_ip, sizeof net_upnp_ip, net_upnp, sizeof net_upnp);
    if ((mode == 1 || mode == 3) && cfg->net_stun[0] && udp_stun_start(net_udp, cfg->net_stun, err, sizeof err) != 0)
        SDL_Log("netplay: %s", err);
    power_on(cfg->ppu_new ? PPU_MODEL_NEW : PPU_MODEL_OLD); /* both sides start from power-on */
    net_summary(cfg, sha1_hex, summary, sizeof summary);
    if (net_init(&net, &nes, host ? 0 : 1, cfg->net_delay, summary, udp_transport(net_udp), net_frame, net_confirmed,
                 NULL) != 0) {
        udp_close(net_udp);
        snprintf(msg, len, "out of memory");
        return -1;
    }
    net_on = 1;
    net_mode = mode;
    net_started_ns = SDL_GetTicksNS();
    net_last_msg[0] = 0;
    if (host)
        snprintf(msg, len, "hosting on UDP port %d: %s", cfg->net_port,
                 net_upnp[0] ? net_upnp : "waiting for player 2");
    else if (mode == 3)
        snprintf(msg, len, "trying the join code...");
    else
        snprintf(msg, len, "joining %s ...", addr);
    SDL_Log("netplay: %s (%s)", msg, summary);
    return 0;
}

/* the host pastes player 2's code: also send towards them (punching through the host's own NAT) */
static int net_add_peer_code(const char *code, char *msg, size_t len)
{
    JoinEndpoints e;
    if (!net_on || joincode_decode(code, &e) != 0) {
        snprintf(msg, len, "that is not a join code");
        return -1;
    }
    udp_add_candidate(net_udp, e.public_ip, e.public_port);
    udp_add_candidate(net_udp, e.local_ip, e.local_port);
    snprintf(msg, len, "trying player 2's join code...");
    return 0;
}

/* ---- settings overlay (Esc, or Guide / Back+Start on a gamepad): src/platform/menu.c pages built from the live
 * state; changes apply at once and are saved to btdd.cfg when the menu closes ---- */

enum {
    PG_MAIN, PG_STATES, PG_DISPLAY, PG_GRAPHICS, PG_AUDIO, PG_CONTROLS, PG_KEYS1, PG_KEYS2, PG_GAMEPLAY, PG_MODS, PG_CHEATS, PG_NET,
};
enum {
    ID_RESUME = 1, ID_RESET, ID_POWER, ID_QUIT, ID_SLOT, ID_SAVE, ID_LOAD, ID_SCALE, ID_FULLSCREEN, ID_ASPECT,
    ID_FILTER, ID_SCANLINES, ID_VIDEO, ID_SHADER, ID_VOLUME, ID_REBIND_ALL, ID_REWIND, ID_FF, ID_BACK,
    ID_NET_HOST, ID_NET_ADDR, ID_NET_JOIN, ID_NET_DELAY, ID_NET_LEAVE, ID_NET_RESYNC, ID_NET_JOINCODE,
    ID_NET_COPY, ID_NET_PEERCODE,
    ID_KEY_BASE = 100, /* + player * BTN_COUNT + button */
    ID_MOD_BASE = 200, /* + mod index */
};

typedef struct PlayCtx {
    Config *cfg;
    Video *video;
    SDL_AudioStream *audio;
    SDL_Gamepad **gp;
    Rebind *rb;
    int *slot, *quit;
    long *frame;
    const char *hash;
    int have_cfg, dirty;
    int capture; /* key being rebound: ID_KEY_BASE + ..., 0 = none */
    int text_edit; /* typing the join address */
    char status[64];
} PlayCtx;

static Menu menu;
static const int rewind_choices[] = {0, 10, 30, 60, 120, 300};

static const char *onoff(int v)
{
    return v ? "on" : "off";
}

static void add_back(Menu *m)
{
    menu_add(m, ITEM_INFO, 0, "", "", "");
    menu_add(m, ITEM_ACTION, ID_BACK, "Back", "", "Esc / B: back");
}

static void build_page(Menu *m, PlayCtx *c)
{
    char v[32], h[64];
    int i;
    switch (m->page) {
    case PG_STATES:
        menu_begin(m, "SAVE STATES");
        slot_describe(*c->slot, v, sizeof v);
        menu_add(m, ITEM_CHOICE, ID_SLOT, "Slot", v + 5, "left/right picks the slot (also 0-9 in game)");
        menu_add(m, ITEM_ACTION, ID_SAVE, "Save to this slot", "", "F6 in game");
        menu_add(m, ITEM_ACTION, ID_LOAD, "Load this slot", "", "F7 in game");
        menu_add(m, ITEM_INFO, 0, "Hold Backspace: rewind", "", "");
        menu_add(m, ITEM_INFO, 0, "Hold Tab: fast forward", "", "");
        add_back(m);
        break;
    case PG_DISPLAY:
        menu_begin(m, "DISPLAY");
        snprintf(v, sizeof v, "%dx", c->cfg->scale);
        menu_add(m, ITEM_CHOICE, ID_SCALE, "Window size", v, "window size in NES pixels (1-10x)");
        menu_add(m, ITEM_CHOICE, ID_FULLSCREEN, "Fullscreen", onoff(c->cfg->fullscreen), "F11 / Alt+Enter in game");
        menu_add(m, ITEM_CHOICE, ID_ASPECT, "Aspect", c->cfg->aspect ? "4:3" : "square",
                 "4:3 = the NES pixel shape (F2)");
        if (!c->video->gpu) {
            menu_add(m, ITEM_CHOICE, ID_FILTER, "Filter", video_filter_names[c->cfg->filter],
                     "nearest / sharp / bilinear (F3)");
            menu_add(m, ITEM_CHOICE, ID_SCANLINES, "Scanlines", onoff(c->cfg->scanlines), "F4 in game");
        } else {
            menu_add(m, ITEM_INFO, 0, "Filters: see Graphics", "", "");
        }
        add_back(m);
        break;
    case PG_GRAPHICS:
        menu_begin(m, "GRAPHICS");
        snprintf(h, sizeof h, "%s",
                 (c->cfg->gpu != (c->video->gpu != NULL)) ? "applies at the next start"
                                                          : "gpu = shader presets (Direct3D 12)");
        menu_add(m, ITEM_CHOICE, ID_VIDEO, "Renderer", c->cfg->gpu ? "gpu" : "sdl", h);
        menu_add(m, ITEM_CHOICE, ID_SHADER, "Shader preset", gpu_preset_names[c->cfg->shader],
                 c->video->gpu ? "F3 in game cycles presets" : "used with renderer = gpu");
        add_back(m);
        break;
    case PG_AUDIO:
        menu_begin(m, "AUDIO");
        snprintf(v, sizeof v, "%d%%", (int)(c->cfg->volume * 100.0f + 0.5f));
        menu_add(m, ITEM_CHOICE, ID_VOLUME, "Volume", v, c->audio ? "" : "no audio device");
        add_back(m);
        break;
    case PG_CONTROLS:
        menu_begin(m, "CONTROLS");
        menu_add(m, ITEM_PAGE, PG_KEYS1, "Player 1 keys", "", "");
        menu_add(m, ITEM_PAGE, PG_KEYS2, "Player 2 keys", "", "");
        menu_add(m, ITEM_ACTION, ID_REBIND_ALL, "Redefine all keys", "", "prompts for every key (F5)");
        for (i = 0; i < 2; i++) {
            snprintf(h, sizeof h, "Pad %d: %.20s", i + 1, c->gp[i] ? SDL_GetGamepadName(c->gp[i]) : "none");
            menu_add(m, ITEM_INFO, 0, h, "", "");
        }
        add_back(m);
        break;
    case PG_KEYS1:
    case PG_KEYS2: {
        int p = m->page == PG_KEYS2;
        menu_begin(m, p ? "PLAYER 2 KEYS" : "PLAYER 1 KEYS");
        for (i = 0; i < BTN_COUNT; i++) {
            int id = ID_KEY_BASE + p * BTN_COUNT + i;
            menu_add(m, ITEM_ACTION, id, button_labels[i],
                     c->capture == id ? "press a key" : SDL_GetScancodeName(c->cfg->keys[p][i]),
                     c->capture == id ? "Esc keeps the old key" : "Enter, then press the new key");
        }
        add_back(m);
        break;
    }
    case PG_NET:
        menu_begin(m, "ONLINE 2-PLAYER");
        if (net_on) {
            net_status(&net, h, sizeof h);
            menu_add(m, ITEM_INFO, 0, h, "", "");
            if (net_upnp[0])
                menu_add(m, ITEM_INFO, 0, net_upnp, "", "");
            if (net.state == NET_HANDSHAKE && net_code[0]) {
                menu_add(m, ITEM_INFO, 0, net_code, "", "");
                menu_add(m, ITEM_ACTION, ID_NET_COPY, "Copy my join code", "", "send it to the other player");
                if (net.local == 0)
                    menu_add(m, ITEM_ACTION, ID_NET_PEERCODE, "Paste player 2's code", "",
                             "if they sent theirs back (strict routers)");
            }
            if (net.state == NET_DESYNC && net.local == 0)
                menu_add(m, ITEM_ACTION, ID_NET_RESYNC, "Resync", "", "send your game to player 2; both continue");
            menu_add(m, ITEM_ACTION, ID_NET_LEAVE, "Leave online game", "", "back to playing alone");
        } else {
            snprintf(v, sizeof v, "port %d", c->cfg->net_port);
            menu_add(m, ITEM_ACTION, ID_NET_HOST, "Host game", v, "you are player 1: send your join code");
            menu_add(m, ITEM_ACTION, ID_NET_JOINCODE, "Join with code", "", "paste the host's join code (clipboard)");
            menu_add(m, ITEM_CHOICE, ID_NET_ADDR, "Address", c->text_edit ? "type, Enter" : c->cfg->net_join,
                     c->text_edit ? "IP or IP:port, then Enter" : "Enter to type the host's IP:port");
            menu_add(m, ITEM_ACTION, ID_NET_JOIN, "Join game", "", "you are player 2");
        }
        snprintf(v, sizeof v, "%d", c->cfg->net_delay);
        menu_add(m, ITEM_CHOICE, ID_NET_DELAY, "Input delay", v, "frames; 1-2 for most connections");
        menu_add(m, ITEM_INFO, 0, "No port forwarding needed: the", "", "");
        menu_add(m, ITEM_INFO, 0, "game tries UPnP and join codes", "", "");
        menu_add(m, ITEM_INFO, 0, "(a STUN server sees your IP).", "", "");
        add_back(m);
        break;
    case PG_GAMEPLAY:
        menu_begin(m, "GAMEPLAY");
        if (c->cfg->rewind_seconds)
            snprintf(v, sizeof v, "%d s", c->cfg->rewind_seconds);
        else
            snprintf(v, sizeof v, "off");
        menu_add(m, ITEM_CHOICE, ID_REWIND, "Rewind length", v, "about 1 MB of memory per second");
        snprintf(v, sizeof v, "%dx", c->cfg->ff_speed);
        menu_add(m, ITEM_CHOICE, ID_FF, "Fast forward", v, "speed while Tab is held");
        add_back(m);
        break;
    case PG_MODS:
    case PG_CHEATS: /* the game's own codes (mods named cheat_*) get their own page */
        menu_begin(m, m->page == PG_CHEATS ? "CHEATS (the game's codes)" : "MODS");
#ifdef BTDD_HAVE_MODS
        for (i = 0; i < mods_count() && i < MENU_MAX_ITEMS - 4; i++) {
            const ModDef *d = mods_def(i);
            if ((strncmp(d->id, "cheat_", 6) == 0) != (m->page == PG_CHEATS))
                continue;
            if (d->value_min != d->value_max && mods_enabled(i))
                snprintf(v, sizeof v, "%d", mods_value(i));
            else
                snprintf(v, sizeof v, "%s", onoff(mods_enabled(i)));
            menu_add(m, ITEM_CHOICE, ID_MOD_BASE + i, d->name, v, d->help);
        }
#else
        menu_add(m, ITEM_INFO, 0, "No mods in this build", "", "");
#endif
        add_back(m);
        break;
    default:
        menu_begin(m, "SETTINGS");
        menu_add(m, ITEM_ACTION, ID_RESUME, "Resume", "", "Esc closes this menu");
        menu_add(m, ITEM_PAGE, PG_STATES, "Save states", "", "");
        menu_add(m, ITEM_PAGE, PG_DISPLAY, "Display", "", "");
        menu_add(m, ITEM_PAGE, PG_GRAPHICS, "Graphics", "", "");
        menu_add(m, ITEM_PAGE, PG_AUDIO, "Audio", "", "");
        menu_add(m, ITEM_PAGE, PG_CONTROLS, "Controls", "", "");
        menu_add(m, ITEM_PAGE, PG_GAMEPLAY, "Gameplay", "", "");
        menu_add(m, ITEM_PAGE, PG_MODS, "Mods", "", "");
        menu_add(m, ITEM_PAGE, PG_CHEATS, "Cheats", "", "the game's own codes, entered for you");
        menu_add(m, ITEM_PAGE, PG_NET, "Online 2-player", "", "play with a friend over the internet");
        menu_add(m, ITEM_INFO, 0, "", "", "");
        menu_add(m, ITEM_ACTION, ID_RESET, "Reset", "", "F1 in game");
        menu_add(m, ITEM_ACTION, ID_POWER, "Power cycle", "", "Ctrl+F1 in game");
        menu_add(m, ITEM_ACTION, ID_QUIT, "Quit", "", "settings are saved");
        if (c->status[0])
            menu_add(m, ITEM_INFO, 0, c->status, "", "");
        break;
    }
}

static int wrap(int v, int lo, int hi)
{
    return v < lo ? hi : v > hi ? lo : v;
}

static void menu_closed(PlayCtx *c)
{
    if (c->dirty && c->have_cfg && config_save(c->cfg) == 0)
        SDL_Log("settings saved: %s", c->cfg->path);
    c->dirty = 0;
    c->capture = 0;
}

static void menu_leave(PlayCtx *c)
{
    menu_close(&menu);
    menu_closed(c);
}

/* One menu event: apply it. */
static void menu_apply(PlayCtx *c, MenuEvent ev, int id, int dir)
{
    Config *cfg = c->cfg;
    int i;
    if (ev == MENU_EV_CLOSE) {
        menu_closed(c);
        return;
    }
    if (ev != MENU_EV_ACTIVATE && ev != MENU_EV_CHANGE)
        return;
    c->status[0] = 0;
    if (id == ID_BACK) {
        int d0, d1;
        MenuEvent back = menu_input(&menu, MENU_BACK, &d0, &d1);
        menu_apply(c, back, d0, d1);
        return;
    }
    if (id >= ID_KEY_BASE && id < ID_KEY_BASE + 2 * BTN_COUNT) {
        c->capture = id;
        return;
    }
#ifdef BTDD_HAVE_MODS
    if (id >= ID_MOD_BASE && id < ID_MOD_BASE + mods_count()) {
        const ModDef *d = mods_def(id - ID_MOD_BASE);
        i = id - ID_MOD_BASE;
        if (d->value_min == d->value_max) {
            mods_enable(i, !mods_enabled(i));
        } else { /* cycle: off, min..max */
            int v = mods_enabled(i) ? mods_value(i) : d->value_min - 1;
            v = wrap(v + dir, d->value_min - 1, d->value_max);
            mods_enable(i, v >= d->value_min);
            if (v >= d->value_min)
                mods_set_value(i, v);
        }
        mods_attach(&nes);
        screen_set_no_sprite_limit(mods_display_flags() & MODS_DISPLAY_NO_SPRITE_LIMIT);
        c->dirty = 1;
        return;
    }
#endif
    switch (id) {
    case ID_RESUME:
        menu_leave(c);
        break;
    case ID_RESET:
        soft_reset();
        menu_leave(c);
        say("reset");
        break;
    case ID_POWER:
        power_on(nes.ppu.model);
        menu_leave(c);
        say("power cycle");
        break;
    case ID_QUIT:
        menu_closed(c);
        *c->quit = 1;
        break;
    case ID_SLOT:
        *c->slot = wrap(*c->slot + dir, 0, 9);
        break;
    case ID_SAVE:
        slot_save(*c->slot, *c->frame, c->hash, c->status, sizeof c->status);
        say(c->status);
        break;
    case ID_LOAD:
        if (slot_load(*c->slot, c->frame, c->hash, c->status, sizeof c->status)) {
            rewind_record(*c->frame);
            if (c->audio)
                SDL_ClearAudioStream(c->audio);
            menu_leave(c);
        }
        say(c->status);
        break;
    case ID_SCALE:
        cfg->scale = wrap(cfg->scale + dir, 1, 10);
        if (!cfg->fullscreen)
            SDL_SetWindowSize(c->video->win, (cfg->aspect ? 292 : 256) * cfg->scale, 240 * cfg->scale);
        c->dirty = 1;
        break;
    case ID_FULLSCREEN:
        cfg->fullscreen = !cfg->fullscreen;
        SDL_SetWindowFullscreen(c->video->win, cfg->fullscreen);
        c->dirty = 1;
        break;
    case ID_ASPECT:
        c->video->aspect = cfg->aspect = !cfg->aspect;
        c->dirty = 1;
        break;
    case ID_FILTER:
        c->video->filter = cfg->filter = wrap(cfg->filter + dir, 0, FILTER_COUNT - 1);
        c->dirty = 1;
        break;
    case ID_SCANLINES:
        c->video->scanlines = cfg->scanlines = !cfg->scanlines;
        c->dirty = 1;
        break;
    case ID_VIDEO:
        cfg->gpu = !cfg->gpu;
        c->dirty = 1;
        break;
    case ID_SHADER:
        cfg->shader = wrap(cfg->shader + dir, 0, GPU_PRESET_COUNT - 1);
        c->video->preset = cfg->shader;
        c->dirty = 1;
        break;
    case ID_VOLUME: {
        int v = (int)(cfg->volume * 10.0f + 0.5f) + dir;
        cfg->volume = (float)SDL_clamp(v, 0, 10) / 10.0f;
        if (c->audio)
            SDL_SetAudioStreamGain(c->audio, cfg->volume);
        c->dirty = 1;
        break;
    }
    case ID_REBIND_ALL:
        menu_leave(c);
        c->rb->active = 1;
        c->rb->step = 0;
        memcpy(c->rb->backup, cfg->keys, sizeof c->rb->backup);
        break;
    case ID_REWIND: {
        int n = (int)(sizeof rewind_choices / sizeof rewind_choices[0]), k = 0;
        for (i = 0; i < n; i++)
            if (rewind_choices[i] == cfg->rewind_seconds)
                k = i;
        cfg->rewind_seconds = rewind_choices[wrap(k + dir, 0, n - 1)];
        if (rw_on)
            rewind_free(&rw);
        free(rw_blob);
        rw_blob = NULL;
        rewind_setup(cfg);
        rewind_record(*c->frame);
        c->dirty = 1;
        break;
    }
    case ID_NET_JOINCODE:
    case ID_NET_HOST:
    case ID_NET_JOIN: {
        char *clip = id == ID_NET_JOINCODE ? SDL_GetClipboardText() : NULL;
        int r = net_start(id == ID_NET_HOST ? 1 : id == ID_NET_JOINCODE ? 3 : 0, clip ? clip : cfg->net_join, cfg,
                          c->hash, c->status, sizeof c->status);
        SDL_free(clip);
        if (r == 0) {
            c->dirty = 1;
            menu_leave(c);
        }
        say(c->status);
        break;
    }
    case ID_NET_COPY:
        if (net_code[0]) {
            SDL_SetClipboardText(net_code);
            say("join code copied");
        }
        break;
    case ID_NET_PEERCODE: {
        char *clip = SDL_GetClipboardText();
        net_add_peer_code(clip ? clip : "", c->status, sizeof c->status);
        SDL_free(clip);
        say(c->status);
        break;
    }
    case ID_NET_ADDR:
        c->text_edit = 1;
        SDL_StartTextInput(c->video->win);
        break;
    case ID_NET_DELAY:
        cfg->net_delay = wrap(cfg->net_delay + dir, 0, 3);
        c->dirty = 1;
        break;
    case ID_NET_RESYNC:
        if (net_resync(&net) == 0)
            menu_leave(c);
        break;
    case ID_NET_LEAVE:
        net_stop();
        power_on(cfg->ppu_new ? PPU_MODEL_NEW : PPU_MODEL_OLD);
        SDL_SetWindowTitle(c->video->win, TITLE);
        say("left the online game");
        menu_leave(c);
        break;
    case ID_FF:
        cfg->ff_speed = wrap(cfg->ff_speed + dir, 2, 16);
        c->dirty = 1;
        break;
    default:
        break;
    }
}

static void menu_do(PlayCtx *c, MenuInput in)
{
    MenuEvent ev;
    int id, dir;
    build_page(&menu, c);
    ev = menu_input(&menu, in, &id, &dir);
    menu_apply(c, ev, id, dir);
}

/* Keyboard while the menu is open (or a key is being rebound). */
static void menu_key(PlayCtx *c, SDL_Scancode sc, SDL_Keycode key)
{
    if (c->text_edit) { /* the join address: characters come as text events */
        size_t n = strlen(c->cfg->net_join);
        if (key == SDLK_BACKSPACE && n)
            c->cfg->net_join[n - 1] = 0;
        else if (key == SDLK_RETURN || key == SDLK_KP_ENTER || key == SDLK_ESCAPE) {
            c->text_edit = 0;
            c->dirty = 1;
            SDL_StopTextInput(c->video->win);
        }
        return;
    }
    if (c->capture) {
        int k = c->capture - ID_KEY_BASE;
        if (key != SDLK_ESCAPE) {
            c->cfg->keys[k / BTN_COUNT][k % BTN_COUNT] = sc;
            c->dirty = 1;
        }
        c->capture = 0;
        return;
    }
    switch (key) {
    case SDLK_UP: case SDLK_KP_8: menu_do(c, MENU_UP); break;
    case SDLK_DOWN: case SDLK_KP_2: menu_do(c, MENU_DOWN); break;
    case SDLK_LEFT: case SDLK_KP_4: menu_do(c, MENU_LEFT); break;
    case SDLK_RIGHT: case SDLK_KP_6: menu_do(c, MENU_RIGHT); break;
    case SDLK_RETURN: case SDLK_KP_ENTER: case SDLK_SPACE: menu_do(c, MENU_ACCEPT); break;
    case SDLK_ESCAPE: case SDLK_BACKSPACE: menu_do(c, MENU_BACK); break;
    default: break;
    }
}

/* Gamepad while the menu is open: d-pad, south = accept, east = back, Guide / Start = close. */
static void menu_pad(PlayCtx *c, Uint8 button)
{
    if (c->capture)
        return;
    switch (button) {
    case SDL_GAMEPAD_BUTTON_DPAD_UP: menu_do(c, MENU_UP); break;
    case SDL_GAMEPAD_BUTTON_DPAD_DOWN: menu_do(c, MENU_DOWN); break;
    case SDL_GAMEPAD_BUTTON_DPAD_LEFT: menu_do(c, MENU_LEFT); break;
    case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: menu_do(c, MENU_RIGHT); break;
    case SDL_GAMEPAD_BUTTON_SOUTH: menu_do(c, MENU_ACCEPT); break;
    case SDL_GAMEPAD_BUTTON_EAST: menu_do(c, MENU_BACK); break;
    case SDL_GAMEPAD_BUTTON_GUIDE: case SDL_GAMEPAD_BUTTON_START: menu_leave(c); break;
    default: break;
    }
}

/* The audio device can take seconds to open on a busy machine; it is opened on its own thread while the window and
 * renderer are created. */
static SDL_AudioStream *audio_opened;

static int SDLCALL open_audio_thread(void *spec)
{
    audio_opened = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, (const SDL_AudioSpec *)spec, NULL, NULL);
    if (!audio_opened)
        SDL_Log("no audio (%s); pacing by timer", SDL_GetError());
    return 0;
}

/* startup timing: one log line with the time of each stage (the window/renderer stage can take seconds while the
 * GPU is busy) */
static Uint64 stage_t0, stage_last;
static char stage_text[400];

static void stage(const char *name)
{
    Uint64 now = SDL_GetTicksNS();
    size_t n = strlen(stage_text);
    if (!stage_t0)
        stage_t0 = stage_last = now;
    if (name && n + 40 < sizeof stage_text)
        snprintf(stage_text + n, sizeof stage_text - n, "%s%s %llu ms", n ? ", " : "", name,
                 (unsigned long long)((now - stage_last) / 1000000));
    stage_last = now;
}

/* SDL_clamp is a macro that evaluates its argument more than once: never pass argv[++i] to it */
static int arg_int(const char *s, int lo, int hi)
{
    int v = atoi(s);
    return v < lo ? lo : v > hi ? hi : v;
}

static int fail(const char *what)
{
    fprintf(stderr, "%s\n", what); /* first: the box waits for the user */
    fflush(stderr);
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "btdd", what, NULL);
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
    int net_cli = 0; /* 1 = --host, 2 = --join, 3 = --host-loopback */
    int slot = 0, states_test = 0, st_phase = 0, st_fail = 0, rewinding = 0, ff = 0, step, steps, pads_ready = 0;
    int menu_step = 0, menu_nav_ok = 0, menu_close_ok = 0; /* --selftest-gamepad, menu part */
    static uint8_t st_ram_a[0x800], st_ram_r[0x800];
    FILE *dump = NULL;
    SDL_Joystick *vjoy = NULL;
    char hash[41], msg[1400];
    Fm2 movie = {0};
    SDL_AudioStream *audio;
    SDL_AudioSpec spec = {SDL_AUDIO_F32, 1, AUDIO_RATE};
    SDL_Gamepad *gp[2] = {NULL, NULL};
    Uint64 next_tick;
    PlayCtx ctx;
    RomResult rr;

    memset(&rb, 0, sizeof rb);
    stage(NULL);
    /* Gamepads are initialized after the first frame is on screen (see the frame loop): their init enumerates
     * DirectInput devices, 1-3 s on a busy machine, and the game's first frames are a black boot screen anyway. */
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO)) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    stage("SDL_Init");
    have_cfg = config_load(&cfg) == 0;
    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--movie") && i + 1 < argc)
            movie_path = argv[++i];
        else if (!strcmp(argv[i], "--frames") && i + 1 < argc)
            max_frames = strtol(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--ppu") && i + 1 < argc)
            cfg.ppu_new = !strcmp(argv[++i], "new");
        else if (!strcmp(argv[i], "--scale") && i + 1 < argc)
            cfg.scale = arg_int(argv[++i], 1, 10);
        else if (!strcmp(argv[i], "--selftest-gamepad"))
            selftest_on = 1;
        else if (!strcmp(argv[i], "--selftest-states"))
            states_test = 1;
        else if (!strcmp(argv[i], "--host")) { /* M13: host an online game (optional port) */
            net_cli = 1;
            if (i + 1 < argc && argv[i + 1][0] >= '0' && argv[i + 1][0] <= '9')
                cfg.net_port = arg_int(argv[++i], 1, 65535);
        } else if (!strcmp(argv[i], "--host-loopback")) { /* tests on one PC */
            net_cli = 3;
            if (i + 1 < argc && argv[i + 1][0] >= '0' && argv[i + 1][0] <= '9')
                cfg.net_port = arg_int(argv[++i], 1, 65535);
        } else if (!strcmp(argv[i], "--join-code") && i + 1 < argc) { /* join with a join code */
            net_cli = 4;
            snprintf(cfg.net_join, sizeof cfg.net_join, "%s", argv[++i]);
        } else if (!strcmp(argv[i], "--join") && i + 1 < argc) {
            net_cli = 2;
            snprintf(cfg.net_join, sizeof cfg.net_join, "%s", argv[++i]);
        } else if (!strcmp(argv[i], "--net-delay") && i + 1 < argc)
            cfg.net_delay = arg_int(argv[++i], 0, 3);
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

    stage("config+rom");
    {
        /* the audio device opens on its own thread while the window and renderer are created */
        SDL_Thread *t = no_audio ? NULL : SDL_CreateThread(open_audio_thread, "btdd audio open", &spec);
        if (video_open(&video, TITLE, cfg.scale, cfg.aspect, cfg.fullscreen, cfg.filter, cfg.scanlines,
                       cfg.renderer, cfg.gpu, cfg.shader) != 0) {
            snprintf(msg, sizeof msg, "cannot open the window: %s", SDL_GetError());
            return fail(msg);
        }
        stage("window+renderer");
        if (t)
            SDL_WaitThread(t, NULL);
        else if (!no_audio)
            open_audio_thread(&spec); /* no thread: open it here */
    }
    audio = audio_opened;
    if (audio) {
        SDL_SetAudioStreamGain(audio, cfg.volume);
        SDL_ResumeAudioStreamDevice(audio);
    } else if (no_audio) {
        SDL_Log("no audio (--no-audio); pacing by timer");
    }
    if (selftest_on) {
        SDL_VirtualJoystickDesc d;
        SDL_JoystickID id;
        if (!SDL_InitSubSystem(SDL_INIT_GAMEPAD))
            return fail("cannot initialize gamepads");
        pads_ready = 1;
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
    stage("audio");
    if (pads_ready)
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
    net_audio = audio;
    if (net_cli) {
        if (net_start(net_cli == 1 ? 1 : net_cli == 3 ? 2 : net_cli == 4 ? 3 : 0, cfg.net_join, &cfg, hash, msg,
                      sizeof msg) != 0)
            return fail(msg);
        net_dump = dump;
        say(msg);
    }
    stage("gamepads+console");
    SDL_Log("startup: %s; total %llu ms", stage_text, (unsigned long long)((SDL_GetTicksNS() - stage_t0) / 1000000));
    next_tick = SDL_GetTicksNS();
    memset(&ctx, 0, sizeof ctx);
    ctx.cfg = &cfg;
    ctx.video = &video;
    ctx.audio = audio;
    ctx.gp = gp;
    ctx.rb = &rb;
    ctx.slot = &slot;
    ctx.quit = &quit;
    ctx.frame = &frame;
    ctx.hash = hash;
    ctx.have_cfg = have_cfg;

    while (!quit) {
        SDL_Event ev;
        int run_frame, hold = paused || rb.active || menu.open;
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
            case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
                if (menu.open) {
                    menu_pad(&ctx, ev.gbutton.button);
                } else if (!rb.active) { /* Guide, or Start while Back is held: settings */
                    SDL_Gamepad *pad = SDL_GetGamepadFromID(ev.gbutton.which);
                    if (ev.gbutton.button == SDL_GAMEPAD_BUTTON_GUIDE ||
                        (ev.gbutton.button == SDL_GAMEPAD_BUTTON_START && pad &&
                         SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_BACK)))
                        menu_open(&menu, PG_MAIN);
                }
                break;
            case SDL_EVENT_TEXT_INPUT:
                if (menu.open && ctx.text_edit) { /* join address: digits, letters, dot, colon, dash */
                    const char *t = ev.text.text;
                    size_t n = strlen(cfg.net_join);
                    for (; *t && n + 1 < sizeof cfg.net_join; t++)
                        if ((*t >= '0' && *t <= '9') || (*t >= 'a' && *t <= 'z') || (*t >= 'A' && *t <= 'Z') ||
                            *t == '.' || *t == ':' || *t == '-') {
                            cfg.net_join[n++] = *t;
                            cfg.net_join[n] = 0;
                        }
                }
                break;
            case SDL_EVENT_KEY_DOWN:
                if (menu.open) { /* the settings menu takes the keyboard (repeats too, for scrolling) */
                    menu_key(&ctx, ev.key.scancode, ev.key.key);
                    break;
                }
                if (ev.key.repeat || rebind_key(&rb, &cfg, ev.key.scancode))
                    break;
                if (ev.key.key == SDLK_ESCAPE) {
                    menu_open(&menu, PG_MAIN); /* settings; Quit is in there */
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
                } else if (ev.key.key == SDLK_F3 && video.gpu) { /* GPU path: cycle the shader presets */
                    video.preset = cfg.shader = (cfg.shader + 1) % GPU_PRESET_COUNT;
                    snprintf(msg, sizeof msg, "shader: %s", gpu_preset_names[cfg.shader]);
                    say(msg);
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
                } else if (ev.key.key == SDLK_F10) {
                    toggle_mod("widescreen", &cfg, have_cfg);
                } else if (ev.key.key == SDLK_F12) {
                    save_screenshot(&cfg);
                } else if ((ev.key.key == SDLK_F6 || ev.key.key == SDLK_F7) && !net_on) {
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
        if (hold) { /* paused, redefining keys or in the settings: keep drawing the last picture with the overlay */
            if (audio)
                SDL_ClearAudioStream(audio);
            if (menu.open) {
                if (vjoy) { /* --selftest-gamepad, menu part: release Guide, d-pad down, then East (back = close) */
                    menu_step++;
                    SDL_SetJoystickVirtualButton(vjoy, SDL_GAMEPAD_BUTTON_GUIDE, 0);
                    SDL_SetJoystickVirtualButton(vjoy, SDL_GAMEPAD_BUTTON_DPAD_DOWN, menu_step >= 3 && menu_step < 6);
                    if (menu_step == 9)
                        menu_nav_ok = menu.page == PG_MAIN && menu.cursor[PG_MAIN] == 1;
                    SDL_SetJoystickVirtualButton(vjoy, SDL_GAMEPAD_BUTTON_EAST, menu_step >= 9 && menu_step < 12);
                    SDL_UpdateJoysticks();
                }
                build_page(&menu, &ctx);
                video_present_menu(&video, pixels, menu.lines, menu_render(&menu));
            } else {
                video_present(&video, pixels, lines, overlay(&rb, &cfg, paused, lines));
            }
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

        if (net_on) { /* online: one session tick per 60 Hz display frame (not paced by the audio queue) */
            Uint64 now = SDL_GetTicksNS();
            char st[160];
            if (now < next_tick) {
                SDL_Delay(1);
                continue;
            }
            next_tick = (now - next_tick > 100000000u) ? now : next_tick + (Uint64)(1e9 / FRAME_HZ);
            net_conn_tick(&cfg);
            {
                int l = net.local;
                uint8_t pad = movie.frames ? (net.have[l] < (uint32_t)movie.count ? movie.frames[net.have[l]].pad[l] : 0)
                                           : read_pad(&cfg, 0, gp[0]); /* each player uses their own player 1 keys */
                if (net_tick(&net, pad))
                    frame = (long)net.frame;
            }
            if (audio && SDL_GetAudioStreamQueued(audio) > (int)(8 * AUDIO_RATE / FRAME_HZ) * (int)sizeof(float))
                SDL_ClearAudioStream(audio); /* clock drift between the sound card and the 60 Hz timer */
            if (net.state == NET_DESYNC && net.stats.desync_frame >= 0 && net.stats.desync_frame != net_logged_desync) {
                char path[1200], *slash;
                net_logged_desync = net.stats.desync_frame;
                snprintf(path, sizeof path, "%s", cfg.path);
                slash = strrchr(path, '\\');
                if (!slash)
                    slash = strrchr(path, '/');
                if (slash) {
                    snprintf(slash + 1, sizeof path - (size_t)(slash + 1 - path), "netplay");
                    SDL_CreateDirectory(path);
                    snprintf(path + strlen(path), sizeof path - strlen(path), "/desync_f%d_p%d.state",
                             net.stats.desync_frame, net.local + 1);
                    nes_state_save(&nes, (uint64_t)net.frame, hash, path);
                    SDL_Log("netplay: desync at frame %d, our state saved to %s%s", net.stats.desync_frame, path,
                            net.local == 0 ? " (Resync is in the settings menu)" : "");
                }
            }
            if (strcmp(net.message, net_last_msg)) {
                snprintf(net_last_msg, sizeof net_last_msg, "%s", net.message);
                say(net.message);
            }
            if ((frame % 60) == 0) {
                snprintf(st, sizeof st, TITLE " - online: %s", net_status(&net, msg, sizeof msg));
                SDL_SetWindowTitle(video.win, st);
            }
            if ((movie.frames && (long)net.reported >= movie.count) || (max_frames >= 0 && (long)net.reported >= max_frames))
                quit = 1;
            make_picture(&video);
            video_present(&video, pixels, lines, overlay(&rb, &cfg, 0, lines));
            continue;
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
            make_picture(&video);
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
                    SDL_SetJoystickVirtualButton(vjoy, SDL_GAMEPAD_BUTTON_GUIDE, frame == 435); /* opens the menu */
                    if (menu_step)
                        SDL_SetJoystickVirtualButton(vjoy, SDL_GAMEPAD_BUTTON_EAST, 0); /* East closed the menu */
                    SDL_UpdateJoysticks();
                    if (menu_step >= 9 && !menu.open)
                        menu_close_ok = 1;
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
        make_picture(&video);
        video_present(&video, pixels, lines, overlay(&rb, &cfg, 0, lines));
        if (!pads_ready) { /* first frame is on screen: now the slow gamepad init (see SDL_Init above) */
            Uint64 t = SDL_GetTicksNS();
            if (!SDL_InitSubSystem(SDL_INIT_GAMEPAD))
                SDL_Log("gamepad init: %s", SDL_GetError());
            pads_ready = 1;
            open_gamepads(gp);
            SDL_Log("gamepads ready after %llu ms", (unsigned long long)((SDL_GetTicksNS() - t) / 1000000));
        }

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
        printf("%s: gamepad Guide opens the settings, d-pad down moves the cursor\n", menu_nav_ok ? "ok  " : "FAIL");
        printf("%s: gamepad East closes the settings, the game continues\n", menu_close_ok ? "ok  " : "FAIL");
        ok &= menu_nav_ok && menu_close_ok;
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
    net_stop();
    SDL_Quit();
    return quit == 2 ? 1 : 0;
}
