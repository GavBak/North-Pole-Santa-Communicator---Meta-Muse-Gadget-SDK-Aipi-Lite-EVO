/*
 * North Pole Santa Communicator extras (CONFIG_MUSE_NPSC_*). See muse_npsc.h
 * and the options' help in Kconfig. A task here sets the clock, watches for
 * waking and bedtime, tells stories (avatar/npsc_stories.h, spoken with the
 * ElevenLabs voice) and follows a goodnight while Muse answers;
 * avatar/muse_pixel.c asks muse_npsc_scene() what to draw every frame.
 */
#include "muse_npsc.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#if CONFIG_MUSE_NPSC_CLOCK
#include "esp_netif_sntp.h"
#endif

#include "muse_chat.h"
#include "muse_state.h"
#include "muse_voice.h"
#include "muse_wifi.h"
#if CONFIG_MUSE_NPSC_STORY
#include "avatar/npsc_stories.h"
#endif
#if CONFIG_MUSE_NPSC_WAKE_RECORD
#include "muse_audio.h"
#include "muse_mem.h"
#endif

static const char *TAG = "npsc";

#define POLL_MS 100
#define WAKE_GREET_US 2500000       /* on waking, the greeting first */
#define WAKE_SPLASH_US 3000000      /* then the countdown splash, then idle */
#define JINGLE_SHOW_US 3600000      /* the 3.34 s sleigh-bell clip, and a little */
#define STORY_WAIT_US 20000000      /* the speech's first audio, with time to spare */
#define GOODNIGHT_DELAY_US 1500000  /* let Wi-Fi come back up from resting */
#define ELF_DIM_PCT 10
#define VALID_EPOCH 1735689600      /* 2025-01-01: anything earlier isn't set */

/* The pretend Christmas Eve route, from 5 pm local time, half an hour per
 * stop, arriving around midnight. Entirely made up; no network data. */
static const char *const ROUTE[] = {
    "the North Pole", "Greenland", "Iceland", "the Atlantic", "Newfoundland",
    "Quebec", "the Great Lakes", "Minnesota", "the Rockies", "Nevada",
    "the Sierras", "your town!", "your street!", "your rooftop!",
};
#define ROUTE_N (sizeof(ROUTE) / sizeof(ROUTE[0]))

typedef enum { STORY_OFF, STORY_WAIT, STORY_PLAY } story_t;

static nvs_handle_t s_nvs;
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static char s_name[NPSC_NAME_MAX + 1];
static volatile int s_bedtime = 20 * 60;   /* minutes after midnight, -1 off */

static volatile bool s_synced;
static volatile int s_sleeps = -1;
static const char *volatile s_place;       /* Christmas Eve evening, else NULL */

static volatile int64_t s_greet_until, s_splash_from, s_splash_until, s_jingle_until;
static volatile story_t s_story;
static int64_t s_story_at;
static volatile bool s_elf;
static volatile int64_t s_goodnight_at;     /* 0: none due */

#if CONFIG_MUSE_NPSC_WAKE_RECORD
static volatile bool s_wr_wanted, s_wr_active, s_wr_cancel;
static volatile int s_wr_round, s_wr_saved;
static volatile npsc_wr_phase_t s_wr_phase;
typedef struct {
    int16_t *pcm;
    size_t frames;
} wr_clip_t;
static wr_clip_t s_wr_clips[NPSC_WAKE_RECORD_MAX_CLIPS];
static int s_wr_stored;
#endif

static int64_t now_us(void)
{
    return esp_timer_get_time();
}

/* ---- Settings ---- */

static const int BEDTIMES[] = { -1, 19 * 60, 19 * 60 + 30, 20 * 60, 20 * 60 + 30, 21 * 60 };

