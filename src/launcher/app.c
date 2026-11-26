#include "app.h"
#include "rom/rom.h"
#include "mods/mods.h"
#include "unzip.h"
#include "romcheck.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define EXE_SUFFIX ".exe"
#define SEP "\\"
#else
#define EXE_SUFFIX ""
#define SEP "/"
#endif

#ifndef BTDD_SOURCE_DIR
#define BTDD_SOURCE_DIR "."
#endif

static const char *const MONTHS[12] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};

static int exists(const char *path)
{
    SDL_PathInfo pi;
    return SDL_GetPathInfo(path, &pi) && pi.type != SDL_PATHTYPE_NONE;
}

static void dirname_of(const char *path, char *out, size_t len)
{
    char *s;
    snprintf(out, len, "%s", path);
    s = strrchr(out, '/');
    {
        char *b = strrchr(out, '\\');
        if (!s || (b && b > s))
            s = b;
    }
    if (s)
        *s = 0;
    else
        snprintf(out, len, ".");
}

static void now_stamp(char *file_stamp, size_t fl, char *human, size_t hl)
{
    SDL_Time t;
    SDL_DateTime d;
    SDL_GetCurrentTime(&t);
    SDL_TimeToDateTime(t, &d, true);
    if (file_stamp)
        snprintf(file_stamp, fl, "%04d%02d%02d-%02d%02d%02d", d.year, d.month, d.day, d.hour, d.minute, d.second);
    if (human)
        snprintf(human, hl, "%d %s %02d:%02d", d.day, MONTHS[(d.month - 1) % 12], d.hour, d.minute);
}

void app_toast(App *a, const char *msg)
{
    snprintf(a->toast, sizeof a->toast, "%s", msg);
    a->toast_until = a->now + 2.5;
}

void app_open_folder(const char *path)
{
    char url[1200];
    size_t i;
    snprintf(url, sizeof url, "file:///%s", path);
    for (i = 0; url[i]; i++)
        if (url[i] == '\\')
            url[i] = '/';
    SDL_OpenURL(url);
}

/* ---------- launcher.ini: first_run_sha1 = <sha1 of the ROM the checklist finished for>, advanced = 0 | 1 ---------- */
static void ini_path(const App *a, char *out, size_t len) { snprintf(out, len, "%s" SEP "launcher.ini", a->data_dir); }

static void ini_read(App *a, char *sha, size_t len)
{
    char p[1100], line[256];
    FILE *f;
    sha[0] = 0;
    ini_path(a, p, sizeof p);
    if (!(f = fopen(p, "r")))
        return;
    while (fgets(line, sizeof line, f))
        if (!strncmp(line, "first_run_sha1 = ", 17)) {
            snprintf(sha, len, "%.40s", line + 17);
        } else if (!strncmp(line, "advanced = ", 11))
            a->advanced = atoi(line + 11) != 0;
    fclose(f);
}

static void ini_write(App *a, const char *sha)
{
    char p[1100];
    FILE *f;
    ini_path(a, p, sizeof p);
    if ((f = fopen(p, "w"))) {
        fprintf(f, "# Pond to Pavement launcher state\nfirst_run_sha1 = %s\nadvanced = %d\n", sha, a->advanced);
        fclose(f);
    }
}

