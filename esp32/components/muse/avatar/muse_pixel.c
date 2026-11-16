/*
 * NORTH POLE / SANTA / COMMUNICATOR: Gav's approved 128x128 screen, shown in
 * place of the animated avatar (with CONFIG_MUSE_FULLSCREEN_ART the UI makes
 * the canvas the whole screen, so it lands pixel for pixel).
 *
 * The picture never changes in idle, boot and off. In the other states the two
 * corner snowflakes take the state's accent colour: blue and gently pulsing
 * while listening, twinkling left/right while thinking, glowing with the voice
 * while speaking, and blinking while there's an error. While thinking, the
 * tree's star (idle and under-the-tree screens) and the white frame pulse
 * yellow together on a one-second step, in place of the thin bar along the
 * bottom edge. While a note is recording (muse_voice,
 * from the start click to the send click or whatever else ends it), the
 * COMMUNICATE label sits in a green box (npsc_art_rec.png).
 *
 * With the North Pole extras (CONFIG_MUSE_NPSC_*, muse_npsc.c) the screen is
 * one of several (muse_npsc_scene): the idle art, idle with the sleeps count,
 * the Christmas Eve tracker, the wake splash, the greeting, story time, Elf on
 * Duty or the jingle. Those come from npsc_screens.h
 * (design/make_feature_screens.py), and their live text (count, place, name)
 * is drawn here with the glyphs in that header; see LAYOUT below.
 *
 * Pixels are RGB565 with red in the high bits, the same packing the default
 * avatar's to565() uses. The frame is composed into a 128x128 buffer when
 * something changes; a still screen is the source image exactly.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sdkconfig.h"
#include "muse_npsc.h"
#include "muse_pixel.h"
#include "muse_voice.h"
#include "npsc_art.h"
#if CONFIG_MUSE_NPSC
#include "npsc_screens.h"
#endif

#define MAP_MAX 512
#define LEVELS 8     /* tint steps, so a slow pulse redraws a few times a second, not every frame */
#define CELLS (NPSC_W / 2)
#define GOLD 0xffd54a

static const uint32_t ACCENT[MUSE_MODE_COUNT] = {
    [MUSE_MODE_BOOT] = 0xa9c0ff,      [MUSE_MODE_IDLE] = 0xa77dff,  [MUSE_MODE_LISTENING] = 0x5cb8ff,
    [MUSE_MODE_THINKING] = 0xe07bff,  [MUSE_MODE_SPEAKING] = 0x6ff0bf,
    [MUSE_MODE_ERROR] = 0xff5c5c,     [MUSE_MODE_OFF] = 0x7c72d0,
};

static uint16_t *s_fb;        /* what's on screen, less the redraw flips */
static uint16_t *s_new;       /* the next frame, composed when the scene changes */
static bool s_fb_tried;
/* The UI redraws a 2x2 cell only when its top-left pixel changes (invalidate_muse
 * samples one pixel per cell). A change elsewhere in a cell toggles the cell's
 * bit, which flips that pixel's lowest green bit; once nothing changes, set bits
 * are cleared, one more redraw, so a still screen is the image exactly. */
static uint8_t s_flip[CELLS][CELLS / 8];
static uint8_t s_dirty[CELLS][CELLS / 8];   /* cells changed this frame: each toggles once */
static bool s_flip_set;       /* some bit may be set */
static bool s_changed;        /* put() changed a pixel this frame */
#define FLAKE_Y0 6            /* rows 6..13 hold both flakes */
#define FLAKE_Y1 13
static uint8_t s_fm[FLAKE_Y1 - FLAKE_Y0 + 1][NPSC_W];   /* 0, or 1 + which flake */
static uint16_t s_col[2];     /* the flakes' colours now */

static npsc_scene_t s_scene = { .id = (npsc_scene_id_t)-1 };
static bool s_rec;
static bool s_glow;           /* thinking: star + edge pulse */
static int s_glow_step;       /* 0..LEVELS */
static int s_star_y = -1;     /* tip y of the tree star, or -1 */
static uint16_t s_map[MAP_MAX];
static int s_size = NPSC_W;

