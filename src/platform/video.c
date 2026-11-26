#include "video.h"
#include <string.h>

#define NES_W 256
#define NES_H 240

const char *const video_filter_names[FILTER_COUNT] = {"nearest", "sharp", "bilinear"};

int video_filter_from_name(const char *name)
{
    int i;
    for (i = 0; i < FILTER_COUNT; i++)
        if (!SDL_strcasecmp(name, video_filter_names[i]))
            return i;
    return -1;
}

int video_open(Video *v, const char *title, int scale, int aspect, int fullscreen, int filter, int scanlines,
               const char *renderer, int use_gpu, int preset)
{
    Uint64 t0 = SDL_GetTicksNS(), t1;
    memset(v, 0, sizeof *v);
    v->filter = filter;
    v->aspect = aspect;
    v->scanlines = scanlines;
    v->win = SDL_CreateWindow(title, (aspect ? 292 : NES_W) * scale, NES_H * scale,
                              SDL_WINDOW_RESIZABLE | (fullscreen ? SDL_WINDOW_FULLSCREEN : 0));
    if (!v->win)
        return -1;
    video_set_icon(v->win);
    t1 = SDL_GetTicksNS();
    SDL_Log("window created in %llu ms", (unsigned long long)((t1 - t0) / 1000000));
    v->preset = preset;
    v->pic_w = NES_W;
    if (use_gpu && (v->gpu = gpu_open(v->win)) != NULL)
        return 0;
    v->ren = SDL_CreateRenderer(v->win, (renderer && renderer[0] && SDL_strcasecmp(renderer, "auto")) ? renderer : NULL);
    if (!v->ren)
        v->ren = SDL_CreateRenderer(v->win, NULL); /* unknown driver name: fall back */
    if (!v->ren)
        return -1;
    v->pic_w = NES_W;
    v->tex = SDL_CreateTexture(v->ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, NES_W, NES_H);
    if (!v->tex)
        return -1;
    SDL_Log("renderer: %s (created in %llu ms)", SDL_GetRendererName(v->ren),
            (unsigned long long)((SDL_GetTicksNS() - t1) / 1000000));
    return 0;
}

/* Where the picture goes in the window: largest integer multiple for square pixels in nearest mode,
 * otherwise the largest 256x240 (or 4:3) rectangle that fits. */
static SDL_FRect output_rect(const Video *v, int ww, int wh)
{
    SDL_FRect r;
    int w = v->pic_w ? v->pic_w : NES_W; /* 256, or 424 with the M9 widescreen mod */
    float pic_w = v->aspect ? w * (NES_H * 4.0f / 3.0f) / NES_W : (float)w, pic_h = NES_H;
    if (v->filter == FILTER_NEAREST && !v->aspect) {
        int k = SDL_min(ww / w, wh / NES_H);
        if (k < 1)
            k = 1;
        r.w = (float)(w * k);
        r.h = (float)(NES_H * k);
    } else {
        float k = SDL_min(ww / pic_w, wh / pic_h);
        r.w = pic_w * k;
        r.h = pic_h * k;
    }
    r.x = (ww - r.w) / 2;
    r.y = (wh - r.h) / 2;
    return r;
}

static void ensure_big(Video *v, int k)
{
    if (v->big && v->big_scale == k)
        return;
    if (v->big)
        SDL_DestroyTexture(v->big);
    v->big = SDL_CreateTexture(v->ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_TARGET, v->pic_w * k, NES_H * k);
    v->big_scale = v->big ? k : 0;
}

/* ---- text overlay in the launcher's look (docs/LAUNCHER_STYLE.md), in NES pixels, 8x8 debug font ---- */
enum { TEXT_TOAST, TEXT_MENU };
static void fill(SDL_Renderer *r, float x, float y, float w, float h, Uint8 cr, Uint8 cg, Uint8 cb, Uint8 ca)
{
    SDL_FRect f = {x, y, w, h};
    SDL_SetRenderDrawColor(r, cr, cg, cb, ca);
    SDL_RenderFillRect(r, &f);
}
#define INK 0x0b, 0x0b, 0x14
#define PAPER 0xff, 0xf8, 0xe7
#define YELLOW 0xff, 0xd2, 0x1f
#define RED 0xff, 0x4a, 0x1c
#define CYAN 0x5f, 0xd8, 0xff
#define GREY 0xc9, 0xc3, 0xb4