/* ---------- setup ---------- */
void app_init(App *a, const char *argv0, int demo)
{
    const char *base = SDL_GetBasePath();
    char p[1200];
    (void)argv0;
    memset(a, 0, sizeof *a);
    a->demo = demo;
    a->lock = SDL_CreateMutex();
    snprintf(a->exe_dir, sizeof a->exe_dir, "%s", base ? base : "./");
    {
        size_t n = strlen(a->exe_dir);
        if (n && (a->exe_dir[n - 1] == '/' || a->exe_dir[n - 1] == '\\'))
            a->exe_dir[n - 1] = 0;
    }
    snprintf(p, sizeof p, "%s" SEP "fonts" SEP "Bangers-Regular.ttf", a->exe_dir);
    if (exists(p))
        snprintf(a->font_dir, sizeof a->font_dir, "%s" SEP "fonts", a->exe_dir);
    else
        snprintf(a->font_dir, sizeof a->font_dir, "%s/res/fonts", BTDD_SOURCE_DIR);
    snprintf(a->play_exe, sizeof a->play_exe, "%s" SEP "btdd_play" EXE_SUFFIX, a->exe_dir);
    snprintf(a->jit_exe, sizeof a->jit_exe, "%s" SEP "btdd_jit" EXE_SUFFIX, a->exe_dir);
    snprintf(a->headless_exe, sizeof a->headless_exe, "%s" SEP "btdd_headless" EXE_SUFFIX, a->exe_dir);
    config_defaults(&a->cfg);
    a->have_cfg = config_load(&a->cfg) == 0;
    dirname_of(a->cfg.path, a->data_dir, sizeof a->data_dir);
    snprintf(a->join_code, sizeof a->join_code, "%s", "");
    app_check_rom(a);
    app_load_logs(a);
    if (demo) {
        int i;
        a->rom_ok = 1;
        a->first_run_done = 1;
        snprintf(a->cfg.rom, sizeof a->cfg.rom, "C:\\Games\\NES\\Battletoads-Double Dragon (USA).nes");
        snprintf(a->rom_msg, sizeof a->rom_msg, "SHA-1 ok");
        for (i = 0; i < STEP_COUNT; i++)
            a->steps[i].state = STEP_DONE;
    }
}

void app_quit(App *a)
{
    if (a->worker)
        SDL_WaitThread(a->worker, NULL);
    if (a->game)
        SDL_DestroyProcess(a->game); /* the game keeps running on its own */
    SDL_DestroyMutex(a->lock);
}

void app_set_advanced(App *a, int on)
{
    char sha[64];
    ini_read(a, sha, sizeof sha);
    a->advanced = on;
    if (!a->demo)
        ini_write(a, sha);
}

void app_save_settings(App *a)
{
    if (a->demo)
        return;
    if (config_save(&a->cfg) != 0)
        app_toast(a, "Could not save settings");
}

void app_check_rom(App *a)
{
    Rom *rom;
    char sha[41] = "", done_sha[64];
    RomResult r;
    a->rom_ok = 0;
    a->rom_sha1[0] = 0;
    if (!a->cfg.rom[0]) {
        snprintf(a->rom_msg, sizeof a->rom_msg, "No ROM picked yet");
        return;
    }
    rom = (Rom *)malloc(sizeof *rom);
    if (!rom)
        return;
    r = rom_load(a->cfg.rom, rom, sha);
    free(rom);
    snprintf(a->rom_sha1, sizeof a->rom_sha1, "%s", sha);
    if (r == ROM_OK) {
        a->rom_ok = 1;
        snprintf(a->rom_msg, sizeof a->rom_msg, "SHA-1 ok");
        a->rom_hint[0] = 0;
    } else /* plain words: what is wrong and what to do */
        romcheck_file(a->cfg.rom, a->rom_msg, sizeof a->rom_msg, a->rom_hint, sizeof a->rom_hint);
    ini_read(a, done_sha, sizeof done_sha);
    a->first_run_done = a->rom_ok && !strcmp(done_sha, a->rom_sha1);
    if (a->first_run_done) {
        int i;
        for (i = 0; i < STEP_COUNT; i++)
            a->steps[i].state = STEP_DONE;
    }
}

/* a .zip ROM: its first .nes is unpacked into <settings folder>/rom/ (btdd_play reads plain .nes files) */
static int unpack_zip(App *a, const char *zip_path, char *out, size_t len)
{
    size_t n = 0, rn = 0;
    uint8_t *zip = (uint8_t *)SDL_LoadFile(zip_path, &n), *rom = NULL;
    char name[200], dir[1100];
    int r;
    FILE *f;
    if (!zip) {
        snprintf(a->rom_msg, sizeof a->rom_msg, "cannot open or read the file");
        return -1;
    }
    r = unzip_first_nes(zip, n, &rom, &rn, name, sizeof name);
    SDL_free(zip);
    if (r) {
        snprintf(a->rom_msg, sizeof a->rom_msg, "%s", r == -3 ? "no .nes file inside this .zip" : r == -2 ? "this .zip uses a packing method we can't read; unzip it first" : "not a readable .zip");
        return -1;
    }
    snprintf(dir, sizeof dir, "%s" SEP "rom", a->data_dir);
    SDL_CreateDirectory(dir);
    snprintf(out, len, "%s" SEP "%s", dir, name);
    f = fopen(out, "wb");
    if (!f || fwrite(rom, 1, rn, f) != rn) {
        if (f)
            fclose(f);
        free(rom);
        snprintf(a->rom_msg, sizeof a->rom_msg, "could not unpack the .zip into %s", dir);
        return -1;
    }
    fclose(f);
    free(rom);
    return 0;
}

