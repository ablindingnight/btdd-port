/* First-start recompilation and the compiled-code cache (M5).
 *
 * native_jit_load(): for the loaded ROM, find a compiled native library in the cache, or make one:
 *   1. key = SHA-1(PRG bytes + btdd.meta bytes + exe version + compiler id); folder
 *      %LOCALAPPDATA%\btdd\cache\<key>\ (XDG cache dir elsewhere), never inside the repo
 *   2. if <folder>\native.dll is missing: btdd_gen_write() writes the C there (in-process, recomp/gen.c), then
 *      the compiler turns it into native.dll:
 *        - libtcc (BTDD_LIBTCC builds; libtcc.dll stays a separate, dynamically linked DLL: LGPL), or
 *        - for development without TinyCC: the command in BTDD_CC (e.g. "zig cc -O2")
 *   3. load the library, check that its struct layout matches ours, call native_register_all(api).
 * The generated C is ROM-derived; it lives only in the player's cache folder.
 */
#include "native_rt.h"
#include "rom/sha1.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#include <direct.h>
#include <windows.h>
#define MKDIR(p) _mkdir(p)
#else
#include <dlfcn.h>
#include <sys/stat.h>
#define MKDIR(p) mkdir(p, 0755)
#endif

#ifdef BTDD_LIBTCC
#include "libtcc.h"
#endif

#ifndef BTDD_SOURCE_DIR
#define BTDD_SOURCE_DIR "."
#endif
#ifndef BTDD_TREE_DIR /* where src/ comes from (a merged tree may differ from the recomp/ checkout) */
#define BTDD_TREE_DIR BTDD_SOURCE_DIR
#endif
#ifndef BTDD_EXE_VERSION
#define BTDD_EXE_VERSION "m5-dev " __DATE__ " " __TIME__ /* the shell scripts pass commit + source hash instead */
#endif

/* Where meta/, src/, recomp/ and TinyCC's tcc/ are: next to the exe in a release package (meta/btdd.meta present
 * there), else the source tree the exe was built from. */
static const char *pkg_root(void)
{
    static char root[1100];
    static int done;
    if (!done) {
        char probe[1200];
        FILE *f;
        done = 1;
#ifdef _WIN32
        {
            DWORD n = GetModuleFileNameA(NULL, root, (DWORD)sizeof root);
            char *slash;
            if (n == 0 || n >= sizeof root)
                root[0] = 0;
            slash = strrchr(root, '\\');
            if (slash)
                *slash = 0;
        }
#else
        root[0] = 0;
#endif
        snprintf(probe, sizeof probe, "%s/meta/btdd.meta", root);
        if (!root[0] || !(f = fopen(probe, "rb")))
            root[0] = 0;
        else
            fclose(f);
    }
    return root;
}
#define SRC_ROOT (pkg_root()[0] ? pkg_root() : BTDD_SOURCE_DIR)
#define TREE_ROOT (pkg_root()[0] ? pkg_root() : BTDD_TREE_DIR)

int btdd_gen_write(const Rom *rom, const char *sha1_hex, const char *meta_path, const char *outdir,
                   const char *confs, const char *excl, int excl_timing, char *err, size_t errcap);

extern int btdd_gen_fast_blocks; /* recomp/gen.c */

/* generated code must see the same Cpu hooks as this host (see N_PRE/N_TICK in native_rt.h) */
#ifdef BTDD_CPU_HOOKS
#define HOOKS_FLAG "-DBTDD_CPU_HOOKS "
#else
#define HOOKS_FLAG ""
#endif

double native_jit_seconds_gen, native_jit_seconds_compile, native_jit_seconds_load;
int native_jit_cache_hit;

static double now_s(void)
{
    struct timespec ts;
    timespec_get(&ts, TIME_UTC);
    return (double)ts.tv_sec + ts.tv_nsec * 1e-9;
}

static int file_exists(const char *p)
{
    FILE *f = fopen(p, "rb");
    if (f)
        fclose(f);
    return f != NULL;
}

static uint8_t *read_all(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    uint8_t *buf;
    long n;
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    buf = malloc((size_t)n + 1);
    if (buf && fread(buf, 1, (size_t)n, f) != (size_t)n) {
        free(buf);
        buf = NULL;
    }
    fclose(f);
    *len = (size_t)n;
    return buf;
}

static const char *compiler_id(void)
{
#ifdef BTDD_LIBTCC
    return "libtcc";
#else
    const char *cc = getenv("BTDD_CC");
    return cc ? cc : "";
#endif
}

