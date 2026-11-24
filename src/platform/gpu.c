/* GPU shader path (see gpu.h). */
#include "gpu.h"
#include "gpu_shaders.h"
#include <string.h>

#define NES_W 256
#define NES_H 240
#define UP 2 /* upscale factor of the upscale pass */

const char *const gpu_preset_names[GPU_PRESET_COUNT] = {"nearest", "sharp", "bilinear", "crt", "smooth", "smooth-crt"};

int gpu_preset_from_name(const char *name)
{
    int i;
    for (i = 0; i < GPU_PRESET_COUNT; i++)
        if (!SDL_strcasecmp(name, gpu_preset_names[i]))
            return i;
    return -1;
}

enum { SH_COPY, SH_SHARP, SH_CRT, SH_UPSCALE, SH_COUNT };
static const char *const shader_names[SH_COUNT] = {"copy", "sharp", "crt", "upscale2x"};

#define MAX_PIPES 16
typedef struct Pipe {
    int shader;
    SDL_GPUTextureFormat format;
    int blend;
    SDL_GPUGraphicsPipeline *pipe;
} Pipe;

struct Gpu {
    SDL_GPUDevice *dev;
    SDL_Window *win;
    SDL_GPUShader *vert, *frag[SH_COUNT];
    Pipe pipes[MAX_PIPES];
    int pipe_count;
    SDL_GPUSampler *nearest, *linear;
    SDL_GPUTexture *frame;        /* frame_w x 240 picture */
    int frame_w;                  /* 256, or wider with the M9 widescreen mod */
    SDL_GPUTexture *up;           /* 512x480 upscale pass target */
    SDL_GPUTexture *overlay;      /* text overlay */
    int overlay_w, overlay_h;
    SDL_GPUTransferBuffer *upload, *overlay_upload;
};

typedef struct Params {
    float src_size[4], out_size[4], params[4], params2[4];
} Params;

static void set_size(float *v, float w, float h)
{
    v[0] = w;
    v[1] = h;
    v[2] = 1.0f / w;
    v[3] = 1.0f / h;
}

static SDL_GPUShader *make_shader(SDL_GPUDevice *dev, const char *name, int fragment, int samplers, int uniforms)
{
    int i;
    for (i = 0; i < GPU_SHADER_BLOB_COUNT; i++) {
        const GpuShaderBlob *b = &gpu_shader_blobs[i];
        if (b->fragment == fragment && !strcmp(b->name, name)) {
            SDL_GPUShaderCreateInfo ci;
            SDL_zero(ci);
            ci.code = b->code;
            ci.code_size = b->size;
            ci.entrypoint = "main";
            ci.format = SDL_GPU_SHADERFORMAT_DXBC;
            ci.stage = fragment ? SDL_GPU_SHADERSTAGE_FRAGMENT : SDL_GPU_SHADERSTAGE_VERTEX;
            ci.num_samplers = (Uint32)samplers;
            ci.num_uniform_buffers = (Uint32)uniforms;
            return SDL_CreateGPUShader(dev, &ci);
        }
    }
    SDL_SetError("shader %s not built in", name);
    return NULL;
}

static SDL_GPUGraphicsPipeline *pipeline(Gpu *g, int shader, SDL_GPUTextureFormat format, int blend)
{
    SDL_GPUGraphicsPipelineCreateInfo ci;
    SDL_GPUColorTargetDescription ct;
    int i;
    for (i = 0; i < g->pipe_count; i++)
        if (g->pipes[i].shader == shader && g->pipes[i].format == format && g->pipes[i].blend == blend)
            return g->pipes[i].pipe;
    if (g->pipe_count == MAX_PIPES)
        return NULL;
    SDL_zero(ci);
    SDL_zero(ct);
    ct.format = format;
    if (blend) {
        ct.blend_state.enable_blend = true;
        ct.blend_state.src_color_blendfactor = SDL_GPU_BLENDFACTOR_SRC_ALPHA;
        ct.blend_state.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
        ct.blend_state.color_blend_op = SDL_GPU_BLENDOP_ADD;
        ct.blend_state.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
        ct.blend_state.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
        ct.blend_state.alpha_blend_op = SDL_GPU_BLENDOP_ADD;
    }
    ci.vertex_shader = g->vert;
    ci.fragment_shader = g->frag[shader];
    ci.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
    ci.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
    ci.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
    ci.target_info.color_target_descriptions = &ct;
    ci.target_info.num_color_targets = 1;
    g->pipes[g->pipe_count].pipe = SDL_CreateGPUGraphicsPipeline(g->dev, &ci);
    if (!g->pipes[g->pipe_count].pipe)
        return NULL;
    g->pipes[g->pipe_count].shader = shader;
    g->pipes[g->pipe_count].format = format;
    g->pipes[g->pipe_count].blend = blend;
    return g->pipes[g->pipe_count++].pipe;
}