/* Letters (case kept, the first one capitalized) and single spaces, - ' . & , */
static void tidy_name(const char *in, char *out, size_t cap)
{
    size_t n = 0;
    for (; *in && n + 1 < cap; in++) {
        char c = *in;
        if (isalpha((unsigned char)c)) {
            out[n] = n ? c : (char)toupper((unsigned char)c);
            n++;
        } else if ((c == ' ' || c == '-' || c == '\'' || c == '.' || c == '&' || c == ',') && n && out[n - 1] != ' ') {
            out[n++] = c;
        }
    }
    while (n && out[n - 1] == ' ') {
        n--;
    }
    out[n] = '\0';
}

void muse_npsc_name(char *out, size_t cap)
{
    taskENTER_CRITICAL(&s_lock);
    strlcpy(out, s_name, cap);
    taskEXIT_CRITICAL(&s_lock);
}

void muse_npsc_set_name(const char *name)
{
    char t[NPSC_NAME_MAX + 1];
    tidy_name(name, t, sizeof(t));
    taskENTER_CRITICAL(&s_lock);
    strlcpy(s_name, t, sizeof(s_name));
    taskEXIT_CRITICAL(&s_lock);
    if (s_nvs) {
        nvs_set_str(s_nvs, "kid_name", t);
        nvs_commit(s_nvs);
    }
    ESP_LOGI(TAG, "kid's name %s", t[0] ? "set" : "cleared");
}

void muse_npsc_bedtime_text(char *out, size_t cap)
{
    int b = s_bedtime;
    if (b < 0) {
        strlcpy(out, "Off", cap);
    } else {
        int h = b / 60 % 12;
        snprintf(out, cap, "%d:%02d %s", h ? h : 12, b % 60, b >= 12 * 60 ? "pm" : "am");
    }
}

void muse_npsc_bedtime_step(int direction)
{
    int n = sizeof(BEDTIMES) / sizeof(BEDTIMES[0]), i = 0;
    while (i < n && BEDTIMES[i] != s_bedtime) {
        i++;
    }
    i = i >= n ? 0 : (i + (direction < 0 ? n - 1 : 1)) % n;
    s_bedtime = BEDTIMES[i];
    if (s_nvs) {
        nvs_set_i16(s_nvs, "bedtime", (int16_t)s_bedtime);
        nvs_commit(s_nvs);
    }
}

static void load_settings(void)
{
    if (nvs_open("npsc", NVS_READWRITE, &s_nvs) != ESP_OK) {
        s_nvs = 0;
        return;
    }
    size_t len = sizeof(s_name);
    if (nvs_get_str(s_nvs, "kid_name", s_name, &len) != ESP_OK) {
#ifdef CONFIG_MUSE_NPSC_KID_NAME_DEFAULT
        tidy_name(CONFIG_MUSE_NPSC_KID_NAME_DEFAULT, s_name, sizeof(s_name));   /* none picked yet */
#else
        s_name[0] = '\0';
#endif
    }
    int16_t b;
    if (nvs_get_i16(s_nvs, "bedtime", &b) == ESP_OK) {
        s_bedtime = b;
    }
}

/* ---- Asking Santa (Muse) ---- */

#if CONFIG_MUSE_NPSC_SPOKEN
static bool ask_santa(const char *what)
{
    char name[NPSC_NAME_MAX + 1];
    muse_npsc_name(name, sizeof(name));
    char *p = malloc(1024);
    if (!p) {
        return false;
    }
    int n = snprintf(p, 1024,
                     "(From the North Pole Santa Communicator, a kid's bedside gadget; your reply is read "
                     "aloud.) Please answer as Santa Claus, talking to young children at bedtime: %s "
                     "Plain spoken words only: no title, lists, emoji or stage directions. Gentle, happy "
                     "and calm, nothing scary.",
                     what);
    if (name[0] && n > 0 && n < 900) {
        bool two = strstr(name, " and ") || strchr(name, '&') || strchr(name, ',');
        snprintf(p + n, 1024 - n, two ? " You're talking to %s together; use their names once or twice."
                                      : " The child's name is %s; use it once or twice.", name);
    }
    muse_voice_clear_stop();
    return muse_hatch_spoken_turn(p);   /* takes p */
}
#endif

