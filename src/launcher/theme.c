#include "theme.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

static SDL_Texture *field[2]; /* home, settings */

static uint32_t lerp_rgb(Rgb a, Rgb b, float t)
{
    int i;
    uint32_t o = 0;
    if (t < 0)
        t = 0;
    if (t > 1)
        t = 1;
    for (i = 0; i < 3; i++) {
        float ca = (float)((a >> (8 * i)) & 255), cb = (float)((b >> (8 * i)) & 255);
        o |= (uint32_t)(ca + (cb - ca) * t + 0.5f) << (8 * i);
    }
    return o;
}

/* radial green field with a masked halftone of dark dots, rendered once in software */
static SDL_Texture *make_field(float mx, float my, float mr)
{
    SDL_Surface *s = SDL_CreateSurface(KIT_W, KIT_H, SDL_PIXELFORMAT_XRGB8888);
    SDL_Texture *t;
    int x, y;
    if (!s)
        return NULL;
    for (y = 0; y < KIT_H; y++) {
        uint32_t *row = (uint32_t *)((uint8_t *)s->pixels + (size_t)y * (size_t)s->pitch);
        for (x = 0; x < KIT_W; x++) {
            float dx = ((float)x - 0.30f * KIT_W) / KIT_W, dy = ((float)y - 0.35f * KIT_H) / KIT_H;
            float d = sqrtf(dx * dx + dy * dy * 0.6f) / 0.8f;
            Rgb c = d < 0.45f ? lerp_rgb(C_GREEN_HI, C_GREEN, d / 0.45f) : lerp_rgb(C_GREEN, C_GREEN_DEEP, (d - 0.45f) / 0.55f);
            /* halftone: 14 px grid, dot radius 2.4 px, faded by distance from the mask centre */
            float gx = (float)(x % 14) - 7.0f + 0.5f, gy = (float)(y % 14) - 7.0f + 0.5f;
            float md = sqrtf(((float)x - mx) * ((float)x - mx) + ((float)y - my) * ((float)y - my)) / mr;
            float dot_a = md < 1 ? (1 - md) * 0.85f : 0;
            float r = sqrtf(gx * gx + gy * gy);
            if (dot_a > 0 && r < 2.9f) {
                float cov = r < 2.4f ? 1.0f : (2.9f - r) / 0.5f;
                c = lerp_rgb(c, C_HALFTONE, cov * dot_a);
            }
            row[x] = c;
        }
    }
    t = SDL_CreateTextureFromSurface(kit_renderer(), s);
    SDL_DestroySurface(s);
    return t;
}

int theme_init(void)
{
    field[0] = make_field(0.78f * KIT_W, 0.42f * KIT_H, 0.62f * KIT_W);
    field[1] = make_field((float)KIT_W, (float)KIT_H, 0.55f * KIT_W);
    return field[0] && field[1] ? 0 : -1;
}

void theme_quit(void)
{
    int i;
    for (i = 0; i < 2; i++)
        if (field[i])
            SDL_DestroyTexture(field[i]), field[i] = NULL;
}

void theme_backdrop(double t, int settings)
{
    float cx = settings ? (float)KIT_W : 980.0f, cy = settings ? (float)KIT_H : 300.0f, R = 1800.0f;
    float rot = (float)fmod(t * 360.0 / 80.0, 360.0) * 3.14159265f / 180.0f;
    int i;
    if (field[settings])
        kit_texture(field[settings], 0, 0, KIT_W, KIT_H, 1);
    else
        kit_rect(0, 0, KIT_W, KIT_H, C_GREEN, 1);
    for (i = 0; i < 26; i++) { /* 6 deg white wedges every 14 deg */
        float a0 = rot + (float)i * 14.0f * 3.14159265f / 180.0f, a1 = a0 + 6.0f * 3.14159265f / 180.0f;
        float p[6];
        p[0] = cx, p[1] = cy;
        p[2] = cx + R * cosf(a0), p[3] = cy + R * sinf(a0);
        p[4] = cx + R * cosf(a1), p[5] = cy + R * sinf(a1);
        {
            float q[8];
            q[0] = p[0], q[1] = p[1], q[2] = p[2], q[3] = p[3], q[4] = p[4], q[5] = p[5], q[6] = p[0], q[7] = p[1];
            kit_quad(q, C_WHITE, 0.07f);
        }
    }
}