uint32_t muse_pixel_accent(muse_mode_t mode)
{
    return ACCENT[mode < MUSE_MODE_COUNT ? mode : MUSE_MODE_IDLE];
}

static uint16_t pack(int r, int g, int b)
{
    return (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}

static uint16_t tint(uint32_t acc, int step)
{
    int r = 255 + (((acc >> 16) & 0xff) - 255) * step / LEVELS;
    int g = 255 + (((acc >> 8) & 0xff) - 255) * step / LEVELS;
    int b = 255 + ((acc & 0xff) - 255) * step / LEVELS;
    return pack(r, g, b);
}

static int quant(float k)
{
    k = k < 0 ? 0 : (k > 1 ? 1 : k);
    return (int)lroundf(k * LEVELS);
}

static const uint16_t *base_of(npsc_scene_id_t id)
{
#if CONFIG_MUSE_NPSC
    switch (id) {
    case NPSC_SCENE_UNDER: return NPSC_SCR_UNDER;
    case NPSC_SCENE_TRACKER: return NPSC_SCR_TRACKER;
    case NPSC_SCENE_WAKE: return NPSC_SCR_WAKE;
    case NPSC_SCENE_GREETING: return NPSC_SCR_GREETING;
    case NPSC_SCENE_STORY: return NPSC_SCR_STORY;
    case NPSC_SCENE_ELF: return NPSC_SCR_ELF;
    case NPSC_SCENE_JINGLE: return NPSC_SCR_JINGLE;
    case NPSC_SCENE_WAKE_RECORD: return NPSC_SCR_UNDER;  /* chrome only; the middle is cleared */
    default: break;
    }
#endif
    (void)id;
    return NPSC_ART;
}

#if CONFIG_MUSE_NPSC
/* ---- Live text ---- */

static void blend(uint16_t *px, uint32_t rgb, int a)
{
    uint16_t c = *px;
    int r = ((c >> 11) & 0x1f) << 3, g = ((c >> 5) & 0x3f) << 2, b = (c & 0x1f) << 3;
    r += ((int)((rgb >> 16) & 0xff) - r) * a / 255;
    g += ((int)((rgb >> 8) & 0xff) - g) * a / 255;
    b += ((int)(rgb & 0xff) - b) * a / 255;
    *px = pack(r, g, b);
}

static const npsc_glyph_t *glyph(const npsc_font_t *f, char c)
{
    return c >= 32 && c < 127 ? &f->g[c - 32] : &f->g[0];
}

/* The font's kerning between a and b, in 1/16 px at f's size (as PIL applies it). */
static int kern16(const npsc_font_t *f, char a, char b)
{
    uint16_t key = (uint16_t)(((uint8_t)a << 8) | (uint8_t)b);
    int lo = 0, hi = NPSC_KERN_N - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (NPSC_KERN_PAIR[mid] == key) {
            int v = NPSC_KERN_64[mid] * f->size;   /* 1/64 px at NPSC_KERN_SIZE */
            int d = NPSC_KERN_SIZE * 4;
            return v >= 0 ? (v + d / 2) / d : -((-v + d / 2) / d);
        }
        if (NPSC_KERN_PAIR[mid] < key) {
            lo = mid + 1;
        } else {
            hi = mid - 1;
        }
    }
    return 0;
}

/* Ink box of t with the pen starting at 0, as PIL's tight bbox. */
static void ink_box(const npsc_font_t *f, const char *t, int *x0, int *y0, int *x1, int *y1)
{
    int pen = 0;
    *x0 = *y0 = 1000;
    *x1 = *y1 = -1000;
    for (const char *s0 = t; *t; t++) {
        const npsc_glyph_t *g = glyph(f, *t);
        if (t > s0) {
            pen += kern16(f, t[-1], *t);
        }
        int x = (pen + 8) >> 4;
        if (g->w) {
            *x0 = x + g->dx < *x0 ? x + g->dx : *x0;
            *x1 = x + g->dx + g->w > *x1 ? x + g->dx + g->w : *x1;
            *y0 = g->dy < *y0 ? g->dy : *y0;
            *y1 = g->dy + g->h > *y1 ? g->dy + g->h : *y1;
        }
        pen += g->adv16;
    }
    if (*x1 < *x0) {
        *x0 = *y0 = *x1 = *y1 = 0;
    }
}