#if CONFIG_MUSE_NPSC_STORY
/* Two children, by the way their name is written: "A and B", "A & B", "A, B". */
static bool two_kids(const char *name)
{
    return strstr(name, " and ") || strchr(name, '&') || strchr(name, ',');
}

/*
 * The story's text with its tokens filled in: {name} the kid's name, or
 * "my friend" with none set; {You} "You two" for two children, else "You".
 */
static void fill_story(const char *in, const char *name, char *out, size_t cap)
{
    size_t n = 0;
    while (*in && n + 1 < cap) {
        const char *with = NULL;
        size_t skip = 0;
        if (!strncmp(in, "{name}", 6)) {
            with = name[0] ? name : "my friend";
            skip = 6;
        } else if (!strncmp(in, "{You}", 5)) {
            with = two_kids(name) ? "You two" : "You";
            skip = 5;
        }
        if (with) {
            n += strlcpy(out + n, with, cap - n);
            if (n >= cap) {
                n = cap - 1;
            }
            in += skip;
        } else {
            out[n++] = *in++;
        }
    }
    out[n] = '\0';
}

/*
 * A shuffle bag: each story once, in random order, before any comes round
 * again, and never the same one twice running. Kept in NVS ("story_bag", a
 * bit per story told; "story_last"), so it carries on across restarts.
 */
static int pick_story(void)
{
    uint32_t all = (1u << NPSC_STORY_COUNT) - 1, told = 0;
    uint8_t last = 0xff;
    if (s_nvs) {
        nvs_get_u32(s_nvs, "story_bag", &told);
        nvs_get_u8(s_nvs, "story_last", &last);
    }
    told &= all;
    if (told == all) {
        told = 0;   /* a new round */
    }
    /* A round's first pick isn't the last round's last story. */
    uint32_t skip = told | (!told && last < NPSC_STORY_COUNT ? 1u << last : 0);
    int left = 0;
    for (int i = 0; i < (int)NPSC_STORY_COUNT; i++) {
        left += !(skip & (1u << i));
    }
    int k = (int)(esp_random() % (uint32_t)left), pick = 0;
    for (int i = 0; i < (int)NPSC_STORY_COUNT; i++) {
        if (!(skip & (1u << i)) && k-- == 0) {
            pick = i;
            break;
        }
    }
    told |= 1u << pick;
    if (s_nvs) {
        nvs_set_u32(s_nvs, "story_bag", told);
        nvs_set_u8(s_nvs, "story_last", (uint8_t)pick);
        nvs_commit(s_nvs);
    }
    return pick;
}
#endif

void muse_npsc_story_start(void)
{
#if CONFIG_MUSE_NPSC_STORY
    if (s_story != STORY_OFF) {
        return;
    }
    if (!muse_wifi_connected()) {
        muse_state_set_caption("STORIES NEED WI-FI");
        return;
    }
    char *p = malloc(1024);
    if (!p) {
        return;
    }
    char name[NPSC_NAME_MAX + 1];
    muse_npsc_name(name, sizeof(name));
    int i = pick_story();
    fill_story(NPSC_STORIES[i].text, name, p, 1024);
    ESP_LOGI(TAG, "story time: %s", NPSC_STORIES[i].title);
    muse_voice_clear_stop();
    if (muse_hatch_say(p)) {   /* takes p */
        s_story_at = now_us();
        s_story = STORY_WAIT;
    }
#endif
}

bool muse_npsc_story_active(void)
{
    return s_story != STORY_OFF;
}

void muse_npsc_jingle(void)
{
#if CONFIG_MUSE_NPSC_JINGLE
    ESP_LOGI(TAG, "jingle");
    s_jingle_until = now_us() + JINGLE_SHOW_US;
    muse_voice_request_jingle();
#endif
}