static SDL_GPUTexture *make_texture(SDL_GPUDevice *dev, int w, int h, SDL_GPUTextureFormat f, SDL_GPUTextureUsageFlags u)
{
    SDL_GPUTextureCreateInfo ci;
    SDL_zero(ci);
    ci.type = SDL_GPU_TEXTURETYPE_2D;
    ci.format = f;
    ci.usage = u;
    ci.width = (Uint32)w;
    ci.height = (Uint32)h;
    ci.layer_count_or_depth = 1;
    ci.num_levels = 1;
    return SDL_CreateGPUTexture(dev, &ci);
}

static SDL_GPUSampler *make_sampler(SDL_GPUDevice *dev, SDL_GPUFilter f)
{
    SDL_GPUSamplerCreateInfo ci;
    SDL_zero(ci);
    ci.min_filter = ci.mag_filter = f;
    ci.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
    ci.address_mode_u = ci.address_mode_v = ci.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    return SDL_CreateGPUSampler(dev, &ci);
}

static SDL_GPUTransferBuffer *make_transfer(SDL_GPUDevice *dev, Uint32 size, SDL_GPUTransferBufferUsage u)
{
    SDL_GPUTransferBufferCreateInfo ci;
    SDL_zero(ci);
    ci.usage = u;
    ci.size = size;
    return SDL_CreateGPUTransferBuffer(dev, &ci);
}

Gpu *gpu_open(SDL_Window *win)
{
    static const int samplers[SH_COUNT] = {1, 1, 1, 1};
    Gpu *g = SDL_calloc(1, sizeof *g);
    int i;
    if (!g)
        return NULL;
    g->dev = SDL_CreateGPUDevice(SDL_GPU_SHADERFORMAT_DXBC, false, NULL);
    if (!g->dev) {
        SDL_Log("gpu: no device with DXBC shaders (%s); using the SDL renderer", SDL_GetError());
        SDL_free(g);
        return NULL;
    }
    if (win) {
        if (!SDL_ClaimWindowForGPUDevice(g->dev, win)) {
            SDL_Log("gpu: cannot claim the window (%s)", SDL_GetError());
            gpu_close(g);
            return NULL;
        }
        g->win = win;
        SDL_SetGPUSwapchainParameters(g->dev, win, SDL_GPU_SWAPCHAINCOMPOSITION_SDR, SDL_GPU_PRESENTMODE_VSYNC);
    }
    g->vert = make_shader(g->dev, "fullscreen", 0, 0, 0);
    for (i = 0; i < SH_COUNT; i++)
        g->frag[i] = make_shader(g->dev, shader_names[i], 1, samplers[i], 1);
    g->nearest = make_sampler(g->dev, SDL_GPU_FILTER_NEAREST);
    g->linear = make_sampler(g->dev, SDL_GPU_FILTER_LINEAR);
    g->frame_w = NES_W;
    g->frame = make_texture(g->dev, NES_W, NES_H, SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM, SDL_GPU_TEXTUREUSAGE_SAMPLER);
    g->up = make_texture(g->dev, NES_W * UP, NES_H * UP, SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM,
                         SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COLOR_TARGET);
    g->upload = make_transfer(g->dev, NES_W * NES_H * 4, SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD);
    for (i = 0; i < SH_COUNT; i++)
        if (!g->frag[i])
            break;
    if (!g->vert || i < SH_COUNT || !g->nearest || !g->linear || !g->frame || !g->up || !g->upload) {
        SDL_Log("gpu: setup failed (%s); using the SDL renderer", SDL_GetError());
        gpu_close(g);
        return NULL;
    }
    SDL_Log("gpu: %s, shader chain ready", SDL_GetGPUDeviceDriver(g->dev));
    return g;
}

