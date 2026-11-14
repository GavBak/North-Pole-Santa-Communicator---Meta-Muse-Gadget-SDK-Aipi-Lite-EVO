/*
 * NORTH POLE / SANTA / COMMUNICATOR: the 128x128 screen design, shown in
 * place of the animated avatar (with CONFIG_MUSE_FULLSCREEN_ART the UI makes
 * the canvas the whole screen, so it lands pixel for pixel).
 *
 * The picture never changes in idle, boot and off. In the other states the two
 * corner snowflakes take the state's accent colour, the same one the bar along
 * the bottom edge uses: blue and gently pulsing while listening, twinkling
 * left/right while thinking, glowing with the voice while speaking, and
 * blinking while there's an error.
 *
 * Pixels come from npsc_art.h (make_art.py from npsc_art.png) as RGB565 with
 * red in the high bits, the same packing the default avatar's to565() uses.
 */
#include <math.h>
#include <string.h>

#include "muse_pixel.h"
#include "npsc_art.h"

#define FLAKE_Y0 6   /* rows 6..13 cover both flakes' 2x2 cells */
#define FLAKE_Y1 13
#define MAP_MAX 512
#define LEVELS 8     /* tint steps, so a slow pulse redraws a few times a second, not every frame */

static const uint32_t ACCENT[MUSE_MODE_COUNT] = {
    [MUSE_MODE_BOOT] = 0xa9c0ff,      [MUSE_MODE_IDLE] = 0xa77dff,  [MUSE_MODE_LISTENING] = 0x5cb8ff,
    [MUSE_MODE_THINKING] = 0xe07bff,  [MUSE_MODE_SPEAKING] = 0x6ff0bf,
    [MUSE_MODE_ERROR] = 0xff5c5c,     [MUSE_MODE_OFF] = 0x7c72d0,
};

static uint8_t s_fm[FLAKE_Y1 - FLAKE_Y0 + 1][NPSC_W];   /* 0, or 1 + which flake */
static bool s_fm_ready;
static uint16_t s_flake_col[2] = { 0xffff, 0xffff };    /* white, as drawn */
static unsigned s_gen;                                   /* odd: cue cells' sampled pixels carry a flipped LSB */
static uint16_t s_map[MAP_MAX];
static int s_size = NPSC_W;

uint32_t muse_pixel_accent(muse_mode_t mode)
{
    return ACCENT[mode < MUSE_MODE_COUNT ? mode : MUSE_MODE_IDLE];
}

static void init_mask(void)
{
    for (int i = 0; i < NPSC_FLAKE_N; i++) {
        s_fm[NPSC_FLAKE[i][1] - FLAKE_Y0][NPSC_FLAKE[i][0]] = (uint8_t)(1 + NPSC_FLAKE[i][2]);
    }
    s_fm_ready = true;
}

static uint16_t tint(uint32_t acc, int step)
{
    int r = 255 + (((acc >> 16) & 0xff) - 255) * step / LEVELS;
    int g = 255 + (((acc >> 8) & 0xff) - 255) * step / LEVELS;
    int b = 255 + ((acc & 0xff) - 255) * step / LEVELS;
    return (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}

static int quant(float k)
{
    k = k < 0 ? 0 : (k > 1 ? 1 : k);
    return (int)lroundf(k * LEVELS);
}

void muse_pixel_render(const muse_pose_t *p)
{
    if (!s_fm_ready) {
        init_mask();
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
    uint32_t acc = muse_pixel_accent(p->mode);
    uint16_t col[2] = { tint(acc, k[0]), tint(acc, k[1]) };
    bool changed = col[0] != s_flake_col[0] || col[1] != s_flake_col[1];
    s_flake_col[0] = col[0];
    s_flake_col[1] = col[1];
    /* The UI redraws a 2x2 cell only when its top-left pixel changes, which a
     * flake pixel elsewhere in the cell doesn't do, so each change flips the
     * LSB of blue on those top-left pixels. Once back to white, one more step
     * puts them back too, so a still screen is the approved image exactly. */
    if (changed || (s_gen & 1 && k[0] == 0 && k[1] == 0)) {
        s_gen++;
    }
}

void muse_pixel_set_size(int px)
{
    s_size = px < 1 ? 1 : (px < MAP_MAX ? px : MAP_MAX);
    for (int i = 0; i < s_size; i++) {
        s_map[i] = (uint16_t)(i * NPSC_W / s_size);
    }
}

static inline bool in_cue_cells(int x)
{
    return (x >= 6 && x <= 13) || (x >= 114 && x <= 121);
}

static inline uint16_t art_px(int x, int y)
{
    uint16_t c = NPSC_ART[y * NPSC_W + x];
    if (y >= FLAKE_Y0 && y <= FLAKE_Y1 && in_cue_cells(x)) {
        uint8_t f = s_fm_ready ? s_fm[y - FLAKE_Y0][x] : 0;
        if (f) {
            c = s_flake_col[f - 1];
        }
        if ((s_gen & 1) && !(x & 1) && !(y & 1)) {
            c ^= 1;
        }
    }
    return c;
}

void muse_pixel_scale(uint16_t *dst, int stride_px, int x0, int x1, int y0, int y1)
{
    int n = x1 - x0 + 1;
    for (int y = y0; y <= y1; y++, dst += stride_px) {
        int sy = s_size == NPSC_W ? y : s_map[y];
        if (s_size == NPSC_W && (sy < FLAKE_Y0 || sy > FLAKE_Y1)) {
            memcpy(dst, &NPSC_ART[sy * NPSC_W + x0], n * sizeof(uint16_t));
            continue;
        }
        for (int i = 0; i < n; i++) {
            int sx = s_size == NPSC_W ? x0 + i : s_map[x0 + i];
            dst[i] = art_px(sx, sy);
        }
    }
}