bool muse_npsc_press(void)
{
    bool used = false;
    bool asleep = muse_state_asleep();
    s_greet_until = s_splash_until = 0;
#if CONFIG_MUSE_NPSC_WAKE_RECORD
    if (s_wr_wanted || s_wr_active) {
        ESP_LOGI(TAG, "wake record: cancelled");
        s_wr_cancel = true;
        s_wr_wanted = false;
        muse_voice_stop_reply();
        used = true;
    }
#endif
    if (s_story != STORY_OFF) {
        ESP_LOGI(TAG, "story stopped");
        s_story = STORY_OFF;
        muse_voice_stop_reply();
        used = true;
    }
    if (s_elf) {
        s_elf = false;
        s_goodnight_at = 0;
        muse_voice_stop_reply();
        used = used || !asleep;
    }
    return used;
}

int muse_npsc_dim_pct(void)
{
    return s_elf ? ELF_DIM_PCT : -1;
}

/* ---- Scenes ---- */

void muse_npsc_scene(npsc_scene_t *s)
{
    int64_t now = now_us();
    muse_mode_t mode = muse_state_mode(NULL);
    s->id = NPSC_SCENE_IDLE;
    s->sleeps = -1;
    s->place = NULL;
    s->name[0] = '\0';
    s->rec_round = 0;
    s->rec_total = 0;
    s->rec_saved = 0;
    s->rec_phase = NPSC_WR_IDLE;
    int sleeps = s_synced ? s_sleeps : -1;
#if CONFIG_MUSE_NPSC_COUNTDOWN
    if (sleeps >= 0) {
        s->id = NPSC_SCENE_UNDER;
        s->sleeps = sleeps;
    }
#endif
#if CONFIG_MUSE_NPSC_WAKE_RECORD
    if (s_wr_wanted || s_wr_active) {
        s->id = NPSC_SCENE_WAKE_RECORD;
        s->rec_round = s_wr_round;
        s->rec_total = NPSC_WAKE_RECORD_ROUNDS;
        s->rec_saved = s_wr_saved;
        s->rec_phase = s_wr_phase;
        return;
    }
#endif
    if (muse_voice_recording()) {
        return;   /* the plain bar, for the green COMMUNICATE box */
    }
#if CONFIG_MUSE_NPSC_TRACKER
    const char *place = s_place;
    if (s_synced && place) {
        s->id = NPSC_SCENE_TRACKER;
        s->place = place;
    }
#endif
    if (s_elf) {
        s->id = NPSC_SCENE_ELF;
    } else if (s_story != STORY_OFF) {
        s->id = NPSC_SCENE_STORY;
    } else if (now < s_jingle_until) {
        s->id = NPSC_SCENE_JINGLE;
    } else if (mode == MUSE_MODE_IDLE && now < s_greet_until) {
        s->id = NPSC_SCENE_GREETING;
        muse_npsc_name(s->name, sizeof(s->name));
        if (!s->name[0]) {
            s->id = NPSC_SCENE_IDLE;
        }
    } else if (mode == MUSE_MODE_IDLE && now >= s_splash_from && now < s_splash_until && sleeps > 0) {
        s->id = NPSC_SCENE_WAKE;
        s->sleeps = sleeps;
    }
}

/* ---- The task ---- */

static int sleeps_to_christmas(const struct tm *t)
{
    if (t->tm_mon == 11 && t->tm_mday == 25) {
        return 0;
    }
    struct tm a = { .tm_year = t->tm_year, .tm_mon = t->tm_mon, .tm_mday = t->tm_mday, .tm_hour = 12, .tm_isdst = -1 };
    struct tm b = { .tm_year = t->tm_year + (t->tm_mon == 11 && t->tm_mday > 25), .tm_mon = 11, .tm_mday = 25,
                    .tm_hour = 12, .tm_isdst = -1 };
    return (int)((mktime(&b) - mktime(&a) + 43200) / 86400);
}

static void on_wake(void)
{
    int64_t now = now_us();
    char name[2];
    muse_npsc_name(name, sizeof(name));
    s_splash_from = now;
#if CONFIG_MUSE_NPSC_PARENT
    if (name[0]) {
        s_greet_until = now + WAKE_GREET_US;
        s_splash_from = s_greet_until;
    }
#endif
#if CONFIG_MUSE_NPSC_WAKE_SPLASH
    s_splash_until = s_splash_from + WAKE_SPLASH_US;
#endif
}

