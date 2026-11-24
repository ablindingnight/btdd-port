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
    t1 = SDL_GetTicksNS();
    SDL_Log("window created in %llu ms", (unsigned long long)((t1 - t0) / 1000000));
    v->preset = preset;
    if (use_gpu && (v->gpu = gpu_open(v->win)) != NULL)
        return 0;
    v->ren = SDL_CreateRenderer(v->win, (renderer && renderer[0] && SDL_strcasecmp(renderer, "auto")) ? renderer : NULL);
    if (!v->ren)
        v->ren = SDL_CreateRenderer(v->win, NULL); /* unknown driver name: fall back */
    if (!v->ren)
        return -1;
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
    float pic_w = v->aspect ? NES_H * 4.0f / 3.0f : (float)NES_W, pic_h = NES_H;
    if (v->filter == FILTER_NEAREST && !v->aspect) {
        int k = SDL_min(ww / NES_W, wh / NES_H);
        if (k < 1)
            k = 1;
        r.w = (float)(NES_W * k);
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
    v->big = SDL_CreateTexture(v->ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_TARGET, NES_W * k, NES_H * k);
    v->big_scale = v->big ? k : 0;
}

/* GPU path: the chain draws the picture; text goes through an RGBA overlay at twice the NES resolution. */
static void present_gpu(Video *v, const uint32_t *pixels, const char *const *text, int text_lines)
{
    int ww, wh, i, keep = v->filter;
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
        SDL_SetRenderDrawColor(r, 0, 0, 0, 200);
        {
            SDL_FRect box = {0, 0, NES_W, (float)(text_lines * 10 + 6)};
            SDL_RenderFillRect(r, &box);
        }
        SDL_SetRenderDrawColor(r, 255, 255, 255, 255);
        for (i = 0; i < text_lines; i++)
            SDL_RenderDebugText(r, 4, 4 + i * 10.0f, text[i]);
        SDL_SetRenderScale(r, 1, 1);
        SDL_RenderPresent(r);
    }
    gpu_present(v->gpu, pixels, (GpuPreset)v->preset, dst, text_lines > 0 && v->overlay_ren ? v->overlay : NULL);
}

void video_present(Video *v, const uint32_t *pixels, const char *const *text, int text_lines)
{
    int ww, wh, i;
    SDL_FRect dst;
    SDL_Texture *src = v->tex;

    if (v->gpu) {
        present_gpu(v, pixels, text, text_lines);
        return;
    }

    SDL_UpdateTexture(v->tex, NULL, pixels, NES_W * (int)sizeof(uint32_t));
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
                    SDL_FRect line = {0, (float)(i * k + k - SDL_max(1, k / 3)), (float)(NES_W * k),
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
        SDL_SetRenderDrawBlendMode(v->ren, SDL_BLENDMODE_BLEND);
        SDL_SetRenderDrawColor(v->ren, 0, 0, 0, 200);
        {
            SDL_FRect box = {dst.x / scale, dst.y / scale, NES_W, (float)(text_lines * 10 + 6)};
            SDL_RenderFillRect(v->ren, &box);
        }
        SDL_SetRenderDrawColor(v->ren, 255, 255, 255, 255);
        for (i = 0; i < text_lines; i++)
            SDL_RenderDebugText(v->ren, dst.x / scale + 4, dst.y / scale + 4 + i * 10.0f, text[i]);
        SDL_SetRenderScale(v->ren, 1, 1);
    }
    SDL_RenderPresent(v->ren);
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
