#include "kit.h"
#include "stb_truetype.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PI_F 3.14159265f

static const char *const font_files[FONT_COUNT] = {
    "Bangers-Regular.ttf", "BarlowCondensed-Bold.ttf", "BarlowCondensed-ExtraBoldItalic.ttf",
    "Barlow-SemiBold.ttf", "Barlow-Bold.ttf",          "JetBrainsMono-Regular.ttf",
};

/* glyph sets: ASCII, Latin-1, and the few symbols the screens use */
static int extra_cp[] = {0x2026, 0x2039, 0x203A, 0x2014, 0x2013, 0x2190, 0x2191, 0x2192, 0x2193, 0x2022, 0x00B7,
                         0x00D7, 0x2019, 0x201C, 0x201D};
#define N_EXTRA ((int)(sizeof extra_cp / sizeof extra_cp[0]))

typedef struct Atlas {
    int font, px;
    SDL_Texture *tex;
    int tw, th;
    stbtt_packedchar ascii[95], latin[96], extra[N_EXTRA];
    int have_extra;
    float ascent;
} Atlas;

#define MAX_ATLAS 48
static struct {
    SDL_Renderer *r;
    unsigned char *ttf[FONT_COUNT];
    stbtt_fontinfo info[FONT_COUNT];
    Atlas atlas[MAX_ATLAS];
    int n_atlas;
    float m[8][6];  /* transform stack: a b c d tx ty */
    float alpha[8];
    int sp;
    char err[256];
    float ps;
} K;

SDL_Renderer *kit_renderer(void) { return K.r; }
void kit_set_pixel_scale(float s) /* quarter steps, so resizing does not rebuild atlases every frame */
{
    s = floorf(s * 4.0f + 0.5f) / 4.0f;
    K.ps = s < 0.5f ? 0.5f : s > 3.0f ? 3.0f : s;
}
const char *kit_error(void) { return K.err; }

static unsigned char *read_all(const char *path)
{
    size_t n = 0;
    void *p = SDL_LoadFile(path, &n);
    return (unsigned char *)p;
}

int kit_init(SDL_Renderer *r, const char *font_dir)
{
    int i, ok = 0;
    memset(&K, 0, sizeof K);
    K.r = r;
    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
    K.m[0][0] = K.m[0][3] = 1.0f;
    K.alpha[0] = 1.0f;
    K.ps = 1.0f;
    for (i = 0; i < FONT_COUNT; i++) {
        char path[1200];
        snprintf(path, sizeof path, "%s/%s", font_dir, font_files[i]);
        K.ttf[i] = read_all(path);
        if (K.ttf[i] && stbtt_InitFont(&K.info[i], K.ttf[i], stbtt_GetFontOffsetForIndex(K.ttf[i], 0)))
            ok++;
        else {
            SDL_free(K.ttf[i]);
            K.ttf[i] = NULL;
            snprintf(K.err, sizeof K.err, "font missing: %s", path);
        }
    }
    return ok == FONT_COUNT ? 0 : -1;
}

void kit_quit(void)
{
    int i;
    for (i = 0; i < K.n_atlas; i++)
        SDL_DestroyTexture(K.atlas[i].tex);
    for (i = 0; i < FONT_COUNT; i++)
        SDL_free(K.ttf[i]);
    memset(&K, 0, sizeof K);
}

/* ---------- transform ---------- */
void kit_push(void)
{
    if (K.sp < 7) {
        memcpy(K.m[K.sp + 1], K.m[K.sp], sizeof K.m[0]);
        K.alpha[K.sp + 1] = K.alpha[K.sp];
        K.sp++;
    }
}
void kit_pop(void)
{
    if (K.sp > 0)
        K.sp--;
}
static void mul(const float n[6]) /* current = current * n */
{
    float *m = K.m[K.sp], o[6];
    o[0] = m[0] * n[0] + m[2] * n[1];
    o[1] = m[1] * n[0] + m[3] * n[1];
    o[2] = m[0] * n[2] + m[2] * n[3];
    o[3] = m[1] * n[2] + m[3] * n[3];
    o[4] = m[0] * n[4] + m[2] * n[5] + m[4];
    o[5] = m[1] * n[4] + m[3] * n[5] + m[5];
    memcpy(m, o, sizeof o);
}
void kit_translate(float x, float y)
{
    float n[6] = {1, 0, 0, 1, 0, 0};
    n[4] = x;
    n[5] = y;
    mul(n);
}
void kit_rotate_at(float deg, float cx, float cy)
{
    float c = cosf(deg * PI_F / 180.0f), s = sinf(deg * PI_F / 180.0f);
    float n[6];
    n[0] = c;
    n[1] = s;
    n[2] = -s;
    n[3] = c;
    kit_translate(cx, cy);
    n[4] = n[5] = 0;
    mul(n);
    kit_translate(-cx, -cy);
}
void kit_alpha(float a) { K.alpha[K.sp] *= a; }