static void draw_overlay(SDL_Renderer *r, float x0, float y0, const char *const *text, int n, int style)
{
    int i;
    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
    if (style == TEXT_TOAST) { /* each line in its own cyan sticker */
        for (i = 0; i < n; i++) {
            float w = (float)SDL_strlen(text[i]) * 8 + 8, y = y0 + 4 + (float)i * 14;
            if (!text[i][0])
                continue;
            fill(r, x0 + 5, y + 1, w + 2, 12, INK, 255);        /* offset shadow */
            fill(r, x0 + 3, y - 1, w + 2, 12, INK, 255);        /* border */
            fill(r, x0 + 4, y, w, 10, CYAN, 255);
            SDL_SetRenderDrawColor(r, INK, 255);
            SDL_RenderDebugText(r, x0 + 8, y + 1, text[i]);
        }
        return;
    }
    {
        int help = n; /* help = the lines after the last blank line (index > 1) */
        float top = y0 + 10, h, y;
        for (i = n - 1; i > 1; i--)
            if (!text[i][0]) {
                help = i + 1;
                break;
            }
        h = (float)(n - 1) * 10 + 10;
        fill(r, x0 + 5, top + 3, 250, h, RED, 255);       /* red offset shadow */
        fill(r, x0 + 2, top, 250, h, YELLOW, 255);        /* 2 px yellow border */
        fill(r, x0 + 4, top + 2, 246, h - 4, INK, 240);   /* ink body */
        if (n > 0) {                                      /* title tab on the top border */
            float tw = (float)SDL_strlen(text[0]) * 8 + 8;
            fill(r, x0 + 8, y0 + 2, tw + 4, 14, INK, 255);
            fill(r, x0 + 9, y0 + 3, tw + 2, 12, YELLOW, 255);
            SDL_SetRenderDrawColor(r, INK, 255);
            SDL_RenderDebugText(r, x0 + 13, y0 + 5, text[0]);
        }
        y = top + 9;
        for (i = 2; i < n; i++, y += 10) {
            const char *t = text[i];
            if (i >= help) {
                if (i == help) { /* dotted rule above the help text */
                    float dx;
                    for (dx = x0 + 8; dx < x0 + 246; dx += 4)
                        fill(r, dx, y - 4, 2, 1, PAPER, 90);
                }
                SDL_SetRenderDrawColor(r, GREY, 255);
                SDL_RenderDebugText(r, x0 + 8, y, t);
                continue;
            }
            if (!t[0])
                continue;
            if (t[0] == '>') { /* the selected item: yellow bar, ink text, red pointer */
                fill(r, x0 + 4, y - 1, 246, 10, YELLOW, 255);
                fill(r, x0 + 6, y, 2, 8, RED, 255);
                fill(r, x0 + 8, y + 1, 2, 6, RED, 255);
                fill(r, x0 + 10, y + 2, 2, 4, RED, 255);
                fill(r, x0 + 12, y + 3, 2, 2, RED, 255);
                SDL_SetRenderDrawColor(r, INK, 255);
            } else
                SDL_SetRenderDrawColor(r, PAPER, 255);
            SDL_RenderDebugText(r, x0 + 16, y, t[0] && t[1] ? t + 2 : t);
        }
    }
}

/* GPU path: the chain draws the picture; text goes through an RGBA overlay at twice the NES resolution. */
static void present_gpu(Video *v, const uint32_t *pixels, const char *const *text, int text_lines, int style)
{
    int ww, wh, keep = v->filter;
    SDL_FRect dst;
    SDL_GetWindowSizeInPixels(v->win, &ww, &wh);
    v->filter = v->preset == GPU_PRESET_NEAREST ? FILTER_NEAREST : FILTER_SHARP;
    dst = output_rect(v, ww, wh);
    v->filter = keep;
    if (text_lines > 0 && !v->overlay) {
        v->overlay = SDL_CreateSurface(NES_W * 2, NES_H * 2, SDL_PIXELFORMAT_ARGB8888);
        v->overlay_ren = v->overlay ? SDL_CreateSoftwareRenderer(v->overlay) : NULL;
    }
    if (text_lines > 0 && v->overlay_ren) {
        SDL_Renderer *r = v->overlay_ren;
        SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_NONE);
        SDL_SetRenderDrawColor(r, 0, 0, 0, 0);
        SDL_RenderClear(r);
        SDL_SetRenderScale(r, 2, 2);
        draw_overlay(r, 0, 0, text, text_lines, style);
        SDL_SetRenderScale(r, 1, 1);
        SDL_RenderPresent(r);
    }
    gpu_present(v->gpu, pixels, (GpuPreset)v->preset, dst, text_lines > 0 && v->overlay_ren ? v->overlay : NULL);
}