static void cache_root(char *out, size_t cap)
{
    const char *base = getenv("BTDD_CACHE_DIR");
    if (base) {
        snprintf(out, cap, "%s", base);
        return;
    }
#ifdef _WIN32
    base = getenv("LOCALAPPDATA");
    snprintf(out, cap, "%s\\btdd", base ? base : ".");
    MKDIR(out);
    snprintf(out + strlen(out), cap - strlen(out), "\\cache");
#else
    base = getenv("XDG_CACHE_HOME");
    if (base)
        snprintf(out, cap, "%s/btdd", base);
    else {
        base = getenv("HOME");
        snprintf(out, cap, "%s/.cache", base ? base : ".");
        MKDIR(out);
        snprintf(out + strlen(out), cap - strlen(out), "/btdd");
    }
#endif
    MKDIR(out);
}

static int compile(const char *dir, const char *dll, char *err, size_t cap)
{
    char path[1200];
    int b;
#ifdef BTDD_LIBTCC
    TCCState *s = tcc_new();
    int ok = 1;
    if (!s) {
        snprintf(err, cap, "tcc_new failed");
        return 1;
    }
    if (getenv("BTDD_TCC_LIB"))
        tcc_set_lib_path(s, getenv("BTDD_TCC_LIB"));
    else if (pkg_root()[0]) { /* release package: TinyCC's include/ and lib/ in tcc/ next to the exe */
        snprintf(path, sizeof path, "%s/tcc", pkg_root());
        tcc_set_lib_path(s, path);
    }
#ifdef BTDD_TCC_LIB_DIR
    else
        tcc_set_lib_path(s, BTDD_TCC_LIB_DIR);
#endif
    tcc_set_output_type(s, TCC_OUTPUT_DLL);
#ifdef BTDD_CPU_HOOKS
    tcc_define_symbol(s, "BTDD_CPU_HOOKS", "1");
#endif
    snprintf(path, sizeof path, "%s/src", TREE_ROOT);
    tcc_add_include_path(s, path);
    snprintf(path, sizeof path, "%s/recomp", SRC_ROOT);
    tcc_add_include_path(s, path);
    for (b = -1; b < 8 && ok; b++) {
        if (b < 0)
            snprintf(path, sizeof path, "%s/native_all.c", dir);
        else
            snprintf(path, sizeof path, "%s/native_b%d.c", dir, b);
        ok = tcc_add_file(s, path) == 0;
    }
    if (ok)
        ok = tcc_output_file(s, dll) == 0;
    tcc_delete(s);
    if (!ok)
        snprintf(err, cap, "libtcc could not compile %s", dir);
    return ok ? 0 : 1;
#else
    const char *cc = getenv("BTDD_CC");
    char cmd[4096];
    if (!cc || !*cc) {
        snprintf(err, cap, "no compiler: this build has no libtcc (BTDD_LIBTCC) and BTDD_CC is not set");
        return 1;
    }
    snprintf(cmd, sizeof cmd, "%s -shared -std=c11 -w %s-I\"%s/src\" -I\"%s/recomp\" -o \"%s\"", cc, HOOKS_FLAG,
             TREE_ROOT, SRC_ROOT, dll);
    for (b = -1; b < 8; b++) {
        if (b < 0)
            snprintf(path, sizeof path, " \"%s/native_all.c\"", dir);
        else
            snprintf(path, sizeof path, " \"%s/native_b%d.c\"", dir, b);
        snprintf(cmd + strlen(cmd), sizeof cmd - strlen(cmd), "%s", path);
    }
#ifdef _WIN32
    { /* cmd.exe strips one pair of outer quotes */
        char wrapped[4200];
        snprintf(wrapped, sizeof wrapped, "\"%s\"", cmd);
        b = system(wrapped);
    }
#else
    b = system(cmd);
#endif
    if (b != 0 || !file_exists(dll)) {
        snprintf(err, cap, "compiler command failed (%d): %s", b, cmd);
        return 1;
    }
    return 0;
#endif
}

typedef int (*RegisterAllFn)(const NativeApi *api);
typedef const size_t *(*LayoutFn)(void);