void app_set_rom(App *a, const char *path)
{
    size_t pl = strlen(path);
    if (pl > 4 && !SDL_strcasecmp(path + pl - 4, ".zip")) {
        char unpacked[1300];
        if (romcheck_file(path, a->rom_msg, sizeof a->rom_msg, a->rom_hint, sizeof a->rom_hint) != RC_OK ||
            unpack_zip(a, path, unpacked, sizeof unpacked) != 0) {
            snprintf(a->cfg.rom, sizeof a->cfg.rom, "%s", path); /* shown, not saved */
            a->rom_ok = 0;
            app_start_first_run(a, STEP_VERIFY);
            return;
        }
        snprintf(a->cfg.rom, sizeof a->cfg.rom, "%s", unpacked);
    } else
        snprintf(a->cfg.rom, sizeof a->cfg.rom, "%s", path);
    app_check_rom(a);
    if (a->rom_ok)
        app_save_settings(a);
    app_start_first_run(a, STEP_VERIFY);
}

int app_ready(const App *a) { return a->rom_ok && a->first_run_done; }

/* ---------- processes ---------- */
static SDL_Process *spawn(const char *const *args, const char *log_path)
{
    SDL_PropertiesID pr = SDL_CreateProperties();
    SDL_IOStream *io = log_path ? SDL_IOFromFile(log_path, "wb") : NULL;
    SDL_Process *p;
    SDL_SetPointerProperty(pr, SDL_PROP_PROCESS_CREATE_ARGS_POINTER, (void *)args);
    SDL_SetNumberProperty(pr, SDL_PROP_PROCESS_CREATE_STDIN_NUMBER, SDL_PROCESS_STDIO_NULL);
    /* background = CREATE_NO_WINDOW on Windows: no console window for the game or the first-run tools */
    SDL_SetBooleanProperty(pr, SDL_PROP_PROCESS_CREATE_BACKGROUND_BOOLEAN, true);
    if (io) {
        SDL_SetNumberProperty(pr, SDL_PROP_PROCESS_CREATE_STDOUT_NUMBER, SDL_PROCESS_STDIO_REDIRECT);
        SDL_SetPointerProperty(pr, SDL_PROP_PROCESS_CREATE_STDOUT_POINTER, io);
        SDL_SetBooleanProperty(pr, SDL_PROP_PROCESS_CREATE_STDERR_TO_STDOUT_BOOLEAN, true);
    } else
        SDL_SetNumberProperty(pr, SDL_PROP_PROCESS_CREATE_STDOUT_NUMBER, SDL_PROCESS_STDIO_NULL);
    p = SDL_CreateProcessWithProperties(pr);
    SDL_DestroyProperties(pr);
    if (io)
        SDL_CloseIO(io); /* the child has its own handle */
    return p;
}

/* the child writes to "<log>.out" (its own file position would overwrite our header lines); appended after */
static void append_output(const char *log_path)
{
    char out[1200];
    size_t n = 0;
    void *buf;
    snprintf(out, sizeof out, "%s.out", log_path);
    buf = SDL_LoadFile(out, &n);
    if (buf) {
        FILE *f = fopen(log_path, "ab");
        if (f) {
            fwrite(buf, 1, n, f);
            fclose(f);
        }
        SDL_free(buf);
    }
    SDL_RemovePath(out);
}

static SDL_Process *spawn_logged(const char *const *args, const char *log_path)
{
    char out[1200];
    snprintf(out, sizeof out, "%s.out", log_path);
    SDL_RemovePath(out);
    return spawn(args, out);
}

static int run_wait(const char *const *args, const char *log_path)
{
    SDL_Process *p = spawn_logged(args, log_path);
    int code = -1;
    if (!p)
        return -1;
    SDL_WaitProcess(p, true, &code);
    SDL_DestroyProcess(p);
    append_output(log_path);
    return code;
}

static void log_line(const char *path, const char *fmt, const char *s)
{
    FILE *f = fopen(path, "a");
    if (f) {
        fprintf(f, fmt, s);
        fclose(f);
    }
}