void theme_titlebar(const char *crumb, const char *version)
{
    float x;
    kit_rect(0, 0, KIT_W, 46, C_INK, 1);
    x = 26 + kit_text(FONT_DISPLAY, 22, 26, 10, "POND TO PAVEMENT", C_YELLOW, 1, ALIGN_LEFT);
    x += 12 + kit_text(FONT_LABEL, 17, x + 12, 13, version, 0x8a8a92, 1, ALIGN_LEFT);
    if (crumb && crumb[0]) {
        x += 26;
        x += kit_text(FONT_LABEL, 18, x, 12, "/ ", C_RED, 1, ALIGN_LEFT);
        kit_text(FONT_LABEL, 18, x, 12, crumb, C_PAPER, 1, ALIGN_LEFT);
    }
}

void theme_box(float x, float y, float w, float h, Rgb fill, float border, float shadow)
{
    if (shadow > 0)
        kit_rect(x + shadow, y + shadow, w, h, C_INK, 1);
    kit_rect(x, y, w, h, C_INK, 1);
    kit_rect(x + border, y + border, w - 2 * border, h - 2 * border, fill, 1);
}

void theme_sticker(float x, float y, const char *text, float deg)
{
    float w = kit_text_width(FONT_LABEL, 18, text) + 28;
    kit_push();
    kit_rotate_at(deg, x, y + 17);
    theme_box(x, y, w, 34, C_YELLOW, 3, 4);
    kit_text(FONT_LABEL, 18, x + 14, y + 6, text, C_INK, 1, ALIGN_LEFT);
    kit_pop();
}

float theme_badge_width(const char *text) { return kit_text_width(FONT_LABEL, 13, text) + 14; }

void theme_badge(float x, float y, const char *text, Rgb fill, Rgb fg)
{
    float w = theme_badge_width(text);
    theme_box(x, y, w, 20, fill, 2, 0);
    kit_text(FONT_LABEL, 13, x + 7, y + 2, text, fg, 1, ALIGN_LEFT);
}

void theme_stamp(float rx, float cy, const char *text, Rgb c)
{
    float w = kit_text_width(FONT_DISPLAY, 18, text) + 18, x = rx - w;
    kit_push();
    kit_rotate_at(-6, x + w / 2, cy);
    kit_frame(x, cy - 15, w, 30, 3, c, 1);
    kit_text(FONT_DISPLAY, 18, x + 9, cy - 11, text, c, 1, ALIGN_LEFT);
    kit_pop();
}