static int ink_w(const npsc_font_t *f, const char *t)
{
    int x0, y0, x1, y1;
    ink_box(f, t, &x0, &y0, &x1, &y1);
    return x1 - x0;
}

/* t centered on cx, the top of its ink at y (ctext_ink in the generator). */
static void text_ink(uint16_t *fb, const npsc_font_t *f, const char *t, int cx, int y, uint32_t rgb)
{
    int x0, y0, x1, y1;
    ink_box(f, t, &x0, &y0, &x1, &y1);
    int ox = cx - (x1 - x0) / 2 - x0, oy = y - y0, pen = 0;
    for (const char *s0 = t; *t; t++) {
        const npsc_glyph_t *g = glyph(f, *t);
        if (t > s0) {
            pen += kern16(f, t[-1], *t);
        }
        int gx = ox + ((pen + 8) >> 4) + g->dx, gy = oy + g->dy;
        const uint8_t *a = f->alpha + g->off;
        for (int j = 0; j < g->h; j++) {
            for (int i = 0; i < g->w; i++) {
                int x = gx + i, yy = gy + j;
                if (a[j * g->w + i] && x >= 3 && x < NPSC_W - 3 && yy >= 3 && yy < NPSC_H - 3) {
                    blend(&fb[yy * NPSC_W + x], rgb, a[j * g->w + i]);
                }
            }
        }
        pen += g->adv16;
    }
}

/* "<name>!" centered (both ways) in the band between "Ho ho ho" and the bar
 * (NPSC_GREET_TOP..BOTTOM, from the generator), with equal gaps above and
 * below: one line as big as fits, else two lines split at the space nearest
 * the middle ("Alex and" / "Sam!"), else smaller. */
#define GREET_W 112
#define GREET_MIN_GAP 4
static void greeting(uint16_t *fb, char *t)
{
    static const npsc_font_t *const FONTS[] = { &NPSC_F30, &NPSC_F20, &NPSC_F14 };
    const int band = NPSC_GREET_BOTTOM - NPSC_GREET_TOP + 1;
    int len = (int)strlen(t), split = -1;
    for (int i = 0; i < len; i++) {
        if (t[i] == ' ' && (split < 0 || abs(2 * i - len) < abs(2 * split - len))) {
            split = i;
        }
    }
    for (int f = 0; f < 3; f++) {
        const npsc_font_t *ft = FONTS[f];
        int x0, y0, x1, y1;
        ink_box(ft, t, &x0, &y0, &x1, &y1);
        if (x1 - x0 <= GREET_W && y1 - y0 <= band - 2 * GREET_MIN_GAP) {
            text_ink(fb, ft, t, 64, NPSC_GREET_TOP + (band - (y1 - y0)) / 2, GOLD);
            return;
        }
        if (split < 0) {
            continue;
        }
        t[split] = '\0';
        const char *a = t, *b = t + split + 1;
        int ax0, ay0, ax1, ay1, bx0, by0, bx1, by1;
        ink_box(ft, a, &ax0, &ay0, &ax1, &ay1);
        ink_box(ft, b, &bx0, &by0, &bx1, &by1);
        /* Baselines a font's line apart; the block is from a's ink top to b's ink bottom. */
        int pitch = ft->size + 1;   /* room for a descender over the next line's capitals */
        int h = pitch + by1 - ay0;
        if (ax1 - ax0 <= GREET_W && bx1 - bx0 <= GREET_W && h <= band - 2 * GREET_MIN_GAP) {
            int top = NPSC_GREET_TOP + (band - h) / 2;
            text_ink(fb, ft, a, 64, top, GOLD);
            text_ink(fb, ft, b, 64, top - ay0 + pitch + by0, GOLD);
            t[split] = ' ';
            return;
        }
        t[split] = ' ';
    }
    text_ink(fb, &NPSC_F14, t, 64, NPSC_GREET_TOP + GREET_MIN_GAP, GOLD);   /* clipped at the border */
}