static void elf_start(void)
{
#if CONFIG_MUSE_NPSC_ELF
    ESP_LOGI(TAG, "bedtime: elf on duty");
    s_elf = true;
    muse_state_set_asleep(false);
    muse_state_poke();
    s_goodnight_at = now_us() + GOODNIGHT_DELAY_US;
#endif
}

static void goodnight(void)
{
    s_goodnight_at = 0;
#if CONFIG_MUSE_NPSC_SPOKEN && CONFIG_MUSE_NPSC_ELF
    if (muse_hatch_ready() && muse_state_mode(NULL) == MUSE_MODE_IDLE
        && ask_santa("say a short, warm goodnight in two or three sentences (under 40 words).")) {
        return;
    }
#endif
#if CONFIG_MUSE_NPSC_JINGLE
    muse_voice_request_jingle();   /* no Muse: the bells say goodnight */
#endif
}

static void npsc_task(void *arg)
{
    bool was_asleep = true, sntp_on = false;
    int last_min = -1;
    int64_t next_clock = 0;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(POLL_MS));
        int64_t now = now_us();
        bool asleep = muse_state_asleep();
        if (was_asleep && !asleep) {
            on_wake();
        }
        was_asleep = asleep;

#if CONFIG_MUSE_NPSC_CLOCK
        if (!sntp_on && muse_wifi_connected()) {
            esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
            cfg.start = true;
            sntp_on = esp_netif_sntp_init(&cfg) == ESP_OK;
            ESP_LOGI(TAG, "SNTP %s", sntp_on ? "started" : "failed");
        }
#else
        (void)sntp_on;
#endif
        if (now >= next_clock) {
            next_clock = now + 1000000;
            time_t t = time(NULL);
            if (t >= VALID_EPOCH) {
                struct tm tm;
                localtime_r(&t, &tm);
                if (!s_synced) {
                    ESP_LOGI(TAG, "clock set: %04d-%02d-%02d %02d:%02d local", tm.tm_year + 1900, tm.tm_mon + 1,
                             tm.tm_mday, tm.tm_hour, tm.tm_min);
                }
                s_sleeps = sleeps_to_christmas(&tm);
                int m = tm.tm_hour * 60 + tm.tm_min;
                s_place = tm.tm_mon == 11 && tm.tm_mday == 24 && m >= 17 * 60 ? ROUTE[(m - 17 * 60) / 30 % ROUTE_N]
                                                                               : NULL;
                s_synced = true;
                int b = s_bedtime;
                if (b >= 0 && last_min >= 0 && last_min < b && m >= b) {
                    elf_start();
                }
                last_min = m;
            }
        }
        if (s_goodnight_at && now >= s_goodnight_at) {
            goodnight();
        }

        /* A story lasts until its speech (or error) has played. */
        float secs;
        muse_mode_t mode = muse_state_mode(&secs);
        if (s_story == STORY_WAIT) {
            float waited = (now - s_story_at) / 1e6f;
            if (mode != MUSE_MODE_IDLE) {
                s_story = STORY_PLAY;
            } else if (secs < waited - 0.2f || now - s_story_at > STORY_WAIT_US) {
                s_story = STORY_OFF;   /* came and went between polls, or never came */
            }
        }
        if (s_story == STORY_PLAY && (mode == MUSE_MODE_IDLE || mode == MUSE_MODE_LISTENING)) {
            s_story = STORY_OFF;
        }
    }
}

void muse_npsc_start(void)
{
    load_settings();
#if CONFIG_MUSE_NPSC_CLOCK
    setenv("TZ", "PST8PDT,M3.2.0,M11.1.0", 1);   /* America/Los_Angeles */
    tzset();
#endif
    xTaskCreate(npsc_task, "npsc", 4096, NULL, 3, NULL);
}