/* rocket fist on a deep-space disc with an orange ring (drawn in a 330 px box, scaled) */
void theme_emblem(float cx, float cy, float size)
{
    float s = size / 330.0f;
    kit_push();
    kit_translate(cx - 165 * s, cy - 165 * s);
    /* the transform has no scale, so scale coordinates by hand */
#define S(v) ((v) * s)
    kit_circle(S(165), S(165), S(154), C_INK, 1);
    kit_circle(S(165), S(165), S(146), C_SPACE, 1);
    kit_circle(S(96), S(88), S(9), C_PAPER, 1);
    kit_circle(S(262), S(110), S(6), C_PAPER, 1);
    kit_circle(S(70), S(150), S(3), C_PAPER, 0.8f);
    kit_line(S(60), S(230), S(30), S(260), S(8), C_PAPER, 1);
    kit_line(S(85), S(255), S(60), S(290), S(8), C_PAPER, 1);
    kit_line(S(250), S(235), S(285), S(265), S(8), C_PAPER, 1);
    kit_ellipse_ring(S(165), S(175), S(162), S(40), S(16), -18, C_INK, 1);
    kit_ellipse_ring(S(165), S(175), S(162), S(40), S(8), -18, C_RED, 1);
    kit_push();
    kit_rotate_at(-14, S(165), S(180));
    {
        static const float flame_o[] = {112, 262, 218, 262, 240, 296, 204, 288, 192, 324, 166, 292, 140, 324, 128, 288, 92, 296};
        static const float flame_i[] = {130, 262, 200, 262, 210, 282, 186, 278, 166, 300, 146, 278, 120, 282};
        float p[18], q[14];
        int i;
        for (i = 0; i < 18; i++)
            p[i] = S(flame_o[i]);
        for (i = 0; i < 14; i++)
            q[i] = S(flame_i[i]);
        /* ink outline by drawing the flame slightly larger first */
        kit_push();
        kit_poly(p, 9, C_INK, 1);
        kit_pop();
        {
            float pi2[18];
            float mx = S(166), my = S(285);
            for (i = 0; i < 9; i++) {
                pi2[2 * i] = mx + (p[2 * i] - mx) * 0.9f;
                pi2[2 * i + 1] = my + (p[2 * i + 1] - my) * 0.9f;
            }
            kit_poly(pi2, 9, C_RED, 1);
        }
        kit_poly(q, 7, C_YELLOW, 1);
        /* cuff */
        theme_box(S(108), S(234), S(114), S(38), C_RED, S(8) > 2 ? S(8) : 2, 0);
        /* fist: palm, knuckles, thumb (ink first, then paper inset) */
        {
            static const float parts[][5] = {
                {98, 128, 136, 118, 24}, {98, 92, 38, 66, 17}, {131, 84, 38, 70, 17},
                {164, 88, 38, 68, 17},   {197, 100, 36, 60, 16},
            };
            int k;
            for (k = 0; k < 5; k++)
                kit_rrect(S(parts[k][0] - 4), S(parts[k][1] - 4), S(parts[k][2] + 8), S(parts[k][3] + 8), S(parts[k][4] + 4), C_INK, 1);
            for (k = 0; k < 5; k++)
                kit_rrect(S(parts[k][0] + 4), S(parts[k][1] + 4), S(parts[k][2] - 8), S(parts[k][3] - 8), S(parts[k][4] - 4), C_PAPER, 1);
            /* finger gaps */
            for (k = 1; k < 4; k++)
                kit_line(S(98 + 33 * k), S(110), S(98 + 33 * k), S(150), S(6), C_INK, 1);
            kit_rrect(S(84), S(164), S(112), S(48), S(24), C_INK, 1);
            kit_rrect(S(92), S(172), S(96), S(32), S(16), C_PAPER, 1);
        }
    }
    kit_pop();
#undef S
    kit_pop();
}

void theme_logo(float cx, float y, float s1, float s2, int tagline)
{
    float h1 = s1 * 0.86f;
    kit_push();
    kit_rotate_at(-4, cx, y + (s1 + s2) / 2);
    {
        static const char *const words[2] = {"POND TO", "PAVEMENT"};
        float sz[2], wy[2];
        int k;
        sz[0] = s1, sz[1] = s2;
        wy[0] = y, wy[1] = y + h1 - s2 * 0.08f;
        for (k = 0; k < 2; k++) { /* ink offset, red offset, then the yellow word with its ink outline */
            kit_text_ink(FONT_DISPLAY, sz[k], cx + 10, wy[k] + 10, words[k], C_INK, C_INK, 4, 0, 0, ALIGN_CENTER);
            kit_text_ink(FONT_DISPLAY, sz[k], cx + 6, wy[k] + 6, words[k], C_RED, C_RED, 4, 0, 0, ALIGN_CENTER);
            kit_text_ink(FONT_DISPLAY, sz[k], cx, wy[k], words[k], C_YELLOW, C_INK, 4, 0, 0, ALIGN_CENTER);
        }
    }
    if (tagline) {
        const char *tg = "YOUR ROM. OUR ENGINE. TWO FISTS.";
        float w = kit_text_width(FONT_LABEL, 17, tg) + 28, ty = y + h1 + s2 * 0.84f + 6;
        kit_rect(cx - w / 2, ty, w, 28, C_INK, 1);
        kit_text(FONT_LABEL, 17, cx, ty + 4, tg, C_PAPER, 1, ALIGN_CENTER);
    }
    kit_pop();
}