/* LAYOUT: where each screen's live text goes (positions match the generator's
 * mockups: ink top y, centered on x = 64 unless noted). */
static void draw_text(uint16_t *fb, const npsc_scene_t *s)
{
    char buf[NPSC_NAME_MAX + 8];
    switch (s->id) {
    case NPSC_SCENE_UNDER:      /* LAYOUT_UNDER: under the tree, above the bar */
        if (s->sleeps == 0) {
            strlcpy(buf, "Merry Christmas!", sizeof(buf));
        } else {
            snprintf(buf, sizeof(buf), "%d sleep%s", s->sleeps, s->sleeps == 1 ? "" : "s");
        }
        text_ink(fb, &NPSC_F9, buf, 64, 97, GOLD);
        break;
    case NPSC_SCENE_TRACKER:    /* LAYOUT_TRACKER: the place under "is over..." */
        if (s->place) {
            const npsc_font_t *f = ink_w(&NPSC_F16, s->place) <= 116 ? &NPSC_F16 : &NPSC_F14;
            text_ink(fb, f, s->place, 64, f == &NPSC_F16 ? 56 : 58, GOLD);
        }
        break;
    case NPSC_SCENE_GREETING:   /* LAYOUT_GREETING: "<name>!" under "Ho ho ho," */
        snprintf(buf, sizeof(buf), "%s!", s->name);
        greeting(fb, buf);
        break;
    case NPSC_SCENE_WAKE: {     /* LAYOUT_WAKE: the count centered in the splash's tag (font 18 as in the art) */
        snprintf(buf, sizeof(buf), "%d", s->sleeps);
        const npsc_font_t *f = ink_w(&NPSC_F18, buf) <= NPSC_WAKE_TAG_X1 - NPSC_WAKE_TAG_X0 - 4 ? &NPSC_F18 : &NPSC_F14;
        int x0, y0, x1, y1;
        ink_box(f, buf, &x0, &y0, &x1, &y1);
        int cx = (NPSC_WAKE_TAG_X0 + NPSC_WAKE_TAG_X1 + 1) / 2;
        text_ink(fb, f, buf, cx, NPSC_WAKE_TAG_Y0 + (NPSC_WAKE_TAG_Y1 - NPSC_WAKE_TAG_Y0 + 1 - (y1 - y0)) / 2, GOLD);
        break;
    }
#if CONFIG_MUSE_NPSC_WAKE_RECORD
    case NPSC_SCENE_WAKE_RECORD: {
        const char *title = NULL, *mid = NULL, *sub = NULL;
        /* F18 is digits-only (the wake-splash count); F9 is a small subset.
         * F14/F16/F20 have the full alphabet. */
        const npsc_font_t *ft = &NPSC_F16, *fm = &NPSC_F16, *fs = &NPSC_F14;
        char midbuf[32], subbuf[32];
        switch (s->rec_phase) {
        case NPSC_WR_INTRO:
            title = "Hey Santa";
            sub = "a test of five";
            ft = &NPSC_F20;
            fs = &NPSC_F14;
            break;
        case NPSC_WR_CUE:
        case NPSC_WR_LISTEN:
        case NPSC_WR_SAVED:
        case NPSC_WR_SKIPPED:
            title = "Recording";
            ft = &NPSC_F16;
            if (s->rec_round > 0 && s->rec_total > 0) {
                snprintf(midbuf, sizeof(midbuf), "%d of %d", s->rec_round, s->rec_total);
                mid = midbuf;
            }
            if (s->rec_phase == NPSC_WR_LISTEN) {
                sub = "say hey santa now";
                fs = &NPSC_F14;
            } else if (s->rec_phase == NPSC_WR_SAVED) {
                sub = "saved";
                fs = &NPSC_F16;
            } else if (s->rec_phase == NPSC_WR_SKIPPED) {
                sub = "skipped";
                fs = &NPSC_F16;
            }
            break;
        case NPSC_WR_DONE:
            title = "All done!";
            ft = &NPSC_F20;
            snprintf(subbuf, sizeof(subbuf), "%d clip%s recorded", s->rec_saved,
                     s->rec_saved == 1 ? "" : "s");
            sub = subbuf;
            fs = &NPSC_F14;
            break;
        default:
            break;
        }
        if (title && mid && sub) {
            text_ink(fb, ft, title, 64, 36, GOLD);
            text_ink(fb, fm, mid, 64, 56, GOLD);
            text_ink(fb, fs, sub, 64, 82, GOLD);
        } else if (title && sub) {
            text_ink(fb, ft, title, 64, 44, GOLD);
            text_ink(fb, fs, sub, 64, 72, GOLD);
        } else if (title) {
            text_ink(fb, ft, title, 64, 56, GOLD);
        }
        break;
    }
#endif
    default:
        break;
    }
}
#endif

