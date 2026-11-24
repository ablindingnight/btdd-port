#include "config.h"
#include "video.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const char *const config_button_names[BTN_COUNT] = {"a", "b", "select", "start", "up", "down", "left", "right"};

void config_defaults(Config *c)
{
    static const SDL_Scancode p1[BTN_COUNT] = {
        SDL_SCANCODE_X, SDL_SCANCODE_Z, SDL_SCANCODE_RSHIFT, SDL_SCANCODE_RETURN,
        SDL_SCANCODE_UP, SDL_SCANCODE_DOWN, SDL_SCANCODE_LEFT, SDL_SCANCODE_RIGHT,
    };
    static const SDL_Scancode p2[BTN_COUNT] = {
        SDL_SCANCODE_H, SDL_SCANCODE_G, SDL_SCANCODE_T, SDL_SCANCODE_Y,
        SDL_SCANCODE_W, SDL_SCANCODE_S, SDL_SCANCODE_A, SDL_SCANCODE_D,
    };
    memset(c, 0, sizeof *c);
    c->scale = 3;
    c->volume = 1.0f;
    c->filter = FILTER_NEAREST;
    snprintf(c->renderer, sizeof c->renderer, "auto");
    c->rewind_seconds = 60;
    c->ff_speed = 4;
    memcpy(c->keys[0], p1, sizeof p1);
    memcpy(c->keys[1], p2, sizeof p2);
}

static void trim(char *s)
{
    char *e;
    while (*s == ' ' || *s == '\t')
        memmove(s, s + 1, strlen(s));
    e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' || e[-1] == '\n'))
        *--e = 0;
}

static void apply(Config *c, const char *key, const char *val)
{
    int p, b;
    if (!strcmp(key, "rom"))
        snprintf(c->rom, sizeof c->rom, "%s", val);
    else if (!strcmp(key, "scale"))
        c->scale = atoi(val) < 1 ? 1 : atoi(val) > 10 ? 10 : atoi(val);
    else if (!strcmp(key, "aspect"))
        c->aspect = atoi(val) != 0;
    else if (!strcmp(key, "fullscreen"))
        c->fullscreen = atoi(val) != 0;
    else if (!strcmp(key, "ppu"))
        c->ppu_new = !strcmp(val, "new");
    else if (!strcmp(key, "filter")) {
        int f = video_filter_from_name(val);
        if (f >= 0)
            c->filter = f;
    } else if (!strcmp(key, "scanlines"))
        c->scanlines = atoi(val) != 0;
    else if (!strcmp(key, "palette"))
        snprintf(c->palette, sizeof c->palette, "%s", val);
    else if (!strcmp(key, "renderer"))
        snprintf(c->renderer, sizeof c->renderer, "%s", val[0] ? val : "auto");
    else if (!strcmp(key, "volume"))
        c->volume = (float)SDL_clamp(atof(val), 0.0, 1.0);
    else if (!strcmp(key, "rewind_seconds"))
        c->rewind_seconds = SDL_clamp(atoi(val), 0, 600);
    else if (!strcmp(key, "ff_speed"))
        c->ff_speed = SDL_clamp(atoi(val), 2, 16);
    else
        for (p = 0; p < 2; p++)
            for (b = 0; b < BTN_COUNT; b++) {
                char name[32];
                snprintf(name, sizeof name, "p%d_%s", p + 1, config_button_names[b]);
                if (!strcmp(key, name)) {
                    SDL_Scancode sc = SDL_GetScancodeFromName(val);
                    if (sc != SDL_SCANCODE_UNKNOWN)
                        c->keys[p][b] = sc;
                    else
                        SDL_Log("btdd.cfg: unknown key name \"%s\" for %s", val, name);
                }
            }
}

static int write_file(const Config *c, const char *path);

int config_save(const Config *c)
{
    return write_file(c, c->path);
}

static int write_file(const Config *c, const char *path)
{
    FILE *f = fopen(path, "w");
    int p, b;
    if (!f)
        return -1;
    fprintf(f, "# btdd_play settings. Key names are SDL key names (e.g. X, Return, Left Shift, Up).\n");
    fprintf(f, "# rom = full path to your Battletoads-Double Dragon (USA).nes (or pass it on the command line)\n");
    fprintf(f, "rom = %s\n", c->rom);
    fprintf(f, "scale = %d\naspect = %d\nfullscreen = %d\nppu = %s\nvolume = %.2f\n", c->scale, c->aspect,
            c->fullscreen, c->ppu_new ? "new" : "old", c->volume);
    fprintf(f, "# filter = nearest | sharp | bilinear; scanlines = 0 | 1\n");
    fprintf(f, "filter = %s\nscanlines = %d\n", video_filter_names[c->filter], c->scanlines);
    fprintf(f, "# palette = path to a .pal file (64 or 512 RGB colors); empty = built-in palette\n");
    fprintf(f, "palette = %s\n", c->palette);
    fprintf(f, "# renderer = auto, direct3d11, direct3d12, direct3d, opengl, vulkan or software\n");
    fprintf(f, "renderer = %s\n", c->renderer);
    fprintf(f, "# rewind_seconds = how far Backspace can rewind (0 = off; ~1 MB per second); ff_speed = Tab speed\n");
    fprintf(f, "rewind_seconds = %d\nff_speed = %d\n", c->rewind_seconds, c->ff_speed);
    for (p = 0; p < 2; p++)
        for (b = 0; b < BTN_COUNT; b++)
            fprintf(f, "p%d_%s = %s\n", p + 1, config_button_names[b], SDL_GetScancodeName(c->keys[p][b]));
    return fclose(f);
}

static int read_file(Config *c, const char *path)
{
    FILE *f = fopen(path, "r");
    char line[1200];
    if (!f)
        return -1;
    while (fgets(line, sizeof line, f)) {
        char *eq;
        trim(line);
        if (!line[0] || line[0] == '#' || !(eq = strchr(line, '=')))
            continue;
        *eq = 0;
        trim(line);
        trim(eq + 1);
        apply(c, line, eq + 1);
    }
    fclose(f);
    return 0;
}

int config_load(Config *c)
{
    const char *base = SDL_GetBasePath();
    char *pref;
    config_defaults(c);
    if (base) {
        snprintf(c->path, sizeof c->path, "%sbtdd.cfg", base);
        if (read_file(c, c->path) == 0)
            return 0;
    }
    pref = SDL_GetPrefPath(NULL, "btdd");
    if (!pref)
        return -1;
    snprintf(c->path, sizeof c->path, "%sbtdd.cfg", pref);
    SDL_free(pref);
    if (read_file(c, c->path) == 0)
        return 0;
    return write_file(c, c->path);
}