static SDL_FColor col(Rgb c, float a)
{
    SDL_FColor f;
    f.r = (float)((c >> 16) & 255) / 255.0f;
    f.g = (float)((c >> 8) & 255) / 255.0f;
    f.b = (float)(c & 255) / 255.0f;
    f.a = a * K.alpha[K.sp];
    return f;
}
static SDL_Vertex vtx(float x, float y, SDL_FColor c, float u, float v)
{
    const float *m = K.m[K.sp];
    SDL_Vertex o;
    o.position.x = m[0] * x + m[2] * y + m[4];
    o.position.y = m[1] * x + m[3] * y + m[5];
    o.color = c;
    o.tex_coord.x = u;
    o.tex_coord.y = v;
    return o;
}

/* ---------- shapes ---------- */
void kit_quad(const float p[8], Rgb c, float a)
{
    SDL_FColor f = col(c, a);
    SDL_Vertex v[4];
    static const int idx[6] = {0, 1, 2, 0, 2, 3};
    int i;
    for (i = 0; i < 4; i++)
        v[i] = vtx(p[2 * i], p[2 * i + 1], f, 0, 0);
    SDL_RenderGeometry(K.r, NULL, v, 4, idx, 6);
}
void kit_rect(float x, float y, float w, float h, Rgb c, float a)
{
    float p[8];
    p[0] = x, p[1] = y, p[2] = x + w, p[3] = y, p[4] = x + w, p[5] = y + h, p[6] = x, p[7] = y + h;
    kit_quad(p, c, a);
}
void kit_frame(float x, float y, float w, float h, float t, Rgb c, float a)
{
    kit_rect(x, y, w, t, c, a);
    kit_rect(x, y + h - t, w, t, c, a);
    kit_rect(x, y + t, t, h - 2 * t, c, a);
    kit_rect(x + w - t, y + t, t, h - 2 * t, c, a);
}
void kit_gradient_v(float x, float y, float w, float h, Rgb top, Rgb bottom, float a)
{
    SDL_Vertex v[4];
    static const int idx[6] = {0, 1, 2, 0, 2, 3};
    SDL_FColor t = col(top, a), b = col(bottom, a);
    v[0] = vtx(x, y, t, 0, 0);
    v[1] = vtx(x + w, y, t, 0, 0);
    v[2] = vtx(x + w, y + h, b, 0, 0);
    v[3] = vtx(x, y + h, b, 0, 0);
    SDL_RenderGeometry(K.r, NULL, v, 4, idx, 6);
}
#define MAXV 512
void kit_poly(const float *xy, int n, Rgb c, float a)
{
    SDL_Vertex v[MAXV + 2];
    int idx[MAXV * 3], i;
    float cx = 0, cy = 0;
    SDL_FColor f = col(c, a);
    if (n < 3 || n > MAXV)
        return;
    for (i = 0; i < n; i++)
        cx += xy[2 * i], cy += xy[2 * i + 1];
    v[0] = vtx(cx / (float)n, cy / (float)n, f, 0, 0);
    for (i = 0; i < n; i++) {
        v[i + 1] = vtx(xy[2 * i], xy[2 * i + 1], f, 0, 0);
        idx[3 * i] = 0;
        idx[3 * i + 1] = i + 1;
        idx[3 * i + 2] = (i + 1) % n + 1;
    }
    SDL_RenderGeometry(K.r, NULL, v, n + 1, idx, 3 * n);
}
static int segs(float r)
{
    int n = (int)(r * 0.9f) + 12;
    return n > 120 ? 120 : n;
}
void kit_circle(float cx, float cy, float r, Rgb c, float a)
{
    float p[2 * 120];
    int n = segs(r), i;
    for (i = 0; i < n; i++) {
        float t = 2 * PI_F * (float)i / (float)n;
        p[2 * i] = cx + r * cosf(t);
        p[2 * i + 1] = cy + r * sinf(t);
    }
    kit_poly(p, n, c, a);
}
void kit_ellipse_ring(float cx, float cy, float rx, float ry, float t, float deg, Rgb c, float a)
{
    SDL_Vertex v[242];
    int idx[120 * 6], n = segs(rx > ry ? rx : ry), i;
    float cr = cosf(deg * PI_F / 180), sr = sinf(deg * PI_F / 180);
    SDL_FColor f = col(c, a);
    for (i = 0; i < n; i++) {
        float th = 2 * PI_F * (float)i / (float)n;
        int k;
        for (k = 0; k < 2; k++) {
            float ex = (rx + (k ? t / 2 : -t / 2)) * cosf(th), ey = (ry + (k ? t / 2 : -t / 2)) * sinf(th);
            v[2 * i + k] = vtx(cx + ex * cr - ey * sr, cy + ex * sr + ey * cr, f, 0, 0);
        }
        idx[6 * i] = 2 * i;
        idx[6 * i + 1] = 2 * i + 1;
        idx[6 * i + 2] = (2 * i + 2) % (2 * n);
        idx[6 * i + 3] = 2 * i + 1;
        idx[6 * i + 4] = (2 * i + 3) % (2 * n);
        idx[6 * i + 5] = (2 * i + 2) % (2 * n);
    }
    SDL_RenderGeometry(K.r, NULL, v, 2 * n, idx, 6 * n);
}
void kit_ring(float cx, float cy, float r_in, float r_out, Rgb c, float a)
{
    kit_ellipse_ring(cx, cy, (r_in + r_out) / 2, (r_in + r_out) / 2, r_out - r_in, 0, c, a);
}
void kit_rrect(float x, float y, float w, float h, float rad, Rgb c, float a)
{
    float p[2 * 4 * 10];
    int i, k, n = 0;
    static const float cxs[4] = {1, 0, 0, 1}, cys[4] = {1, 1, 0, 0};
    if (rad * 2 > w)
        rad = w / 2;
    if (rad * 2 > h)
        rad = h / 2;
    for (k = 0; k < 4; k++) {
        float ox = cxs[k] ? x + w - rad : x + rad, oy = cys[k] ? y + h - rad : y + rad;
        for (i = 0; i < 10; i++) {
            float th = (float)k * PI_F / 2 + (PI_F / 2) * (float)i / 9.0f;
            p[2 * n] = ox + rad * cosf(th);
            p[2 * n + 1] = oy + rad * sinf(th);
            n++;
        }
    }
    kit_poly(p, n, c, a);
}
void kit_line(float x0, float y0, float x1, float y1, float t, Rgb c, float a)
{
    float dx = x1 - x0, dy = y1 - y0, l = sqrtf(dx * dx + dy * dy), nx, ny, p[8];
    if (l <= 0)
        return;
    nx = -dy / l * t / 2;
    ny = dx / l * t / 2;
    p[0] = x0 + nx, p[1] = y0 + ny, p[2] = x1 + nx, p[3] = y1 + ny;
    p[4] = x1 - nx, p[5] = y1 - ny, p[6] = x0 - nx, p[7] = y0 - ny;
    kit_quad(p, c, a);
    kit_circle(x0, y0, t / 2, c, a);
    kit_circle(x1, y1, t / 2, c, a);
}
void kit_texture(SDL_Texture *t, float x, float y, float w, float h, float a)
{
    SDL_Vertex v[4];
    static const int idx[6] = {0, 1, 2, 0, 2, 3};
    SDL_FColor f = col(0xffffff, a);
    v[0] = vtx(x, y, f, 0, 0);
    v[1] = vtx(x + w, y, f, 1, 0);
    v[2] = vtx(x + w, y + h, f, 1, 1);
    v[3] = vtx(x, y + h, f, 0, 1);
    SDL_RenderGeometry(K.r, t, v, 4, idx, 6);
}