/* The screen for scene s, recording patch and all, but white snowflakes. */
static void compose(uint16_t *fb, const npsc_scene_t *s, bool rec)
{
    memcpy(fb, base_of(s->id), NPSC_W * NPSC_H * sizeof(uint16_t));
#if CONFIG_MUSE_NPSC_WAKE_RECORD
    if (s->id == NPSC_SCENE_WAKE_RECORD) {
        uint16_t red = NPSC_SCR_UNDER[40 * NPSC_W + 8];
        for (int y = 30; y <= 105; y++) {
            for (int x = 4; x <= NPSC_W - 5; x++) {
                fb[y * NPSC_W + x] = red;
            }
        }
    }
#endif
#if CONFIG_MUSE_NPSC
    draw_text(fb, s);
#endif
    if (rec && (s->id == NPSC_SCENE_IDLE || s->id == NPSC_SCENE_UNDER)) {
        for (int y = NPSC_REC_Y0; y <= NPSC_REC_Y1; y++) {
            memcpy(&fb[y * NPSC_W + NPSC_REC_X0], &NPSC_REC[(y - NPSC_REC_Y0) * NPSC_REC_W],
                   NPSC_REC_W * sizeof(uint16_t));
        }
    }
}


static uint16_t want(int x, int y);
static void put(int x, int y, uint16_t c);

/* Tree-star tip (x, y): idle art at (63, 82), under-the-tree screen 10 px higher. */
#define STAR_X 63
#define STAR_IDLE_Y 82
#define STAR_UNDER_Y 72
#define GLOW_EDGE 10          /* outer band refreshed when the pulse steps */
static const int8_t STAR_OFF[12][2] = {
    {0, 0}, {1, 0}, {0, 1}, {1, 1},
    {-2, 2}, {-1, 2}, {2, 2}, {3, 2},
    {-2, 3}, {-1, 3}, {2, 3}, {3, 3},
};

static int star_y_of(npsc_scene_id_t id)
{
    if (id == NPSC_SCENE_IDLE) {
        return STAR_IDLE_Y;
    }
#if CONFIG_MUSE_NPSC
    if (id == NPSC_SCENE_UNDER) {
        return STAR_UNDER_Y;
    }
#endif
    return -1;
}

static bool is_star(int x, int y)
{
    if (s_star_y < 0) {
        return false;
    }
    for (int i = 0; i < 12; i++) {
        if (x == STAR_X + STAR_OFF[i][0] && y == s_star_y + STAR_OFF[i][1]) {
            return true;
        }
    }
    return false;
}

static float star_dist(int x, int y)
{
    float best = 99.0f;
    for (int i = 0; i < 12; i++) {
        float dx = (float)(x - (STAR_X + STAR_OFF[i][0]));
        float dy = (float)(y - (s_star_y + STAR_OFF[i][1]));
        float d = sqrtf(dx * dx + dy * dy);
        if (d < best) {
            best = d;
        }
    }
    return best;
}

static uint16_t mix_rgb(int r0, int g0, int b0, int r1, int g1, int b1, float t)
{
    if (t < 0) {
        t = 0;
    }
    if (t > 1) {
        t = 1;
    }
    return pack((int)lroundf((float)r0 + (float)(r1 - r0) * t),
                (int)lroundf((float)g0 + (float)(g1 - g0) * t),
                (int)lroundf((float)b0 + (float)(b1 - b0) * t));
}

