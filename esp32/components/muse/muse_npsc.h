/*
 * North Pole Santa Communicator extras (CONFIG_MUSE_NPSC_*): the clock, the
 * scenes avatar/muse_pixel.c draws (countdown, wake splash, greeting, story,
 * Elf on Duty, sleigh tracker, jingle), story and goodnight requests to
 * Muse, and the kid's name and bedtime settings.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "sdkconfig.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    NPSC_SCENE_IDLE,       /* the approved idle art */
    NPSC_SCENE_UNDER,      /* idle with "<sleeps> sleeps" under the tree */
    NPSC_SCENE_TRACKER,    /* Christmas Eve: "Santa's sleigh is over... <place>" */
    NPSC_SCENE_WAKE,       /* wake splash with the sleeps count (placeholder art) */
    NPSC_SCENE_GREETING,   /* "Ho ho ho, <name>!" */
    NPSC_SCENE_STORY,
    NPSC_SCENE_ELF,
    NPSC_SCENE_JINGLE,
    NPSC_SCENE_WAKE_RECORD,  /* four MENU presses: record "hey santa" clips */
} npsc_scene_id_t;

/* Live text for NPSC_SCENE_WAKE_RECORD (otherwise ignored). */
typedef enum {
    NPSC_WR_IDLE = 0,
    NPSC_WR_INTRO,     /* "Hey Santa" / "a test of five" */
    NPSC_WR_CUE,       /* "Recording x of 5" while Santa says "Again!" */
    NPSC_WR_LISTEN,    /* plus "say hey santa now" after the beep */
    NPSC_WR_SAVED,
    NPSC_WR_SKIPPED,
    NPSC_WR_DONE,      /* "N clips recorded" */
} npsc_wr_phase_t;

#define NPSC_NAME_MAX 24   /* room for two names: "Alex and Sam" */

typedef struct {
    npsc_scene_id_t id;
    int sleeps;                /* days to Dec 25, 0 on the day; -1 unknown */
    const char *place;         /* NPSC_SCENE_TRACKER */
    char name[NPSC_NAME_MAX + 1];
    int rec_round;             /* 1..N, 0 on intro/done */
    int rec_total;
    int rec_saved;             /* clips kept this session */
    npsc_wr_phase_t rec_phase;
} npsc_scene_t;

#if CONFIG_MUSE_NPSC
/* Starts the clock/bedtime/story task and loads the settings. */
void muse_npsc_start(void);
/* The scene to draw now; cheap, called every frame by the UI. */
void muse_npsc_scene(npsc_scene_t *out);
/* Any button press. True if it was used up: it stopped a story or a
 * goodnight, or ended Elf on Duty on a lit screen. */
bool muse_npsc_press(void);
/* Backlight cap in percent while Elf on Duty shows, else -1. */
int muse_npsc_dim_pct(void);

void muse_npsc_story_start(void);
bool muse_npsc_story_active(void);
void muse_npsc_jingle(void);

#if CONFIG_MUSE_NPSC_WAKE_RECORD
#define NPSC_WAKE_RECORD_ROUNDS 5
#define NPSC_WAKE_RECORD_CAP_S 3
#define NPSC_WAKE_RECORD_START_S 5
#define NPSC_WAKE_RECORD_MAX_CLIPS 20   /* 3 s 16 kHz 16-bit mono each; PSRAM until reboot */

/* Four MENU presses: queue a session for the voice task. */
void muse_npsc_wake_record_start(void);
bool muse_npsc_wake_record_wanted(void);
bool muse_npsc_wake_record_active(void);
/* Voice task: take the request, update the screen, save a clip, finish. */
void muse_npsc_wake_record_begin(void);
void muse_npsc_wake_record_cancel(void);
bool muse_npsc_wake_record_cancelled(void);
void muse_npsc_wake_record_set(int round, npsc_wr_phase_t phase, int saved);
bool muse_npsc_wake_record_save(const int16_t *pcm, size_t frames);
int muse_npsc_wake_record_session_saved(void);
int muse_npsc_wake_record_stored(void);
void muse_npsc_wake_record_dump(void);
void muse_npsc_wake_record_finish(void);
#endif

/* The kid's name ("" when not set) and bedtime (menu). */
void muse_npsc_name(char *out, size_t cap);
void muse_npsc_set_name(const char *name);
void muse_npsc_bedtime_text(char *out, size_t cap);
void muse_npsc_bedtime_step(int direction);
#else
static inline void muse_npsc_scene(npsc_scene_t *out)
{
    out->id = NPSC_SCENE_IDLE;
    out->sleeps = -1;
    out->place = NULL;
    out->name[0] = '\0';
    out->rec_round = 0;
    out->rec_total = 0;
    out->rec_saved = 0;
    out->rec_phase = NPSC_WR_IDLE;
}
#endif

#ifdef __cplusplus
}
#endif