/* ---------- text ---------- */
static Atlas *atlas_for(KitFont f, float size)
{
    int px = (int)(size * K.ps + 0.5f), i, tw, th, over;
    Atlas *A;
    unsigned char *bits;
    stbtt_pack_context pc;
    stbtt_pack_range rg[3];
    if (!K.ttf[f] || px < 4)
        return NULL;
    for (i = 0; i < K.n_atlas; i++)
        if (K.atlas[i].font == (int)f && K.atlas[i].px == px)
            return &K.atlas[i];
    if (K.n_atlas == MAX_ATLAS) { /* forget the oldest */
        SDL_DestroyTexture(K.atlas[0].tex);
        memmove(&K.atlas[0], &K.atlas[1], sizeof(Atlas) * (MAX_ATLAS - 1));
        K.n_atlas--;
    }
    A = &K.atlas[K.n_atlas];
    memset(A, 0, sizeof *A);
    A->font = (int)f;
    A->px = px;
    over = px < 40 ? 2 : 1;
    tw = px * over < 48 ? 512 : px * over < 100 ? 1024 : 2048;
    th = tw;
    bits = (unsigned char *)calloc((size_t)tw * (size_t)th, 1);
    if (!bits)
        return NULL;
    if (!stbtt_PackBegin(&pc, bits, tw, th, 0, 1, NULL)) {
        free(bits);
        return NULL;
    }
    stbtt_PackSetOversampling(&pc, (unsigned)over, (unsigned)over);
    memset(rg, 0, sizeof rg);
    rg[0].font_size = (float)px;
    rg[0].first_unicode_codepoint_in_range = 32;
    rg[0].num_chars = 95;
    rg[0].chardata_for_range = A->ascii;
    rg[1].font_size = (float)px;
    rg[1].first_unicode_codepoint_in_range = 160;
    rg[1].num_chars = 96;
    rg[1].chardata_for_range = A->latin;
    rg[2].font_size = (float)px;
    rg[2].array_of_unicode_codepoints = extra_cp;
    rg[2].num_chars = N_EXTRA;
    rg[2].chardata_for_range = A->extra;
    stbtt_PackFontRanges(&pc, K.ttf[f], 0, rg, 3);
    stbtt_PackEnd(&pc);
    {
        float scale = stbtt_ScaleForPixelHeight(&K.info[f], (float)px);
        int asc, desc, gap;
        stbtt_GetFontVMetrics(&K.info[f], &asc, &desc, &gap);
        A->ascent = (float)asc * scale;
    }
    {
        SDL_Surface *s = SDL_CreateSurface(tw, th, SDL_PIXELFORMAT_RGBA32);
        int y, x;
        if (!s) {
            free(bits);
            return NULL;
        }
        for (y = 0; y < th; y++) {
            uint8_t *row = (uint8_t *)s->pixels + (size_t)y * (size_t)s->pitch;
            for (x = 0; x < tw; x++) {
                row[4 * x] = row[4 * x + 1] = row[4 * x + 2] = 255;
                row[4 * x + 3] = bits[(size_t)y * (size_t)tw + (size_t)x];
            }
        }
        A->tex = SDL_CreateTextureFromSurface(K.r, s);
        SDL_DestroySurface(s);
        if (A->tex)
            SDL_SetTextureScaleMode(A->tex, SDL_SCALEMODE_LINEAR);
    }
    free(bits);
    A->tw = tw;
    A->th = th;
    K.n_atlas++;
    return A;
}