static uint16_t glow_mix(uint16_t c, int gr, int gg, int gb, float a)
{
    if (a <= 0) {
        return c;
    }
    if (a > 1) {
        a = 1;
    }
    int r = ((c >> 11) & 0x1f) << 3, g = ((c >> 5) & 0x3f) << 2, b = (c & 0x1f) << 3;
    r += (int)lroundf((float)(gr - r) * a);
    g += (int)lroundf((float)(gg - g) * a);
    b += (int)lroundf((float)(gb - b) * a);
    return pack(r, g, b);
}

/* Yellow edge glow and tree-star pulse, in sync, while thinking. */
static uint16_t glow_px(int x, int y, uint16_t c)
{
    float p = (float)s_glow_step / (float)LEVELS;
    int d = x;
    if (y < d) {
        d = y;
    }
    if (NPSC_W - 1 - x < d) {
        d = NPSC_W - 1 - x;
    }
    if (NPSC_H - 1 - y < d) {
        d = NPSC_H - 1 - y;
    }
    int r = ((c >> 11) & 0x1f) << 3, g = ((c >> 5) & 0x3f) << 2, b = (c & 0x1f) << 3;
    if (d <= 1) {
        c = mix_rgb(r, g, b, 255, 213, 74, 0.25f + 0.75f * p);
    } else if (d <= 3) {
        c = mix_rgb(255, 255, 255, 255, 213, 74, 0.35f + 0.65f * p);
    } else {
        float max_in = 4.0f + 5.0f * p;
        if ((float)d < max_in) {
            float u = ((float)d - 3.0f) / (max_in - 3.0f);
            if (u < 0) {
                u = 0;
            }
            if (u > 1) {
                u = 1;
            }
            float a = powf(1.0f - u, 1.25f) * p * 0.80f;
            if (c == 0xffff) {
                a *= 0.35f;
            }
            c = glow_mix(c, 255, 220, 90, a);
        }
    }

    if (s_star_y < 0) {
        return c;
    }
    if (is_star(x, y)) {
        /* Dim gold to a hot white-yellow. */
        return mix_rgb(204, 156, 55, 255, 250, 190, p);
    }
    float rad = 1.2f + 4.2f * p;
    float dist = star_dist(x, y);
    if (dist <= rad) {
        float a = powf(1.0f - dist / rad, 1.35f) * p * 0.92f;
        int cr = ((c >> 11) & 0x1f) << 3, cg = ((c >> 5) & 0x3f) << 2, cb = (c & 0x1f) << 3;
        if (cg > cr + 20 && cg > cb) {
            a *= 0.28f;
        }
        c = glow_mix(c, 255, 220, 90, a);
    }
    if (p > 0.35f) {
        static const int8_t RAY[][2] = {
            {0, -1}, {0, -2}, {0, -3},
            {0, 1}, {0, 2},
            {-1, 0}, {-2, 0}, {-3, 0},
            {1, 0}, {2, 0}, {3, 0},
        };
        float k = (p - 0.35f) / 0.65f;
        for (int i = 0; i < 11; i++) {
            int dx = RAY[i][0], dy = RAY[i][1];
            int rx, ry;
            if (dy < 0) {
                rx = STAR_X;
                ry = s_star_y + dy;
            } else if (dy > 0) {
                rx = STAR_X;
                ry = s_star_y + 3 + dy;
            } else if (dx < 0) {
                rx = STAR_X - 2 + dx;
                ry = s_star_y + 2;
            } else {
                rx = STAR_X + 3 + dx;
                ry = s_star_y + 2;
            }
            if (x == rx && y == ry) {
                int ad = (dx < 0 ? -dx : dx) + (dy < 0 ? -dy : dy);
                float a = k * (1.0f - (float)(ad - 1) / 3.0f) * 0.85f;
                int cr = ((c >> 11) & 0x1f) << 3, cg = ((c >> 5) & 0x3f) << 2, cb = (c & 0x1f) << 3;
                if (cg > cr + 20 && cg > cb) {
                    a *= 0.25f;
                }
                c = glow_mix(c, 255, 240, 150, a);
                break;
            }
        }
    }
    return c;
}