static void logs_dir(const App *a, char *out, size_t len)
{
    snprintf(out, len, "%s" SEP "logs", a->data_dir);
    SDL_CreateDirectory(out);
}

static void new_log(const App *a, const char *kind, const char *what, char *out, size_t len)
{
    char dir[1100], stamp[32];
    logs_dir(a, dir, sizeof dir);
    now_stamp(stamp, sizeof stamp, NULL, 0);
    snprintf(out, len, "%s" SEP "%s-%s.log", dir, kind, stamp);
    log_line(out, "# what: %s\n", what);
}

/* ---------- first-run checklist (worker thread) ---------- */
static void set_step(App *a, int i, StepState s, float prog, const char *note)
{
    SDL_LockMutex(a->lock);
    a->steps[i].state = s;
    a->steps[i].progress = prog;
    if (note)
        snprintf(a->steps[i].note, sizeof a->steps[i].note, "%s", note);
    SDL_UnlockMutex(a->lock);
}

void app_step_get(App *a, int i, Step *out)
{
    SDL_LockMutex(a->lock);
    *out = a->steps[i];
    SDL_UnlockMutex(a->lock);
}

static int files_equal(const char *x, const char *y)
{
    size_t nx = 0, ny = 0;
    void *bx = SDL_LoadFile(x, &nx), *by = SDL_LoadFile(y, &ny);
    int eq = bx && by && nx == ny && nx > 0 && !memcmp(bx, by, nx);
    SDL_free(bx);
    SDL_free(by);
    return eq;
}

typedef struct Job {
    App *a;
    int from;
} Job;

static int SDLCALL worker(void *ud)
{
    Job *j = (Job *)ud;
    App *a = j->a;
    int from = j->from, native_ok = 0;
    char log[1100], ram_ref[1100], ram_nat[1100];
    free(j);
    new_log(a, "firstrun", "First run", log, sizeof log);
    snprintf(ram_ref, sizeof ram_ref, "%s" SEP "selfcheck_ref.bin", a->data_dir);
    snprintf(ram_nat, sizeof ram_nat, "%s" SEP "selfcheck_native.bin", a->data_dir);

    if (from <= STEP_VERIFY) {
        set_step(a, STEP_VERIFY, STEP_RUN, -1, "Checking the SHA-1");
        if (!a->rom_ok) {
            set_step(a, STEP_VERIFY, STEP_FAIL, 0, a->rom_msg);
            log_line(log, "# result: %s\n", "rom not verified");
            SDL_SetAtomicInt(&a->worker_busy, 0);
            return 0;
        }
        set_step(a, STEP_VERIFY, STEP_DONE, 1, "SHA-1 matches the USA cartridge");
    }
    if (from <= STEP_COMPILE) {
        if (!exists(a->jit_exe))
            set_step(a, STEP_COMPILE, STEP_SKIP, 0, "No native compiler in this download yet: the game runs on the reference engine");
        else {
            const char *args[] = {a->jit_exe, a->cfg.rom, "--frames", "1", NULL};
            int code;
            set_step(a, STEP_COMPILE, STEP_RUN, -1, "Built-in compiler turns your ROM's code into a PC program");
            log_line(log, "# step: %s\n", "compile");
            code = run_wait(args, log);
            native_ok = code == 0;
            set_step(a, STEP_COMPILE, native_ok ? STEP_DONE : STEP_SKIP, 1,
                     native_ok ? "Native build ready (in your cache folder)"
                               : "Not built (see Logs); the game runs on the reference engine");
        }
    }
    if (from <= STEP_CHECK) {
        if (!exists(a->headless_exe))
            set_step(a, STEP_CHECK, STEP_SKIP, 0, "Checker not in this download");
        else {
            const char *ref[] = {a->headless_exe, a->cfg.rom, "--frames", "600", "--dump-ram", ram_ref, NULL};
            set_step(a, STEP_CHECK, STEP_RUN, 0.2f, "Boot to the title in the background");
            log_line(log, "# step: %s\n", "self-check reference");
            if (run_wait(ref, log) != 0)
                set_step(a, STEP_CHECK, STEP_FAIL, 0, "The engine did not boot your ROM (see Logs)");
            else if (native_ok) {
                const char *nat[] = {a->jit_exe, a->cfg.rom, "--frames", "600", "--dump-ram", ram_nat, NULL};
                set_step(a, STEP_CHECK, STEP_RUN, 0.6f, "Comparing native and reference frames");
                log_line(log, "# step: %s\n", "self-check native");
                if (run_wait(nat, log) == 0 && files_equal(ram_ref, ram_nat))
                    set_step(a, STEP_CHECK, STEP_DONE, 1, "Native and reference frames match");
                else
                    set_step(a, STEP_CHECK, STEP_FAIL, 0, "Native and reference differ (see Logs)");
            } else
                set_step(a, STEP_CHECK, STEP_DONE, 1, "Boots to the title on the reference engine");
            SDL_RemovePath(ram_ref);
            SDL_RemovePath(ram_nat);
        }
    }
    set_step(a, STEP_ENHANCE, STEP_SKIP, 0, "Sprite packs and HD tiles come later; nothing to do now");
    set_step(a, STEP_READY, STEP_DONE, 1, "Press PLAY");
    SDL_LockMutex(a->lock);
    a->first_run_done = 1;
    SDL_UnlockMutex(a->lock);
    ini_write(a, a->rom_sha1);
    log_line(log, "# result: %s\n", "ok");
    SDL_SetAtomicInt(&a->worker_busy, 0);
    return 0;
}