#if CONFIG_MUSE_NPSC_WAKE_RECORD
void muse_npsc_wake_record_start(void)
{
    if (s_wr_wanted || s_wr_active) {
        return;
    }
    if (s_story != STORY_OFF || muse_voice_recording() || muse_state_mode(NULL) != MUSE_MODE_IDLE) {
        return;
    }
    if (!muse_wifi_connected()) {
        muse_state_set_caption("RECORDING NEEDS WI-FI");
        return;
    }
    s_wr_cancel = false;
    s_wr_phase = NPSC_WR_INTRO;
    s_wr_round = 0;
    s_wr_saved = 0;
    s_wr_wanted = true;
    ESP_LOGI(TAG, "wake record: requested");
}

bool muse_npsc_wake_record_wanted(void)
{
    return s_wr_wanted && !s_wr_active;
}

bool muse_npsc_wake_record_active(void)
{
    return s_wr_active;
}

void muse_npsc_wake_record_begin(void)
{
    s_wr_wanted = false;
    s_wr_active = true;
    s_wr_cancel = false;
    s_wr_saved = 0;
    s_wr_round = 0;
    s_wr_phase = NPSC_WR_INTRO;
}

void muse_npsc_wake_record_cancel(void)
{
    if (s_wr_wanted || s_wr_active) {
        s_wr_cancel = true;
        s_wr_wanted = false;
        muse_voice_stop_reply();
    }
}

bool muse_npsc_wake_record_cancelled(void)
{
    return s_wr_cancel;
}

void muse_npsc_wake_record_set(int round, npsc_wr_phase_t phase, int saved)
{
    s_wr_round = round;
    s_wr_phase = phase;
    s_wr_saved = saved;
}

bool muse_npsc_wake_record_save(const int16_t *pcm, size_t frames)
{
    if (!pcm || frames < MUSE_AUDIO_RATE / 10) {   /* shorter than 100 ms: not a clip */
        return false;
    }
    portENTER_CRITICAL(&s_lock);
    int i = s_wr_stored;
    if (i >= NPSC_WAKE_RECORD_MAX_CLIPS) {
        portEXIT_CRITICAL(&s_lock);
        ESP_LOGW(TAG, "wake record: PSRAM clip list full (%d)", NPSC_WAKE_RECORD_MAX_CLIPS);
        return false;
    }
    s_wr_stored = i + 1;   /* reserve the slot while we allocate */
    portEXIT_CRITICAL(&s_lock);
    int16_t *copy = heap_caps_malloc(frames * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!copy) {
        portENTER_CRITICAL(&s_lock);
        if (s_wr_stored == i + 1) {
            s_wr_stored = i;
        }
        portEXIT_CRITICAL(&s_lock);
        ESP_LOGW(TAG, "wake record: no PSRAM for a %.2fs clip", (double)frames / MUSE_AUDIO_RATE);
        return false;
    }
    memcpy(copy, pcm, frames * sizeof(int16_t));
    portENTER_CRITICAL(&s_lock);
    s_wr_clips[i].pcm = copy;
    s_wr_clips[i].frames = frames;
    s_wr_saved++;
    portEXIT_CRITICAL(&s_lock);
    return true;
}

int muse_npsc_wake_record_session_saved(void)
{
    return s_wr_saved;
}

int muse_npsc_wake_record_stored(void)
{
    return s_wr_stored;
}

static void wr_le32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static void wr_le16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void wr_wav_header(uint8_t h[44], size_t frames)
{
    uint32_t data = (uint32_t)(frames * 2);
    memset(h, 0, 44);
    memcpy(h, "RIFF", 4);
    wr_le32(h + 4, 36 + data);
    memcpy(h + 8, "WAVEfmt ", 8);
    wr_le32(h + 16, 16);
    wr_le16(h + 20, 1);
    wr_le16(h + 22, 1);
    wr_le32(h + 24, MUSE_AUDIO_RATE);
    wr_le32(h + 28, MUSE_AUDIO_RATE * 2);
    wr_le16(h + 32, 2);
    wr_le16(h + 34, 16);
    memcpy(h + 36, "data", 4);
    wr_le32(h + 40, data);
}