static void refresh_glow(void)
{
    for (int y = 0; y < NPSC_H; y++) {
        for (int x = 0; x < NPSC_W; x++) {
            int d = x;
            if (y < d) {
                d = y;
            }
            if (NPSC_W - 1 - x < d) {
                d = NPSC_W - 1 - x;
            }
            if (NPSC_H - 1 - y < d) {
                d = NPSC_H - 1 - y;
            }
            if (d < GLOW_EDGE) {
                put(x, y, want(x, y));
                continue;
            }
            /* Both tree positions, so a scene change's leftover halo is cleared. */
            if ((y >= STAR_IDLE_Y - GLOW_EDGE && y <= STAR_IDLE_Y + 3 + GLOW_EDGE &&
                 x >= STAR_X - 3 - GLOW_EDGE && x <= STAR_X + 4 + GLOW_EDGE) ||
                (y >= STAR_UNDER_Y - GLOW_EDGE && y <= STAR_UNDER_Y + 3 + GLOW_EDGE &&
                 x >= STAR_X - 3 - GLOW_EDGE && x <= STAR_X + 4 + GLOW_EDGE)) {
                put(x, y, want(x, y));
            }
        }
    }
}

/* Pixel (x, y) of the frame: the composed scene, with its white snowflake
 * pixels (where the screen's art has them) in the flakes' colours. */
static uint16_t want(int x, int y)
{
    uint16_t c = s_new[y * NPSC_W + x];
    if (y >= FLAKE_Y0 && y <= FLAKE_Y1 && s_fm[y - FLAKE_Y0][x] && base_of(s_scene.id)[y * NPSC_W + x] == 0xffff) {
        c = s_col[s_fm[y - FLAKE_Y0][x] - 1];
    }
    if (s_glow) {
        c = glow_px(x, y, c);
    }
    return c;
}

static void put(int x, int y, uint16_t c)
{
    uint16_t *p = &s_fb[y * NPSC_W + x];
    if (*p != c) {
        *p = c;
        s_dirty[y >> 1][x >> 4] |= (uint8_t)(1 << ((x >> 1) & 7));
        s_changed = true;
    }
}