static uint32_t utf8_next(const char **ps)
{
    const unsigned char *s = (const unsigned char *)*ps;
    uint32_t c = s[0];
    int n = 0;
    if (c < 0x80)
        n = 0;
    else if ((c & 0xE0) == 0xC0)
        c &= 0x1F, n = 1;
    else if ((c & 0xF0) == 0xE0)
        c &= 0x0F, n = 2;
    else
        c &= 0x07, n = 3;
    s++;
    while (n-- > 0 && (*s & 0xC0) == 0x80)
        c = (c << 6) | (*s++ & 0x3F);
    *ps = (const char *)s;
    return c;
}

static int glyph(Atlas *A, uint32_t cp, const stbtt_packedchar **set, int *index)
{
    int i;
    if (cp >= 32 && cp < 127) {
        *set = A->ascii;
        *index = (int)cp - 32;
        return 1;
    }
    if (cp >= 160 && cp < 256) {
        *set = A->latin;
        *index = (int)cp - 160;
        return 1;
    }
    for (i = 0; i < N_EXTRA; i++)
        if ((uint32_t)extra_cp[i] == cp && A->extra[i].x1 > A->extra[i].x0) {
            *set = A->extra;
            *index = i;
            return 1;
        }
    *set = A->ascii; /* '?' */
    *index = '?' - 32;
    return 1;
}

static float scale_of(Atlas *A, float size) { return size / (float)A->px; }

float kit_text_width(KitFont f, float size, const char *s)
{
    Atlas *A = atlas_for(f, size);
    float w = 0;
    if (!A)
        return 0;
    while (*s) {
        const stbtt_packedchar *set;
        int i;
        glyph(A, utf8_next(&s), &set, &i);
        w += set[i].xadvance;
    }
    return w * scale_of(A, size);
}