void gpu_close(Gpu *g)
{
    int i;
    if (!g)
        return;
    if (g->dev) {
        SDL_WaitForGPUIdle(g->dev);
        for (i = 0; i < g->pipe_count; i++)
            SDL_ReleaseGPUGraphicsPipeline(g->dev, g->pipes[i].pipe);
        for (i = 0; i < SH_COUNT; i++)
            if (g->frag[i])
                SDL_ReleaseGPUShader(g->dev, g->frag[i]);
        if (g->vert)
            SDL_ReleaseGPUShader(g->dev, g->vert);
        if (g->nearest)
            SDL_ReleaseGPUSampler(g->dev, g->nearest);
        if (g->linear)
            SDL_ReleaseGPUSampler(g->dev, g->linear);
        if (g->frame)
            SDL_ReleaseGPUTexture(g->dev, g->frame);
        if (g->up)
            SDL_ReleaseGPUTexture(g->dev, g->up);
        if (g->overlay)
            SDL_ReleaseGPUTexture(g->dev, g->overlay);
        if (g->upload)
            SDL_ReleaseGPUTransferBuffer(g->dev, g->upload);
        if (g->overlay_upload)
            SDL_ReleaseGPUTransferBuffer(g->dev, g->overlay_upload);
        if (g->win)
            SDL_ReleaseWindowFromGPUDevice(g->dev, g->win);
        SDL_DestroyGPUDevice(g->dev);
    }
    SDL_free(g);
}

int gpu_set_frame_width(Gpu *g, int w)
{
    SDL_GPUTexture *f, *u;
    SDL_GPUTransferBuffer *t;
    if (w == g->frame_w)
        return 0;
    f = make_texture(g->dev, w, NES_H, SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM, SDL_GPU_TEXTUREUSAGE_SAMPLER);
    u = make_texture(g->dev, w * UP, NES_H * UP, SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM,
                     SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COLOR_TARGET);
    t = make_transfer(g->dev, (Uint32)(w * NES_H * 4), SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD);
    if (!f || !u || !t) {
        if (f)
            SDL_ReleaseGPUTexture(g->dev, f);
        if (u)
            SDL_ReleaseGPUTexture(g->dev, u);
        if (t)
            SDL_ReleaseGPUTransferBuffer(g->dev, t);
        return -1;
    }
    SDL_WaitForGPUIdle(g->dev);
    SDL_ReleaseGPUTexture(g->dev, g->frame);
    SDL_ReleaseGPUTexture(g->dev, g->up);
    SDL_ReleaseGPUTransferBuffer(g->dev, g->upload);
    g->frame = f;
    g->up = u;
    g->upload = t;
    g->frame_w = w;
    return 0;
}

const char *gpu_driver(const Gpu *g)
{
    return g && g->dev ? SDL_GetGPUDeviceDriver(g->dev) : "none";
}

/* copy pass: picture (and overlay) to the GPU */
static void upload(Gpu *g, SDL_GPUCommandBuffer *cmd, const uint32_t *pixels, SDL_Surface *overlay)
{
    SDL_GPUCopyPass *cp;
    SDL_GPUTextureTransferInfo src;
    SDL_GPUTextureRegion dst;
    void *p = SDL_MapGPUTransferBuffer(g->dev, g->upload, true);
    if (!p)
        return;
    memcpy(p, pixels, (size_t)g->frame_w * NES_H * 4);
    SDL_UnmapGPUTransferBuffer(g->dev, g->upload);
    if (overlay && (!g->overlay || g->overlay_w != overlay->w || g->overlay_h != overlay->h)) {
        if (g->overlay)
            SDL_ReleaseGPUTexture(g->dev, g->overlay);
        if (g->overlay_upload)
            SDL_ReleaseGPUTransferBuffer(g->dev, g->overlay_upload);
        g->overlay = make_texture(g->dev, overlay->w, overlay->h, SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM,
                                  SDL_GPU_TEXTUREUSAGE_SAMPLER);
        g->overlay_upload = make_transfer(g->dev, (Uint32)(overlay->w * overlay->h * 4), SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD);
        g->overlay_w = overlay->w;
        g->overlay_h = overlay->h;
    }
    if (overlay && g->overlay && g->overlay_upload) {
        uint8_t *q = SDL_MapGPUTransferBuffer(g->dev, g->overlay_upload, true);
        int y;
        if (q) {
            for (y = 0; y < overlay->h; y++)
                memcpy(q + y * overlay->w * 4, (uint8_t *)overlay->pixels + y * overlay->pitch, (size_t)overlay->w * 4);
            SDL_UnmapGPUTransferBuffer(g->dev, g->overlay_upload);
        }
    }
    cp = SDL_BeginGPUCopyPass(cmd);
    SDL_zero(src);
    SDL_zero(dst);
    src.transfer_buffer = g->upload;
    src.pixels_per_row = (Uint32)g->frame_w;
    src.rows_per_layer = NES_H;
    dst.texture = g->frame;
    dst.w = (Uint32)g->frame_w;
    dst.h = NES_H;
    dst.d = 1;
    SDL_UploadToGPUTexture(cp, &src, &dst, true);
    if (overlay && g->overlay && g->overlay_upload) {
        src.transfer_buffer = g->overlay_upload;
        src.pixels_per_row = (Uint32)overlay->w;
        src.rows_per_layer = (Uint32)overlay->h;
        dst.texture = g->overlay;
        dst.w = (Uint32)overlay->w;
        dst.h = (Uint32)overlay->h;
        SDL_UploadToGPUTexture(cp, &src, &dst, true);
    }
    SDL_EndGPUCopyPass(cp);
}

