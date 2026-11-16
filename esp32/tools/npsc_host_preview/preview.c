/* Host render of components/muse/avatar/muse_pixel.c — the firmware path.
 * Builds with stub sdkconfig/muse_voice; output is 128x128 RGB PPM.
 *
 *   preview DIR
 *     writes thinking-idle-00.ppm .. thinking-idle-15.ppm (1 s pulse)
 *     and thinking-under.ppm (peak pulse, 40 sleeps)
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "muse_npsc.h"
#include "muse_pixel.h"

static npsc_scene_t s_scene = {
    .id = NPSC_SCENE_IDLE,
    .sleeps = -1,
    .place = NULL,
    .name = {0},
};

void muse_npsc_scene(npsc_scene_t *out)
{
    *out = s_scene;
}

bool muse_voice_recording(void)
{
    return false;
}

static uint16_t frame[128 * 128];

static void settle(const muse_pose_t *p)
{
    /* One changing frame may flip a cell's lowest green bit; the next
     * unchanged frame clears it, so the capture is the exact image. */
    muse_pixel_render(p);
    muse_pixel_render(p);
}

static void save_ppm(const char *path)
{
    muse_pixel_set_size(128);
    muse_pixel_scale(frame, 128, 0, 127, 0, 127);
    FILE *f = fopen(path, "wb");
    if (!f) {
        perror(path);
        return;
    }
    fprintf(f, "P6 128 128 255\n");
    for (int i = 0; i < 128 * 128; i++) {
        uint16_t c = frame[i];
        unsigned char rgb[3] = {
            (unsigned char)(((c >> 11) & 31) * 255 / 31),
            (unsigned char)(((c >> 5) & 63) * 255 / 63),
            (unsigned char)((c & 31) * 255 / 31),
        };
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
}

int main(int argc, char **argv)
{
    const char *dir = argc > 1 ? argv[1] : ".";
    char path[512];
    muse_pose_t p;
    memset(&p, 0, sizeof(p));
    p.mode = MUSE_MODE_THINKING;
    p.t = 10.0f;

    s_scene.id = NPSC_SCENE_IDLE;
    s_scene.sleeps = -1;
    for (int i = 0; i < 16; i++) {
        p.mode_t = (float)i / 16.0f;
        settle(&p);
        snprintf(path, sizeof(path), "%s/thinking-idle-%02d.ppm", dir, i);
        save_ppm(path);
    }

    s_scene.id = NPSC_SCENE_UNDER;
    s_scene.sleeps = 40;   /* Nov 15 to Dec 25 */
    p.mode_t = 0.5f;       /* peak of the 1 s pulse, both glows together */
    settle(&p);
    snprintf(path, sizeof(path), "%s/thinking-under.ppm", dir);
    save_ppm(path);
    return 0;
}