void muse_pixel_render(const muse_pose_t *p)
{
    if (!s_fb_tried) {
        s_fb_tried = true;
        s_fb = malloc(NPSC_W * NPSC_H * sizeof(uint16_t));
        s_new = malloc(NPSC_W * NPSC_H * sizeof(uint16_t));
        if (s_fb && s_new) {
            memcpy(s_fb, NPSC_ART, NPSC_W * NPSC_H * sizeof(uint16_t));
            memcpy(s_new, NPSC_ART, NPSC_W * NPSC_H * sizeof(uint16_t));
            for (int i = 0; i < NPSC_FLAKE_N; i++) {
                s_fm[NPSC_FLAKE[i][1] - FLAKE_Y0][NPSC_FLAKE[i][0]] = (uint8_t)(1 + NPSC_FLAKE[i][2]);
            }
            s_col[0] = s_col[1] = 0xffff;
            s_scene.id = NPSC_SCENE_IDLE;
            s_scene.sleeps = -1;
            s_scene.place = NULL;
            s_scene.name[0] = '\0';
        } else {
            free(s_fb);
            free(s_new);
            s_fb = s_new = NULL;   /* the approved art, still */
        }
    }
    if (!s_fb) {
        return;
    }
    const float tau = 6.2831853f;
    int k[2] = { 0, 0 };
    switch (p->mode) {
    case MUSE_MODE_LISTENING:
        k[0] = k[1] = quant(0.75f + 0.25f * sinf(p->mode_t * tau * 0.8f));
        break;
    case MUSE_MODE_THINKING: {
        float s = sinf(p->mode_t * tau * 1.2f);
        k[0] = quant(s);
        k[1] = quant(-s);
        break;
    }
    case MUSE_MODE_SPEAKING:
        k[0] = k[1] = quant(0.35f + p->level * 1.3f);
        break;
    case MUSE_MODE_ERROR:
        k[0] = k[1] = fmodf(p->mode_t, 1.0f) < 0.5f ? LEVELS : 0;
        break;
    default:   /* boot, idle, off: exactly as approved */
        break;
    }

    npsc_scene_t s;
    muse_npsc_scene(&s);
    bool rec = muse_voice_recording();
    bool scene_changed = s.id != s_scene.id || s.sleeps != s_scene.sleeps || s.place != s_scene.place
                         || strcmp(s.name, s_scene.name) || rec != s_rec
                         || s.rec_round != s_scene.rec_round || s.rec_total != s_scene.rec_total
                         || s.rec_saved != s_scene.rec_saved || s.rec_phase != s_scene.rec_phase;
    uint32_t acc = muse_pixel_accent(p->mode);
    uint16_t col[2] = { tint(acc, k[0]), tint(acc, k[1]) };
    bool flakes_changed = col[0] != s_col[0] || col[1] != s_col[1];
    s_col[0] = col[0];
    s_col[1] = col[1];
    bool glow = p->mode == MUSE_MODE_THINKING;
    int glow_step = glow ? quant(0.5f - 0.5f * cosf(p->mode_t * tau)) : 0;
    int star_y = star_y_of(s.id);
    bool glow_changed = glow != s_glow || (glow && (glow_step != s_glow_step || star_y != s_star_y));
    s_glow = glow;
    s_glow_step = glow_step;
    s_star_y = star_y;
    s_changed = false;
    if (scene_changed) {
        s_scene = s;
        s_rec = rec;
        compose(s_new, &s, rec);
        for (int y = 0; y < NPSC_H; y++) {
            for (int x = 0; x < NPSC_W; x++) {
                put(x, y, want(x, y));
            }
        }
    } else if (flakes_changed || glow_changed) {
        if (flakes_changed) {
            for (int i = 0; i < NPSC_FLAKE_N; i++) {
                put(NPSC_FLAKE[i][0], NPSC_FLAKE[i][1], want(NPSC_FLAKE[i][0], NPSC_FLAKE[i][1]));
            }
        }
        if (glow_changed) {
            refresh_glow();
        }
    }
    if (s_changed) {
        uint8_t *f = &s_flip[0][0], *d = &s_dirty[0][0];
        for (size_t i = 0; i < sizeof(s_flip); i++) {
            f[i] ^= d[i];
            d[i] = 0;
        }
        s_flip_set = true;
    } else if (s_flip_set) {
        memset(s_flip, 0, sizeof(s_flip));   /* settled: the exact image */
        s_flip_set = false;
    }
}

void muse_pixel_set_size(int px)
{
    s_size = px < 1 ? 1 : (px < MAP_MAX ? px : MAP_MAX);
    for (int i = 0; i < s_size; i++) {
        s_map[i] = (uint16_t)(i * NPSC_W / s_size);
    }
}

void muse_pixel_scale(uint16_t *dst, int stride_px, int x0, int x1, int y0, int y1)
{
    const uint16_t *src = s_fb ? s_fb : NPSC_ART;
    int n = x1 - x0 + 1;
    for (int y = y0; y <= y1; y++, dst += stride_px) {
        if (s_size == NPSC_W) {
            memcpy(dst, &src[y * NPSC_W + x0], n * sizeof(uint16_t));
            if (s_fb && !(y & 1)) {
                const uint8_t *row = s_flip[y >> 1];
                for (int x = (x0 + 1) & ~1; x <= x1; x += 2) {
                    if (row[x >> 4] & (1 << ((x >> 1) & 7))) {
                        dst[x - x0] ^= 0x20;   /* green's lowest bit, the finest step RGB565 has */
                    }
                }
            }
            continue;
        }
        int sy = s_map[y];
        for (int i = 0; i < n; i++) {
            dst[i] = src[sy * NPSC_W + s_map[x0 + i]];
        }
    }
}