/* One full-viewport draw of `shader` reading `tex` (size tw x th) into the current pass. */
static void draw(Gpu *g, SDL_GPUCommandBuffer *cmd, SDL_GPURenderPass *rp, SDL_GPUTextureFormat fmt, int shader,
                 int blend, SDL_GPUTexture *tex, int tw, int th, SDL_GPUSampler *smp, SDL_FRect vp, const float *prm,
                 const float *prm2)
{
    SDL_GPUGraphicsPipeline *pipe = pipeline(g, shader, fmt, blend);
    SDL_GPUViewport v;
    SDL_GPUTextureSamplerBinding b;
    Params u;
    if (!pipe)
        return;
    SDL_zero(u);
    set_size(u.src_size, (float)tw, (float)th);
    set_size(u.out_size, vp.w, vp.h);
    memcpy(u.params, prm, sizeof u.params);
    if (prm2)
        memcpy(u.params2, prm2, sizeof u.params2);
    v.x = vp.x;
    v.y = vp.y;
    v.w = vp.w;
    v.h = vp.h;
    v.min_depth = 0;
    v.max_depth = 1;
    b.texture = tex;
    b.sampler = smp;
    SDL_BindGPUGraphicsPipeline(rp, pipe);
    SDL_SetGPUViewport(rp, &v);
    SDL_BindGPUFragmentSamplers(rp, 0, &b, 1);
    SDL_PushGPUFragmentUniformData(cmd, 0, &u, sizeof u);
    SDL_DrawGPUPrimitives(rp, 3, 1, 0, 0);
}

/* The chain into target (format fmt), picture in rect dst, optional overlay. */
static void chain(Gpu *g, SDL_GPUCommandBuffer *cmd, SDL_GPUTexture *target, SDL_GPUTextureFormat fmt, GpuPreset preset,
                  SDL_FRect dst, SDL_Surface *overlay)
{
    static const float one[4] = {1, 0, 0, 0};
    static const float crt[4] = {0.85f, 0.35f, 1.35f, 0}; /* scanlines, mask, brightness */
    static const float crt_soft[4] = {0.35f, 0, 0, 0};    /* horizontal softness */
    SDL_GPUColorTargetInfo ct;
    SDL_GPURenderPass *rp;
    SDL_GPUTexture *src = g->frame;
    int sw = g->frame_w, sh = NES_H;

    if (preset == GPU_PRESET_SMOOTH || preset == GPU_PRESET_SMOOTH_CRT) { /* pass 1: upscale */
        SDL_FRect all = {0, 0, (float)(g->frame_w * UP), NES_H * UP};
        SDL_zero(ct);
        ct.texture = g->up;
        ct.load_op = SDL_GPU_LOADOP_DONT_CARE;
        ct.store_op = SDL_GPU_STOREOP_STORE;
        rp = SDL_BeginGPURenderPass(cmd, &ct, 1, NULL);
        draw(g, cmd, rp, SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM, SH_UPSCALE, 0, g->frame, g->frame_w, NES_H, g->nearest, all,
             one, NULL);
        SDL_EndGPURenderPass(rp);
        src = g->up;
        sw = g->frame_w * UP;
        sh = NES_H * UP;
    }

    SDL_zero(ct);
    ct.texture = target;
    ct.load_op = SDL_GPU_LOADOP_CLEAR;
    ct.store_op = SDL_GPU_STOREOP_STORE;
    ct.clear_color.a = 1.0f;
    rp = SDL_BeginGPURenderPass(cmd, &ct, 1, NULL);
    switch (preset) {
    case GPU_PRESET_NEAREST:
        draw(g, cmd, rp, fmt, SH_COPY, 0, src, sw, sh, g->nearest, dst, one, NULL);
        break;
    case GPU_PRESET_BILINEAR:
        draw(g, cmd, rp, fmt, SH_COPY, 0, src, sw, sh, g->linear, dst, one, NULL);
        break;
    case GPU_PRESET_CRT:
        draw(g, cmd, rp, fmt, SH_CRT, 0, src, sw, sh, g->linear, dst, crt, crt_soft);
        break;
    case GPU_PRESET_SMOOTH_CRT: {
        /* the CRT works per NES line: it reads the upscaled picture but keeps its beams at 240 lines */
        static const float soft240[4] = {0.35f, NES_H, 0, 0};
        draw(g, cmd, rp, fmt, SH_CRT, 0, src, sw, sh, g->linear, dst, crt, soft240);
        break;
    }
    default: /* sharp, smooth */
        draw(g, cmd, rp, fmt, SH_SHARP, 0, src, sw, sh, g->linear, dst, one, NULL);
        break;
    }
    if (overlay && g->overlay) {
        SDL_FRect ov = dst; /* the text covers the middle 256-pixel part of a wide picture */
        ov.w = dst.w * NES_W / (float)g->frame_w;
        ov.x = dst.x + (dst.w - ov.w) / 2;
        draw(g, cmd, rp, fmt, SH_COPY, 1, g->overlay, g->overlay_w, g->overlay_h, g->nearest, ov, one, NULL);
    }
    SDL_EndGPURenderPass(rp);
}