void theme_burst(float x, float y, float w, float h, const char *text, float text_size, int enabled, double t)
{
    static const float pts[] = {0, 32, 9, 22, 6, 2, 24, 16, 36, 0, 46, 15, 62, 2, 68, 18, 90, 6, 86, 28, 100, 38,
                                90, 54, 99, 76, 80, 72, 74, 100, 58, 80, 44, 98, 34, 78, 14, 94, 16, 70, 1, 62, 10, 48};
    float p[44], q[44], cx = x + w / 2, cy = y + h / 2;
    float k = enabled ? (float)(0.5 - 0.5 * cos(t * 2 * 3.14159265 / 1.6)) : 0;
    int i;
    kit_push();
    kit_rotate_at(-1.5f * k, cx, cy);
    for (i = 0; i < 22; i++) {
        float px = x + pts[2 * i] / 100 * w, py = y + pts[2 * i + 1] / 100 * h;
        px = cx + (px - cx) * (1 + 0.04f * k);
        py = cy + (py - cy) * (1 + 0.04f * k);
        p[2 * i] = px;
        p[2 * i + 1] = py;
        q[2 * i] = cx + (px - cx) * 0.9f;
        q[2 * i + 1] = cy + (py - cy) * 0.86f;
    }
    if (!enabled)
        kit_alpha(0.55f);
    kit_poly(p, 22, C_INK, 1);
    kit_poly(q, 22, enabled ? C_YELLOW : C_GREY, 1);
    kit_text_ink(FONT_DISPLAY, text_size, cx, cy - text_size * 0.55f, text, enabled ? C_RED : 0x8a857a, C_INK, 3, 4, 4,
                 ALIGN_CENTER);
    kit_pop();
}

void theme_icon(Icon ic, float x, float y, float sz, Rgb c)
{
    float u = sz / 24.0f, t = 2.4f * u;
#define L(a, b, cc, d) kit_line(x + (a) * u, y + (b) * u, x + (cc) * u, y + (d) * u, t, c, 1)
    switch (ic) {
    case ICON_HOME:
        L(3, 11, 12, 4); L(12, 4, 21, 11); L(5, 10, 5, 20); L(19, 10, 19, 20); L(5, 20, 19, 20); L(10, 20, 10, 14); L(14, 14, 14, 20); L(10, 14, 14, 14);
        break;
    case ICON_DISPLAY:
        L(3, 4, 21, 4); L(21, 4, 21, 16); L(21, 16, 3, 16); L(3, 16, 3, 4); L(8, 20, 16, 20); L(12, 16, 12, 20);
        break;
    case ICON_GRAPHICS: {
        int i;
        for (i = 0; i < 5; i++) {
            float a0 = -1.5708f + (float)i * 2.5133f, a1 = a0 + 2.5133f;
            L(12 + 9 * cosf(a0), 12.5f + 9 * sinf(a0), 12 + 9 * cosf(a1), 12.5f + 9 * sinf(a1));
        }
        break;
    }
    case ICON_AUDIO:
        L(4, 9, 8, 9); L(8, 9, 13, 5); L(13, 5, 13, 19); L(13, 19, 8, 15); L(8, 15, 4, 15); L(4, 15, 4, 9);
        L(16, 8, 18, 12); L(18, 12, 16, 16); L(19, 5, 22, 12); L(22, 12, 19, 19);
        break;
    case ICON_CONTROLS:
        L(5, 7, 19, 7); L(5, 17, 19, 17); L(5, 7, 2, 12); L(2, 12, 5, 17); L(19, 7, 22, 12); L(22, 12, 19, 17);
        L(7, 10, 7, 14); L(5, 12, 9, 12); kit_circle(x + 16 * u, y + 11 * u, 1.3f * u, c, 1); kit_circle(x + 18 * u, y + 13.5f * u, 1.3f * u, c, 1);
        break;
    case ICON_GAMEPLAY: {
        int i;
        for (i = 0; i < 9; i++) {
            float a0 = -2.4f + (float)i * 0.55f, a1 = a0 + 0.55f;
            L(12 + 8 * cosf(a0), 12 + 8 * sinf(a0), 12 + 8 * cosf(a1), 12 + 8 * sinf(a1));
        }
        L(3, 4, 3, 9); L(3, 9, 8, 9);
        break;
    }
    case ICON_MODS:
        L(4, 4, 11, 4); L(11, 4, 11, 11); L(11, 11, 4, 11); L(4, 11, 4, 4);
        L(13, 13, 20, 13); L(20, 13, 20, 20); L(20, 20, 13, 20); L(13, 20, 13, 13);
        L(13, 4, 20, 4); L(20, 4, 20, 11); L(4, 13, 11, 13); L(4, 13, 4, 20);
        break;
    case ICON_ONLINE:
        kit_ring(x + 12 * u, y + 12 * u, 8 * u, 8 * u + t, c, 1);
        L(3, 12, 21, 12); L(12, 3, 12, 21);
        kit_ellipse_ring(x + 12 * u, y + 12 * u, 4 * u, 9 * u, t * 0.9f, 0, c, 1);
        break;
    case ICON_BUILDS:
        L(5, 19, 14, 10); kit_ring(x + 16 * u, y + 8 * u, 3 * u, 3 * u + t, c, 1);
        break;
    case ICON_LOGS:
        L(5, 3, 19, 3); L(19, 3, 19, 21); L(19, 21, 5, 21); L(5, 21, 5, 3); L(8, 8, 16, 8); L(8, 12, 16, 12); L(8, 16, 13, 16);
        break;
    case ICON_ABOUT:
        kit_ring(x + 12 * u, y + 12 * u, 8 * u, 8 * u + t, c, 1);
        L(12, 11, 12, 17); kit_circle(x + 12 * u, y + 7.5f * u, 1.5f * u, c, 1);
        break;
    case ICON_CHECK:
        L(4, 13, 9, 18); L(9, 18, 20, 6);
        break;
    default:
        break;
    }
#undef L
}

