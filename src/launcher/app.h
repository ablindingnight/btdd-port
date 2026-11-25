/* Launcher state: settings (shared btdd.cfg), the player's ROM, the first-run checklist on a worker thread, game
 * runs with their logs, and the report folder. No drawing here (screens.c). */
#ifndef BTDD_LAUNCHER_APP_H
#define BTDD_LAUNCHER_APP_H
#include "platform/config.h"
#include <SDL3/SDL.h>

#ifndef LAUNCHER_VERSION
#define LAUNCHER_VERSION "0.1.0"
#endif

typedef enum Screen {
    SCR_HOME, SCR_FIRST_RUN,
    SCR_DISPLAY, SCR_GRAPHICS, SCR_AUDIO, SCR_CONTROLS, SCR_GAMEPLAY, SCR_MODS, SCR_ONLINE, SCR_BUILDS, SCR_LOGS,
    SCR_ABOUT, SCR_INGAME, SCR_COUNT
} Screen;
/* SCR_INGAME is only for --ui-shot: btdd_play's in-game menu as it looks, over a placeholder picture */
#define SCR_FIRST_SETTINGS SCR_DISPLAY
#define SCR_LAST_SETTINGS SCR_ABOUT

typedef enum StepState { STEP_WAIT, STEP_RUN, STEP_DONE, STEP_SKIP, STEP_FAIL } StepState;
enum { STEP_PICK, STEP_VERIFY, STEP_COMPILE, STEP_CHECK, STEP_ENHANCE, STEP_READY, STEP_COUNT };

typedef struct Step {
    StepState state;
    float progress; /* 0..1 while running, <0 = unknown */
    char note[160];
} Step;

#define MAX_LOGS 12
typedef struct LogEntry {
    char file[512];  /* full path */
    char when[32];   /* "24 Nov 17:31" */
    char what[96];
    char result[24];
    char took[24];
} LogEntry;

typedef struct App {
    Config cfg;
    int have_cfg;
    char exe_dir[1024];
    char data_dir[1024];  /* the folder holding btdd.cfg (states/, logs/, reports there) */
    char font_dir[1024];
    /* ROM */
    int rom_ok;           /* verified USA dump */
    char rom_sha1[41];
    char rom_msg[160];
    /* tools next to the launcher */
    char play_exe[1100], jit_exe[1100], headless_exe[1100];
    /* first run */
    Step steps[STEP_COUNT];
    SDL_Mutex *lock;
    SDL_Thread *worker;
    SDL_AtomicInt worker_busy;
    int first_run_done;
    /* game process */
    SDL_Process *game;
    char game_log[1024];
    Uint64 game_started;
    char game_what[96];
    /* logs */
    LogEntry logs[MAX_LOGS];
    int n_logs;
    /* ui */
    Screen screen;
    int focus[SCR_COUNT];
    char toast[128];
    double toast_until;
    int capture;          /* key rebinding: player*16 + button + 1, 0 = none */
    int edit;             /* text field being edited (screens.c ids), 0 = none */
    char join_code[160];
    int demo;             /* example values for screenshots */
    double now;
    int quit;
} App;

void app_init(App *a, const char *argv0, int demo);
void app_quit(App *a);
void app_save_settings(App *a);
void app_check_rom(App *a);                      /* quick verify of cfg.rom (main thread) */
void app_set_rom(App *a, const char *path);      /* new ROM: verify, save, restart the checklist */
void app_start_first_run(App *a, int from_step); /* worker thread: verify, compile, self-check */
void app_step_get(App *a, int i, Step *out);
int app_ready(const App *a);                     /* ROM verified and the checklist finished */
/* starts btdd_play with extra arguments (NULL-terminated); returns 0 on success */
int app_launch_game(App *a, const char *what, const char *const *extra);
void app_poll(App *a);                           /* game finished? worker messages */
void app_load_logs(App *a);
int app_save_report(App *a, char *out_dir, size_t len);
void app_toast(App *a, const char *msg);
void app_open_folder(const char *path);
int app_slot_info(const App *a, int slot, char *when, size_t len); /* 1 if the save slot file exists */
void app_cache_info(const App *a, char *out, size_t len);
int app_clear_cache(App *a);

#endif