static void present(Video *v, const uint32_t *pixels, const char *const *text, int text_lines, int style)
{
    int ww, wh, i;
    SDL_FRect dst;
    SDL_Texture *src = v->tex;

    if (v->gpu) {
        present_gpu(v, pixels, text, text_lines, style);
        return;
    }

    SDL_UpdateTexture(v->tex, NULL, pixels, v->pic_w * (int)sizeof(uint32_t));
    SDL_GetRenderOutputSize(v->ren, &ww, &wh);
    dst = output_rect(v, ww, wh);

    /* integer-scaled stage: for the sharp filter, and for scanlines in every mode */
    if (v->filter == FILTER_SHARP || v->scanlines) {
        int k = (int)SDL_ceilf(dst.h / NES_H);
        if (v->filter == FILTER_BILINEAR || v->filter == FILTER_NEAREST)
            k = SDL_max(2, (int)(dst.h / NES_H)); /* scanlines need at least 2 output rows per NES row */
        k = SDL_clamp(k, 1, 8);
        ensure_big(v, k);
        if (v->big) {
            SDL_SetRenderTarget(v->ren, v->big);
            SDL_SetTextureScaleMode(v->tex, SDL_SCALEMODE_NEAREST);
            SDL_RenderTexture(v->ren, v->tex, NULL, NULL);
            if (v->scanlines && k >= 2) {
                SDL_SetRenderDrawBlendMode(v->ren, SDL_BLENDMODE_BLEND);
                SDL_SetRenderDrawColor(v->ren, 0, 0, 0, 96);
                for (i = 0; i < NES_H; i++) {
                    SDL_FRect line = {0, (float)(i * k + k - SDL_max(1, k / 3)), (float)(v->pic_w * k),
                                      (float)SDL_max(1, k / 3)};
                    SDL_RenderFillRect(v->ren, &line);
                }
            }
            SDL_SetRenderTarget(v->ren, NULL);
            src = v->big;
        }
    }
    SDL_SetTextureScaleMode(src, v->filter == FILTER_NEAREST ? SDL_SCALEMODE_NEAREST : SDL_SCALEMODE_LINEAR);

    SDL_SetRenderDrawColor(v->ren, 0, 0, 0, 255);
    SDL_RenderClear(v->ren);
    SDL_RenderTexture(v->ren, src, NULL, &dst);
    if (text_lines > 0) {
        float scale = SDL_max(1.0f, SDL_floorf(dst.h / NES_H));
        SDL_SetRenderScale(v->ren, scale, scale);
        draw_overlay(v->ren, dst.x / scale, dst.y / scale, text, text_lines, style);
        SDL_SetRenderScale(v->ren, 1, 1);
    }
    SDL_RenderPresent(v->ren);
}

void video_set_icon(SDL_Window *w)
{
    const char *base = SDL_GetBasePath();
    char path[1024];
    SDL_Surface *s;
    if (!base)
        return;
    SDL_snprintf(path, sizeof path, "%sart/icon.png", base);
    if ((s = SDL_LoadPNG(path)) != NULL) {
        SDL_SetWindowIcon(w, s);
        SDL_DestroySurface(s);
    }
}

void video_draw_overlay(SDL_Renderer *r, float x0, float y0, const char *const *text, int n, int menu)
{
    draw_overlay(r, x0, y0, text, n, menu ? TEXT_MENU : TEXT_TOAST);
}

void video_present(Video *v, const uint32_t *pixels, const char *const *text, int text_lines)
{
    present(v, pixels, text, text_lines, TEXT_TOAST);
}

void video_present_menu(Video *v, const uint32_t *pixels, const char *const *text, int text_lines)
{
    present(v, pixels, text, text_lines, TEXT_MENU);
}

int video_set_width(Video *v, int w)
{
    SDL_Texture *t;
    if (w == v->pic_w)
        return 0;
    if (v->gpu) { /* GPU shader path: the chain takes any width */
        if (gpu_set_frame_width(v->gpu, w) != 0)
            return -1;
        if (!(SDL_GetWindowFlags(v->win) & (SDL_WINDOW_FULLSCREEN | SDL_WINDOW_MAXIMIZED))) {
            int ww, wh;
            SDL_GetWindowSize(v->win, &ww, &wh);
            SDL_SetWindowSize(v->win, ww * w / (v->pic_w ? v->pic_w : NES_W), wh);
        }
        v->pic_w = w;
        return 0;
    }
    if (!v->ren)
        return -1;
    t = SDL_CreateTexture(v->ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, w, NES_H);
    if (!t)
        return -1;
    SDL_DestroyTexture(v->tex);
    v->tex = t;
    if (!(SDL_GetWindowFlags(v->win) & (SDL_WINDOW_FULLSCREEN | SDL_WINDOW_MAXIMIZED))) {
        int ww, wh; /* keep the height, follow the picture's width */
        SDL_GetWindowSize(v->win, &ww, &wh);
        SDL_SetWindowSize(v->win, ww * w / (v->pic_w ? v->pic_w : NES_W), wh);
    }
    v->pic_w = w;
    if (v->big)
        SDL_DestroyTexture(v->big);
    v->big = NULL;
    v->big_scale = 0;
    return 0;
}

void video_close(Video *v)
{
    if (v->overlay_ren)
        SDL_DestroyRenderer(v->overlay_ren);
    if (v->overlay)
        SDL_DestroySurface(v->overlay);
    if (v->gpu)
        gpu_close(v->gpu);
    if (v->big)
        SDL_DestroyTexture(v->big);
    if (v->tex)
        SDL_DestroyTexture(v->tex);
    if (v->ren)
        SDL_DestroyRenderer(v->ren);
    if (v->win)
        SDL_DestroyWindow(v->win);
    memset(v, 0, sizeof *v);
}