void app_start_first_run(App *a, int from)
{
    Job *j;
    int i;
    if (a->demo || SDL_GetAtomicInt(&a->worker_busy))
        return;
    if (a->worker) {
        SDL_WaitThread(a->worker, NULL);
        a->worker = NULL;
    }
    SDL_LockMutex(a->lock);
    a->first_run_done = 0;
    for (i = from; i < STEP_COUNT; i++) {
        a->steps[i].state = STEP_WAIT;
        a->steps[i].note[0] = 0;
    }
    a->steps[STEP_PICK].state = a->cfg.rom[0] ? STEP_DONE : STEP_WAIT;
    SDL_UnlockMutex(a->lock);
    if (!a->cfg.rom[0])
        return;
    j = (Job *)malloc(sizeof *j);
    if (!j)
        return;
    j->a = a;
    j->from = from;
    SDL_SetAtomicInt(&a->worker_busy, 1);
    a->worker = SDL_CreateThread(worker, "launcher first run", j);
    if (!a->worker) {
        free(j);
        SDL_SetAtomicInt(&a->worker_busy, 0);
    }
}

/* ---------- the game ---------- */
int app_launch_game(App *a, const char *what, const char *const *extra)
{
    const char *args[16];
    int n = 0;
    char human[32];
    if (a->demo)
        return 0;
    if (a->game) {
        app_toast(a, "The game is already running");
        return -1;
    }
    if (!exists(a->play_exe)) {
        app_toast(a, "btdd_play is missing next to the launcher");
        return -1;
    }
    args[n++] = a->play_exe;
    args[n++] = a->cfg.rom;
    while (extra && *extra && n < 15)
        args[n++] = *extra++;
    args[n] = NULL;
    new_log(a, "run", what, a->game_log, sizeof a->game_log);
    now_stamp(NULL, 0, human, sizeof human);
    log_line(a->game_log, "# started: %s\n", human);
    a->game = spawn_logged(args, a->game_log);
    if (!a->game) {
        log_line(a->game_log, "# result: %s\n", "could not start");
        app_toast(a, "Could not start the game");
        return -1;
    }
    a->game_started = SDL_GetTicks();
    snprintf(a->game_what, sizeof a->game_what, "%s", what);
    return 0;
}

void app_poll(App *a)
{
    int code = 0;
    if (a->game && SDL_WaitProcess(a->game, false, &code)) {
        char took[64], res[64];
        Uint64 s = (SDL_GetTicks() - a->game_started) / 1000;
        SDL_DestroyProcess(a->game);
        a->game = NULL;
        append_output(a->game_log);
        if (s >= 60)
            snprintf(took, sizeof took, "%d min", (int)(s / 60));
        else
            snprintf(took, sizeof took, "%d s", (int)s);
        snprintf(res, sizeof res, code == 0 ? "ok" : "exit %d", code);
        log_line(a->game_log, "# took: %s\n", took);
        log_line(a->game_log, "# result: %s\n", res);
        /* the game may have changed settings (F8/F9/F10, keys) */
        config_defaults(&a->cfg);
        config_load(&a->cfg);
        app_load_logs(a);
    }
    if (!SDL_GetAtomicInt(&a->worker_busy) && a->worker) {
        SDL_WaitThread(a->worker, NULL);
        a->worker = NULL;
        app_load_logs(a);
    }
}