float theme_key_width(const char *label, float h)
{
    float w = kit_text_width(FONT_LABEL, h * 0.62f, label) + h * 0.55f;
    return w < h * 1.3f ? h * 1.3f : w;
}

void theme_key(float x, float y, const char *label, float h)
{
    float w = theme_key_width(label, h);
    kit_rect(x, y, w, h, C_INK, 1);
    kit_rect(x + 2, y + 2, w - 4, h - 6, C_WHITE, 1);
    kit_text(FONT_LABEL, h * 0.62f, x + w / 2, y + h * 0.1f, label, C_INK, 1, ALIGN_CENTER);
}

void theme_button(float x, float y, float w, float h, const char *label, Rgb fill, Rgb fg, int focused, int disabled)
{
    kit_push();
    if (disabled)
        kit_alpha(0.45f);
    if (focused) {
        kit_rect(x - 5, y - 5, w + 10, h + 10, C_RED, 1);
    }
    theme_box(x, y, w, h, fill, 3, focused ? 6.0f : 4.0f);
    kit_text(FONT_DISPLAY, h * 0.62f, x + w / 2, y + h * 0.16f, label, fg, 1, ALIGN_CENTER);
    kit_pop();
}

void theme_halftone_bar(float x, float y, float w, float h, float frac, double t, int stripes)
{
    float fw = (w - 6) * (frac < 0 ? 0 : frac > 1 ? 1 : frac);
    theme_box(x, y, w, h, C_WHITE, 3, 0);
    if (fw <= 0)
        return;
    if (stripes) {
        float off = (float)fmod(t * 22.6, 22.6), sx;
        kit_rect(x + 3, y + 3, fw, h - 6, C_YELLOW, 1);
        for (sx = x + 3 - 22.6f + off; sx < x + 3 + fw; sx += 22.6f) {
            float a = sx, b = sx + 11.3f, p[8];
            float l = x + 3, r = x + 3 + fw;
            p[0] = a < l ? l : a > r ? r : a, p[1] = y + 3;
            p[2] = b < l ? l : b > r ? r : b, p[3] = y + 3;
            p[4] = (b - (h - 6)) < l ? l : (b - (h - 6)) > r ? r : (b - (h - 6)), p[5] = y + h - 3;
            p[6] = (a - (h - 6)) < l ? l : (a - (h - 6)) > r ? r : (a - (h - 6)), p[7] = y + h - 3;
            kit_quad(p, C_RED, 1);
        }
    } else {
        float dx, dy;
        kit_rect(x + 3, y + 3, fw, h - 6, C_YELLOW, 1);
        for (dy = y + 6; dy < y + h - 3; dy += 6)
            for (dx = x + 6; dx < x + 3 + fw; dx += 6)
                kit_circle(dx, dy, 1.3f, C_INK, 1);
    }
    kit_rect(x + 3 + fw - 3, y + 3, 3, h - 6, C_INK, 1);
}