static const char WR_B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

typedef struct {
    uint8_t rem[3];
    int nrem;
    char line[77];
    int col;
} wr_b64_t;

static void wr_b64_init(wr_b64_t *s)
{
    s->nrem = 0;
    s->col = 0;
}

static void wr_b64_putc(wr_b64_t *s, char c)
{
    s->line[s->col++] = c;
    if (s->col == 76) {
        s->line[76] = '\0';
        printf("%s\n", s->line);
        s->col = 0;
    }
}

static void wr_b64_triple(wr_b64_t *s, unsigned a, unsigned b, unsigned c, int n)
{
    unsigned v = (a << 16) | (b << 8) | c;
    wr_b64_putc(s, WR_B64[(v >> 18) & 63]);
    wr_b64_putc(s, WR_B64[(v >> 12) & 63]);
    wr_b64_putc(s, n >= 2 ? WR_B64[(v >> 6) & 63] : '=');
    wr_b64_putc(s, n >= 3 ? WR_B64[v & 63] : '=');
}

static void wr_b64_feed(wr_b64_t *s, const uint8_t *src, size_t n)
{
    size_t i = 0;
    if (s->nrem && n) {
        while (s->nrem < 3 && i < n) {
            s->rem[s->nrem++] = src[i++];
        }
        if (s->nrem == 3) {
            wr_b64_triple(s, s->rem[0], s->rem[1], s->rem[2], 3);
            s->nrem = 0;
        }
    }
    while (i + 3 <= n) {
        wr_b64_triple(s, src[i], src[i + 1], src[i + 2], 3);
        i += 3;
    }
    while (i < n) {
        s->rem[s->nrem++] = src[i++];
    }
}

static void wr_b64_end(wr_b64_t *s)
{
    if (s->nrem) {
        wr_b64_triple(s, s->rem[0], s->nrem > 1 ? s->rem[1] : 0, 0, s->nrem);
        s->nrem = 0;
    }
    if (s->col) {
        s->line[s->col] = '\0';
        printf("%s\n", s->line);
        s->col = 0;
    }
}

void muse_npsc_wake_record_dump(void)
{
    int n;
    portENTER_CRITICAL(&s_lock);
    n = s_wr_stored;
    portEXIT_CRITICAL(&s_lock);
    printf("@clips begin count=%d max=%d cap_s=%d rate=%d\n", n, NPSC_WAKE_RECORD_MAX_CLIPS,
           NPSC_WAKE_RECORD_CAP_S, MUSE_AUDIO_RATE);
    fflush(stdout);
    for (int i = 0; i < n; i++) {
        int16_t *pcm;
        size_t frames;
        portENTER_CRITICAL(&s_lock);
        pcm = s_wr_clips[i].pcm;
        frames = s_wr_clips[i].frames;
        portEXIT_CRITICAL(&s_lock);
        if (!pcm || !frames) {
            continue;
        }
        uint8_t hdr[44];
        wr_wav_header(hdr, frames);
        printf("@clip %d frames=%u ms=%u bytes=%u\n", i + 1, (unsigned)frames,
               (unsigned)(frames * 1000 / MUSE_AUDIO_RATE), (unsigned)(44 + frames * 2));
        wr_b64_t b64;
        wr_b64_init(&b64);
        wr_b64_feed(&b64, hdr, 44);
        wr_b64_feed(&b64, (const uint8_t *)pcm, frames * sizeof(int16_t));
        wr_b64_end(&b64);
        printf("@clip %d end\n", i + 1);
        fflush(stdout);
    }
    printf("@clips done count=%d\n", n);
    fflush(stdout);
}

void muse_npsc_wake_record_finish(void)
{
    s_wr_active = false;
    s_wr_wanted = false;
    s_wr_phase = NPSC_WR_IDLE;
    s_wr_round = 0;
}
#endif