/* ---------- logs ---------- */
static int cmp_desc(const void *x, const void *y) { return strcmp(*(char *const *)y, *(char *const *)x); }

void app_load_logs(App *a)
{
    char dir[1100];
    char **files;
    int n = 0, i;
    a->n_logs = 0;
    if (a->demo) { /* example rows for screenshots */
        static const char *const ex[4][4] = {{"24 Nov 17:31", "Play · 2P online (host)", "ok", "42 min"},
                                             {"24 Nov 17:02", "First run", "ok", ""},
                                             {"23 Nov 22:14", "Play", "exit 3", "15 min"},
                                             {"23 Nov 21:40", "Play", "ok", "28 min"}};
        for (i = 0; i < 4; i++) {
            LogEntry *e = &a->logs[i];
            memset(e, 0, sizeof *e);
            snprintf(e->when, sizeof e->when, "%s", ex[i][0]);
            snprintf(e->what, sizeof e->what, "%s", ex[i][1]);
            snprintf(e->result, sizeof e->result, "%s", ex[i][2]);
            snprintf(e->took, sizeof e->took, "%s", ex[i][3]);
        }
        a->n_logs = 4;
        return;
    }
    logs_dir(a, dir, sizeof dir);
    files = SDL_GlobDirectory(dir, "*.log", 0, &n);
    if (!files)
        return;
    qsort(files, (size_t)n, sizeof *files, cmp_desc);
    for (i = 0; i < n && a->n_logs < MAX_LOGS; i++) {
        LogEntry *e = &a->logs[a->n_logs];
        const char *name = files[i], *dash;
        FILE *f;
        char line[512];
        memset(e, 0, sizeof *e);
        snprintf(e->file, sizeof e->file, "%s" SEP "%s", dir, name);
        dash = strchr(name, '-');
        if (dash && strlen(dash) >= 14) { /* -YYYYMMDD-HHMMSS */
            int mo = (dash[5] - '0') * 10 + (dash[6] - '0'), d = (dash[7] - '0') * 10 + (dash[8] - '0');
            snprintf(e->when, sizeof e->when, "%d %s %.2s:%.2s", d, MONTHS[(mo + 11) % 12], dash + 10, dash + 12);
        }
        snprintf(e->result, sizeof e->result, "running");
        if ((f = fopen(e->file, "r"))) {
            while (fgets(line, sizeof line, f)) {
                line[strcspn(line, "\r\n")] = 0;
                if (!strncmp(line, "# what: ", 8))
                    snprintf(e->what, sizeof e->what, "%s", line + 8);
                else if (!strncmp(line, "# took: ", 8))
                    snprintf(e->took, sizeof e->took, "%s", line + 8);
                else if (!strncmp(line, "# result: ", 10))
                    snprintf(e->result, sizeof e->result, "%s", line + 10);
            }
            fclose(f);
        }
        a->n_logs++;
    }
    SDL_free(files);
}

/* copies a text file, replacing the user's profile folder and name */
static int copy_masked(const char *src, const char *dst)
{
    size_t n = 0;
    char *buf = (char *)SDL_LoadFile(src, &n);
    const char *prof = SDL_getenv("USERPROFILE"), *user = SDL_getenv("USERNAME");
    FILE *f;
    size_t i;
    if (!buf)
        return -1;
    if (!(f = fopen(dst, "wb"))) {
        SDL_free(buf);
        return -1;
    }
    if (!prof)
        prof = SDL_getenv("HOME");
    if (!user)
        user = SDL_getenv("USER");
    for (i = 0; i < n;) {
        size_t pl = prof ? strlen(prof) : 0, ul = user ? strlen(user) : 0;
        if (pl > 3 && i + pl <= n && !SDL_strncasecmp(buf + i, prof, pl)) {
            fputs("%USERPROFILE%", f);
            i += pl;
        } else if (ul > 2 && i + ul <= n && !SDL_strncasecmp(buf + i, user, ul)) {
            fputs("<user>", f);
            i += ul;
        } else
            fputc(buf[i++], f);
    }
    fclose(f);
    SDL_free(buf);
    return 0;
}