int gpu_present(Gpu *g, const uint32_t *pixels, GpuPreset preset, SDL_FRect dst, SDL_Surface *overlay)
{
    SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(g->dev);
    SDL_GPUTexture *sc = NULL;
    if (!cmd)
        return -1;
    upload(g, cmd, pixels, overlay);
    if (!SDL_WaitAndAcquireGPUSwapchainTexture(cmd, g->win, &sc, NULL, NULL) || !sc) {
        SDL_SubmitGPUCommandBuffer(cmd); /* minimized: nothing to draw into */
        return 0;
    }
    chain(g, cmd, sc, SDL_GetGPUSwapchainTextureFormat(g->dev, g->win), preset, dst, overlay);
    return SDL_SubmitGPUCommandBuffer(cmd) ? 0 : -1;
}

int gpu_render_offscreen(Gpu *g, const uint32_t *pixels, GpuPreset preset, int w, int h, uint32_t *out)
{
    SDL_GPUTexture *t = make_texture(g->dev, w, h, SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM,
                                     SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER);
    SDL_GPUTransferBuffer *down = make_transfer(g->dev, (Uint32)(w * h * 4), SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD);
    SDL_GPUCommandBuffer *cmd;
    SDL_GPUCopyPass *cp;
    SDL_GPUTextureRegion src;
    SDL_GPUTextureTransferInfo dst;
    SDL_GPUFence *fence;
    SDL_FRect all = {0, 0, (float)w, (float)h};
    int ok = 0;
    if (!t || !down || !(cmd = SDL_AcquireGPUCommandBuffer(g->dev)))
        goto done;
    upload(g, cmd, pixels, NULL);
    chain(g, cmd, t, SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM, preset, all, NULL);
    cp = SDL_BeginGPUCopyPass(cmd);
    SDL_zero(src);
    SDL_zero(dst);
    src.texture = t;
    src.w = (Uint32)w;
    src.h = (Uint32)h;
    src.d = 1;
    dst.transfer_buffer = down;
    dst.pixels_per_row = (Uint32)w;
    dst.rows_per_layer = (Uint32)h;
    SDL_DownloadFromGPUTexture(cp, &src, &dst);
    SDL_EndGPUCopyPass(cp);
    fence = SDL_SubmitGPUCommandBufferAndAcquireFence(cmd);
    if (fence) {
        SDL_WaitForGPUFences(g->dev, true, &fence, 1);
        SDL_ReleaseGPUFence(g->dev, fence);
        {
            const uint32_t *p = SDL_MapGPUTransferBuffer(g->dev, down, false);
            if (p) {
                int i;
                for (i = 0; i < w * h; i++)
                    out[i] = 0xFF000000u | (p[i] & 0xFFFFFFu); /* BGRA in memory = 0xAARRGGBB */
                SDL_UnmapGPUTransferBuffer(g->dev, down);
                ok = 1;
            }
        }
    }
done:
    if (t)
        SDL_ReleaseGPUTexture(g->dev, t);
    if (down)
        SDL_ReleaseGPUTransferBuffer(g->dev, down);
    return ok ? 0 : -1;
}