float kit_text_ascent(KitFont f, float size)
{
    Atlas *A = atlas_for(f, size);
    return A ? A->ascent * scale_of(A, size) : size * 0.8f;
}

static float draw_run(Atlas *A, float size, float x, float y, const char *s, Rgb c, float a)
{
    SDL_Vertex v[4 * 128];
    int idx[6 * 128], n = 0;
    float sc = scale_of(A, size), pen = 0, base = y + A->ascent * sc;
    SDL_FColor fc = col(c, a);
    while (*s) {
        const stbtt_packedchar *set;
        int i;
        stbtt_aligned_quad q;
        float px = 0, py = 0;
        glyph(A, utf8_next(&s), &set, &i);
        stbtt_GetPackedQuad(set, A->tw, A->th, i, &px, &py, &q, 0);
        if (q.x1 > q.x0) {
            float x0 = x + (pen + q.x0) * sc, x1 = x + (pen + q.x1) * sc, y0 = base + q.y0 * sc, y1 = base + q.y1 * sc;
            v[4 * n] = vtx(x0, y0, fc, q.s0, q.t0);
            v[4 * n + 1] = vtx(x1, y0, fc, q.s1, q.t0);
            v[4 * n + 2] = vtx(x1, y1, fc, q.s1, q.t1);
            v[4 * n + 3] = vtx(x0, y1, fc, q.s0, q.t1);
            idx[6 * n] = 4 * n, idx[6 * n + 1] = 4 * n + 1, idx[6 * n + 2] = 4 * n + 2;
            idx[6 * n + 3] = 4 * n, idx[6 * n + 4] = 4 * n + 2, idx[6 * n + 5] = 4 * n + 3;
            n++;
            if (n == 128) {
                SDL_RenderGeometry(K.r, A->tex, v, 4 * n, idx, 6 * n);
                n = 0;
            }
        }
        pen += set[i].xadvance;
    }
    if (n)
        SDL_RenderGeometry(K.r, A->tex, v, 4 * n, idx, 6 * n);
    return pen * sc;
}

float kit_text(KitFont f, float size, float x, float y, const char *s, Rgb c, float a, KitAlign al)
{
    Atlas *A = atlas_for(f, size);
    float w;
    if (!A || !s)
        return 0;
    w = kit_text_width(f, size, s);
    if (al == ALIGN_CENTER)
        x -= w / 2;
    else if (al == ALIGN_RIGHT)
        x -= w;
    draw_run(A, size, x, y, s, c, a);
    return w;
}

float kit_text_ink(KitFont f, float size, float x, float y, const char *s, Rgb fill, Rgb ink, float stroke,
                   float sdx, float sdy, KitAlign al)
{
    int k;
    float w = kit_text_width(f, size, s);
    if (al == ALIGN_CENTER)
        x -= w / 2;
    else if (al == ALIGN_RIGHT)
        x -= w;
    if (sdx != 0 || sdy != 0)
        for (k = 0; k < 16; k++) {
            float t = 2 * PI_F * (float)k / 16;
            kit_text(f, size, x + sdx + stroke * cosf(t), y + sdy + stroke * sinf(t), s, ink, 1, ALIGN_LEFT);
        }
    if (stroke > 0)
        for (k = 0; k < 16; k++) {
            float t = 2 * PI_F * (float)k / 16;
            kit_text(f, size, x + stroke * cosf(t), y + stroke * sinf(t), s, ink, 1, ALIGN_LEFT);
        }
    kit_text(f, size, x, y, s, fill, 1, ALIGN_LEFT);
    return w;
}

float kit_text_wrap(KitFont f, float size, float x, float y, float w, float line_h, const char *s, Rgb c, float a)
{
    char line[512];
    float yy = y;
    while (*s) {
        int n = 0, last_space = -1;
        while (s[n] && s[n] != '\n' && n < (int)sizeof line - 1) {
            line[n] = s[n];
            line[n + 1] = 0;
            if (s[n] == ' ')
                last_space = n;
            if (kit_text_width(f, size, line) > w && last_space > 0) {
                n = last_space;
                break;
            }
            n++;
        }
        line[n] = 0;
        kit_text(f, size, x, yy, line, c, a, ALIGN_LEFT);
        yy += line_h;
        s += n;
        while (*s == ' ' || *s == '\n')
            s++;
    }
    return yy - y;
}