int app_save_report(App *a, char *out_dir, size_t len)
{
    char stamp[32], dst[1300], ini[1100];
    int i;
    now_stamp(stamp, sizeof stamp, NULL, 0);
    snprintf(out_dir, len, "%s" SEP "reports", a->data_dir);
    SDL_CreateDirectory(out_dir);
    snprintf(out_dir, len, "%s" SEP "reports" SEP "report-%s", a->data_dir, stamp);
    if (!SDL_CreateDirectory(out_dir))
        return -1;
    for (i = 0; i < a->n_logs && i < 6; i++) {
        const char *base = strrchr(a->logs[i].file, SEP[0]);
        snprintf(dst, sizeof dst, "%s" SEP "%s", out_dir, base ? base + 1 : a->logs[i].file);
        copy_masked(a->logs[i].file, dst);
    }
    snprintf(dst, sizeof dst, "%s" SEP "btdd.cfg", out_dir);
    copy_masked(a->cfg.path, dst);
    ini_path(a, ini, sizeof ini);
    snprintf(dst, sizeof dst, "%s" SEP "launcher.ini", out_dir);
    copy_masked(ini, dst);
    return 0;
}

/* ---------- save slots, cache ---------- */
int app_slot_info(const App *a, int slot, char *when, size_t len)
{
    char p[1300];
    SDL_PathInfo pi;
    SDL_DateTime d;
    if (a->demo) {
        static const int used[10] = {1, 1, 0, 1, 1, 0, 0, 0, 0, 0};
        if (used[slot])
            snprintf(when, len, "%d Nov", 20 + slot);
        return used[slot];
    }
    if (!a->rom_sha1[0])
        return 0;
    snprintf(p, sizeof p, "%s" SEP "states" SEP "%s" SEP "slot%d.state", a->data_dir, a->rom_sha1, slot);
    if (!SDL_GetPathInfo(p, &pi) || pi.type != SDL_PATHTYPE_FILE)
        return 0;
    SDL_TimeToDateTime(pi.modify_time, &d, true);
    snprintf(when, len, "%d %s", d.day, MONTHS[(d.month - 1) % 12]);
    return 1;
}

static void cache_dir(char *out, size_t len)
{
    const char *la = SDL_getenv("LOCALAPPDATA");
    if (la)
        snprintf(out, len, "%s" SEP "btdd" SEP "cache", la);
    else {
        const char *h = SDL_getenv("HOME");
        snprintf(out, len, "%s/.cache/btdd", h ? h : ".");
    }
}

#define ENUM_GO_ON ((SDL_EnumerationResult)0) /* SDL: keep enumerating */
typedef struct Sum {
    Uint64 bytes;
    int files;
    int remove;
} Sum;

static SDL_EnumerationResult SDLCALL sum_cb(void *ud, const char *dir, const char *name)
{
    Sum *s = (Sum *)ud;
    char p[1400];
    SDL_PathInfo pi;
    snprintf(p, sizeof p, "%s%s", dir, name);
    if (!SDL_GetPathInfo(p, &pi))
        return ENUM_GO_ON;
    if (pi.type == SDL_PATHTYPE_DIRECTORY)
        SDL_EnumerateDirectory(p, sum_cb, s);
    else {
        s->bytes += pi.size;
        s->files++;
    }
    if (s->remove)
        SDL_RemovePath(p);
    return ENUM_GO_ON;
}

void app_cache_info(const App *a, char *out, size_t len)
{
    char d[1100];
    Sum s = {0, 0, 0};
    if (a->demo) {
        snprintf(out, len, "1.8 MB in your cache folder");
        return;
    }
    cache_dir(d, sizeof d);
    if (!exists(d) || !SDL_EnumerateDirectory(d, sum_cb, &s) || !s.files)
        snprintf(out, len, "empty");
    else
        snprintf(out, len, "%.1f MB in %d files", (double)s.bytes / (1024.0 * 1024.0), s.files);
}

int app_clear_cache(App *a)
{
    char d[1100];
    Sum s = {0, 0, 1};
    (void)a;
    cache_dir(d, sizeof d);
    if (!exists(d))
        return 0;
    return SDL_EnumerateDirectory(d, sum_cb, &s) ? 0 : -1;
}