static int load_lib(const char *dll, const NativeApi *api, char *err, size_t cap)
{
    static const size_t ours[] = {
#define NATIVE_HOST_LAYOUT(e) (size_t)(e),
        NATIVE_LAYOUT_LIST(NATIVE_HOST_LAYOUT) 0};
    RegisterAllFn reg;
    LayoutFn layout;
    const size_t *theirs;
    size_t i;
#ifdef _WIN32
    HMODULE h = LoadLibraryA(dll);
    if (!h) {
        snprintf(err, cap, "cannot load %s (error %lu)", dll, (unsigned long)GetLastError());
        return 1;
    }
    reg = (RegisterAllFn)(void (*)(void))GetProcAddress(h, "native_register_all");
    layout = (LayoutFn)(void (*)(void))GetProcAddress(h, "native_layout");
#else
    void *h = dlopen(dll, RTLD_NOW | RTLD_LOCAL);
    if (!h) {
        snprintf(err, cap, "cannot load %s: %s", dll, dlerror());
        return 1;
    }
    *(void **)&reg = dlsym(h, "native_register_all");
    *(void **)&layout = dlsym(h, "native_layout");
#endif
    if (!reg || !layout) {
        snprintf(err, cap, "%s lacks native_register_all/native_layout", dll);
        return 1;
    }
    theirs = layout();
    for (i = 0; i < sizeof ours / sizeof ours[0]; i++)
        if (theirs[i] != ours[i]) {
            snprintf(err, cap, "%s was compiled with a different struct layout (item %u: %u vs %u)", dll,
                     (unsigned)i, (unsigned)theirs[i], (unsigned)ours[i]);
            return 1;
        }
    if (!reg(api)) {
        snprintf(err, cap, "%s: API version mismatch", dll);
        return 1;
    }
    return 0;
}

int native_jit_load(const Rom *rom, const NativeApi *api, char *err, size_t cap)
{
    const char *meta = getenv("BTDD_META");
    char meta_default[1024], root[1024], dir[1100], dll[1200], key[41];
    uint8_t digest[20], *meta_bytes, *buf;
    size_t meta_len, n, i;
    const char *ver = BTDD_EXE_VERSION, *conf = getenv("BTDD_CONF") ? getenv("BTDD_CONF") : "verified,static";
    const char *fb = getenv("BTDD_FAST_BLOCKS");
    char cid[600];
    double t0;

    /* everything that changes the generated code or how it is compiled goes into the cache key */
    btdd_gen_fast_blocks = fb && fb[0] == '1'; /* exact on both gates but no measurable gain: opt-in */
    snprintf(cid, sizeof cid, "%s|%s|fast=%d|%s", compiler_id(), conf, btdd_gen_fast_blocks, HOOKS_FLAG);

    if (!meta) {
        snprintf(meta_default, sizeof meta_default, "%s/meta/btdd.meta", SRC_ROOT);
        meta = meta_default;
    }
    meta_bytes = read_all(meta, &meta_len);
    if (!meta_bytes) {
        snprintf(err, cap, "cannot read %s", meta);
        return 1;
    }
    {
        /* the runtime header the generated code is compiled against is part of the key too */
        char rt_path[1100];
        size_t rt_len = 0;
        uint8_t *rt;
        snprintf(rt_path, sizeof rt_path, "%s/recomp/native_rt.h", SRC_ROOT);
        rt = read_all(rt_path, &rt_len);
        n = sizeof rom->prg + meta_len + strlen(ver) + 1 + strlen(cid) + (rt ? rt_len : 0);
        buf = malloc(n);
        if (!buf)
            return 1;
        memcpy(buf, rom->prg, sizeof rom->prg);
        memcpy(buf + sizeof rom->prg, meta_bytes, meta_len);
        memcpy(buf + sizeof rom->prg + meta_len, ver, strlen(ver) + 1);
        memcpy(buf + sizeof rom->prg + meta_len + strlen(ver) + 1, cid, strlen(cid));
        if (rt)
            memcpy(buf + sizeof rom->prg + meta_len + strlen(ver) + 1 + strlen(cid), rt, rt_len);
        free(rt);
    }
    sha1(buf, n, digest);
    free(buf);
    free(meta_bytes);
    for (i = 0; i < 20; i++)
        sprintf(key + 2 * i, "%02x", digest[i]);

    cache_root(root, sizeof root);
    snprintf(dir, sizeof dir, "%s/%.16s", root, key);
    snprintf(dll, sizeof dll, "%s/native.dll", dir);
    native_jit_cache_hit = file_exists(dll);
    if (!native_jit_cache_hit) {
        MKDIR(dir);
        t0 = now_s();
        if (btdd_gen_write(rom, NULL, meta, dir, conf, "", 0, err, cap) != 0)
            return 1;
        native_jit_seconds_gen = now_s() - t0;
        t0 = now_s();
        if (compile(dir, dll, err, cap) != 0)
            return 1;
        native_jit_seconds_compile = now_s() - t0;
    }
    t0 = now_s();
    if (load_lib(dll, api, err, cap) != 0)
        return 1;
    native_jit_seconds_load = now_s() - t0;
    snprintf(err, cap, "%s %s", native_jit_cache_hit ? "cache hit" : "compiled", dll);
    return 0;
}
