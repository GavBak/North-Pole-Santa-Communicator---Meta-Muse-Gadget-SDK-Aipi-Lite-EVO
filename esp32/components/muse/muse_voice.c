/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "muse_voice.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#include "muse_adpcm.h"
#include "muse_audio.h"
#include "muse_board.h"
#include "muse_chat.h"
#include "muse_input.h"
#include "muse_mem.h"
#include "muse_settings.h"
#include "muse_state.h"
#include "muse_wifi.h"
#if CONFIG_MUSE_WAKE_WORD
#include "muse_menu.h"
#include "muse_wake.h"
#endif
#if CONFIG_MUSE_WAKE_WORD || CONFIG_MUSE_NPSC_WAKE_RECORD
#include "muse_npsc.h"
#endif
#if CONFIG_MUSE_NPSC_JINGLE
#include "avatar/npsc_jingle.h"
#endif

static const char *TAG = "muse_voice";

#define MAX_SECS 15
#define TAIL_FRAMES (MUSE_AUDIO_RATE * 12 / 100)   /* capture lag + poll interval, stops before the release click */
#define MAX_FRAMES (MUSE_AUDIO_RATE * MAX_SECS)
#define MIN_HELD_FRAMES (MUSE_AUDIO_RATE * 3 / 10)   /* shorter presses are taps, not speech */
#if CONFIG_MUSE_BOARD_M5STACK_CARDPUTER_ADV
#define PRE_CHUNKS 6                                   /* 120 ms; a 320 ms upload burst stalls on the ADV */
#else
#define PRE_CHUNKS 16                                  /* 320 ms of audio kept from before the press */
#endif
#define SETTLE_CHUNKS 10   /* after Muse makes a sound, 200 ms of capture is its own tail */
#define REST_BACKSTOP_MS 60000
#if CONFIG_MUSE_WAKE_WORD
/* A turn "hey Santa" started has no release: it ends on silence once speech
 * has started, judged against the mic's noise floor (tracked while idle and
 * during the turn). */
#define HF_SPEECH_DB 10.0f        /* above the floor: speech */
#define HF_SPEECH_CHUNKS 3        /* 60 ms of speech in a row counts as speaking */
#define HF_QUIET_CHUNKS 50        /* 1.0 s without speech, once it started, ends the turn */
#define HF_GRACE_CHUNKS 10        /* the first 200 ms is the tail of "hey Santa" itself */
#define HF_START_FRAMES (MUSE_AUDIO_RATE * 5)   /* nothing said in 5 s: dropped */
#define HF_FOLLOW_FRAMES (MUSE_AUDIO_RATE * 3)  /* follow-up turns: 3 s of quiet ends the session */
#define HF_DISCARD_CHUNKS 15                    /* 300 ms of speaker tail, not pre-roll, not in the 3 s */
/*
 * The floor is a low quantile of the 20 ms levels: it steps down 0.5 dB on a
 * quieter chunk and up 0.03 dB (1.5 dB/s) otherwise, so it sits near the
 * quietest ~6% of the room noise. One odd chunk (a DMA glitch, a zeroed
 * buffer) moves it 0.5 dB, not all the way, and digital silence is ignored.
 */
#define HF_FLOOR_FALL_DB 0.5f
#define HF_FLOOR_RISE_DB 0.03f
#define HF_FLOOR_MIN_DB (-90.0f)  /* quieter than this is a zeroed buffer, not the room */
#endif
#define SNORE_STALE_MS 500    /* a snore asked for longer ago than this is dropped */
#define SNORE_AMP 1100.0f     /* peak; the chirp's is ~7000: about 16 dB quieter */

#if CONFIG_MUSE_HATCH
/* A note recorded while Hatch is out of reach is saved in PSRAM, and goes once it's back. */
#define HOLD_NOTES 1
#else
#define HOLD_NOTES 0   /* no PSRAM: a press needs Hatch */
#endif
#define HELD_MAX 4
#define HELD_TRIES 8                            /* then a saved note is dropped */
#define HELD_POLL_MS 5000                       /* resting with notes saved: check for Wi-Fi this often */
#define HELD_KEEP_US (30LL * 60 * 1000000)      /* muse_voice_notes_waiting() */
#define RETRY_MIN_US (15LL * 1000000)
#define RETRY_MAX_US (120LL * 1000000)
#define STALL_US (30LL * 1000000)               /* Hatch took none of a note this long: give up */
#define ACK_WAIT_US (30LL * 1000000)            /* asleep, waiting for the VM to have a note */

static QueueHandle_t s_queue;
static volatile bool s_monitor;
static volatile float s_monitor_db = -100.0f;
static volatile bool s_chirp;
static volatile bool s_loopback;
static volatile bool s_mp3test;
static volatile uint32_t s_snore_ms;   /* when a snore was asked for (ms since boot), 0 for none */

/*
 * Pre-roll: while idle the mic keeps running into this ring, so a recording
 * can start a little before the press. People start talking as they press,
 * and the power key reports the press late.
 */
typedef struct {
#if MUSE_LOW_MEM
    muse_adpcm_t start;   /* encoder state before this chunk, to decode it alone */
    uint8_t adpcm[MUSE_AUDIO_CHUNK / 2];
#else
    int16_t pcm[MUSE_AUDIO_CHUNK];
#endif
} pre_chunk_t;

static pre_chunk_t *s_pre;   /* PRE_CHUNKS, a ring */
static size_t s_pre_next, s_pre_fill;
#if MUSE_LOW_MEM
static muse_adpcm_t s_pre_enc;
#endif
static int16_t s_chunk[MUSE_AUDIO_CHUNK];
static int s_settle;
#if CONFIG_MUSE_WAKE_WORD
static bool s_wake_ok;         /* the model loaded */
static bool s_wake_heard;      /* idle_capture heard "hey Santa" */
static bool s_wake_fresh;      /* not listening last chunk: start the model over */
static float s_floor_db = -60.0f;
static bool s_convo;          /* a "hey santa" session: listen again after each reply */
static bool s_convo_listen;   /* the next loop pass is a follow-up record() */

static void track_floor(float *floor, float db)
{
    if (db < HF_FLOOR_MIN_DB) {
        return;
    }
    *floor += db < *floor ? -HF_FLOOR_FALL_DB : HF_FLOOR_RISE_DB;
}

/* Listen only when a press would start a plain turn: awake, idle, not in
 * the menu or a story. */
static bool wake_listening(void)
{
    if (!s_wake_ok || muse_state_asleep() || muse_state_mode(NULL) != MUSE_MODE_IDLE || muse_menu_is_open()) {
        return false;
    }
#if CONFIG_MUSE_NPSC_STORY
    if (muse_npsc_story_active()) {
        return false;
    }
#endif
#if CONFIG_MUSE_NPSC_WAKE_RECORD
    if (muse_npsc_wake_record_active() || muse_npsc_wake_record_wanted()) {
        return false;
    }
#endif
    return true;
}
#endif

/*
 * The note being recorded (HOLD_NOTES), kept whole in case it can't go now.
 * Streaming, Hatch has had s_sent frames of it.
 */
static int16_t *s_rec;   /* MAX_FRAMES */
static size_t s_rec_n, s_sent;
static bool s_live = true;
static bool s_tried;     /* it went to Hatch */

typedef struct {
    int16_t *pcm;
    size_t frames;
    int64_t at_us;   /* when it was recorded */
    int tries;
} held_note_t;

static int s_held_count;
static volatile bool s_waiting;        /* muse_voice_notes_waiting() */
#if HOLD_NOTES
static held_note_t s_held[HELD_MAX];   /* oldest first */
static int64_t s_next_send_us;
static int64_t s_send_backoff_us = RETRY_MIN_US;
#endif

/* The i-th oldest pre-roll chunk, into out. */
static void pre_get(size_t i, int16_t *out)
{
    const pre_chunk_t *c = &s_pre[(s_pre_next + PRE_CHUNKS - s_pre_fill + i) % PRE_CHUNKS];
#if MUSE_LOW_MEM
    muse_adpcm_t dec = c->start;
    muse_adpcm_decode_block(&dec, c->adpcm, MUSE_AUDIO_CHUNK, out);
#else
    memcpy(out, c->pcm, sizeof(c->pcm));
#endif
}

/* Call after Muse plays anything: its own sound must not become pre-roll. */
static void pre_reset(void)
{
    s_pre_fill = 0;
    s_settle = SETTLE_CHUNKS;
#if CONFIG_MUSE_WAKE_WORD
    s_wake_heard = false;
    s_wake_fresh = true;   /* Muse's own sound mustn't count toward the wake word */
#endif
}

/* One 20 ms idle read: feeds the pre-roll ring and the settings mic meter. */
static void idle_capture(void)
{
    if (muse_audio_read(s_chunk, MUSE_AUDIO_CHUNK) != ESP_OK) {
        vTaskDelay(pdMS_TO_TICKS(20));
        return;
    }
    if (s_monitor) {
        float db = muse_audio_dbfs(s_chunk, MUSE_AUDIO_CHUNK);
        s_monitor_db = db > s_monitor_db ? db : s_monitor_db * 0.9f + db * 0.1f;
    }
    if (s_settle > 0) {
        s_settle--;
        return;
    }
#if CONFIG_MUSE_WAKE_WORD
    if (wake_listening()) {
        if (s_wake_fresh) {
            s_wake_fresh = false;
            muse_wake_reset();
        }
        track_floor(&s_floor_db, muse_audio_dbfs(s_chunk, MUSE_AUDIO_CHUNK));
        s_wake_heard |= muse_wake_feed(s_chunk, MUSE_AUDIO_CHUNK);
    } else {
        s_wake_fresh = true;
        s_wake_heard = false;
    }
#endif
    pre_chunk_t *c = &s_pre[s_pre_next];
#if MUSE_LOW_MEM
    c->start = s_pre_enc;
    muse_adpcm_encode_block(&s_pre_enc, s_chunk, MUSE_AUDIO_CHUNK, c->adpcm);
#else
    memcpy(c->pcm, s_chunk, sizeof(c->pcm));
#endif
    s_pre_next = (s_pre_next + 1) % PRE_CHUNKS;
    s_pre_fill = s_pre_fill < PRE_CHUNKS ? s_pre_fill + 1 : PRE_CHUNKS;
}

/* Non-blocking: returns true if an event of `type` arrived (others dropped). */
static bool got_event(muse_ptt_t type)
{
    muse_input_event_t ev;
    bool hit = false;
    while (xQueueReceive(s_queue, &ev, 0) == pdTRUE) {
        muse_state_poke();
        hit |= ev.type == type;
    }
    return hit;
}

/* Drain queued talk events. *down / *up (if given) are set if that edge arrived. */
static void drain_talk(bool *down, bool *up)
{
    muse_input_event_t ev;
    if (down) {
        *down = false;
    }
    if (up) {
        *up = false;
    }
    while (xQueueReceive(s_queue, &ev, 0) == pdTRUE) {
        muse_state_poke();
        if (down && ev.type == MUSE_PTT_DOWN) {
            *down = true;
        }
        if (up && ev.type == MUSE_PTT_UP) {
            *up = true;
        }
    }
}

/* Level stats of a recording, gathered chunk by chunk for the log. */
typedef struct {
    double acc;
    size_t frames;
    int peak, clipped;
    float floor_db;
    float tail_db[25];   /* the last 0.5 s, to see where speech ends and the release click lands */
    size_t chunks;
} rec_stats_t;

/* Adds one captured chunk (up to MUSE_AUDIO_CHUNK frames) to the stats. */
static void keep(rec_stats_t *st, const int16_t *pcm, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        int v = abs(pcm[i]);
        st->peak = v > st->peak ? v : st->peak;
        st->clipped += v >= 32000;
        st->acc += (double)pcm[i] * pcm[i];
    }
    st->frames += n;
    if (n == MUSE_AUDIO_CHUNK) {
        float db = muse_audio_dbfs(pcm, n);
        st->floor_db = db < st->floor_db ? db : st->floor_db;
        st->tail_db[st->chunks++ % 25] = db;
    }
}

/* Gives Hatch what it has room for of the note being recorded. */
static void feed_live(void)
{
#if HOLD_NOTES
    if (s_live && s_sent < s_rec_n) {
        s_sent += muse_hatch_turn_audio_wait(s_rec + s_sent, s_rec_n - s_sent, 0);
    }
#endif
}

/* Streams the note being recorded from its start, then the rest as it comes. */
static void go_live(void)
{
    muse_hatch_turn_begin();
    s_live = s_tried = true;
    s_sent = 0;
    feed_live();
}

/* One chunk of speech: into the stats, and on to Hatch, by way of the kept note if there is one. */
static void take(rec_stats_t *st, const int16_t *pcm)
{
    keep(st, pcm, MUSE_AUDIO_CHUNK);
    if (!s_rec) {
        if (s_live) {
            muse_hatch_turn_audio(pcm, MUSE_AUDIO_CHUNK);
        }
        return;
    }
    memcpy(s_rec + s_rec_n, pcm, MUSE_AUDIO_CHUNK * sizeof(int16_t));
    s_rec_n += MUSE_AUDIO_CHUNK;
    feed_live();
}

/*
 * Records speech until release or MAX_SECS, starting with the pre-roll; *held
 * gets the part after the press. It streams to Hatch as it goes if it can.
 * If not, the note is kept in s_rec to send later, and streams from partway
 * if Hatch comes within reach. Returns false if Hatch failed the turn with no
 * kept note to fall back on (why says what failed). There is no start chirp:
 * anything played now would land on top of the first words.
 */
static volatile uint32_t s_rec_starts, s_rec_ends;   /* record() calls begun and finished */
static volatile int64_t s_stop_us;     /* muse_voice_stop_reply(), 0 for none */
static volatile bool s_jingle;
#define STOP_KEEP_US 70000000LL

bool muse_voice_recording(void)
{
    return s_rec_starts != s_rec_ends;
}

uint32_t muse_voice_recordings_started(void)
{
    return s_rec_starts;
}

static bool record(bool barge_in, bool hands_free, bool *spoke, size_t *held, char *why, size_t cap,
                   size_t start_frames, bool follow_up, bool *cancelled, bool local, size_t cap_frames,
                   size_t *clip_from)
{
    *spoke = !hands_free;
    if (cancelled) {
        *cancelled = false;
    }
    if (clip_from) {
        *clip_from = 0;
    }
#if !CONFIG_MUSE_WAKE_WORD
    (void)start_frames;
    (void)follow_up;
#endif
    if (!local) {
        (void)cap_frames;
    }
    s_stop_us = 0;
    s_rec_starts++;
    muse_state_set_mode(MUSE_MODE_LISTENING);
    muse_state_set_progress(0);
    s_rec_n = s_sent = 0;
    s_live = s_tried = false;
    if (!local && (!s_rec || (muse_hatch_ready() && !s_held_count))) {
        go_live();
    }
    if (!local) {
        muse_state_set_caption(s_live ? "LISTENING..." : "RECORDING...");
    }
    bool heard = false, ok = true;
    bool gave_up = false;   /* Hatch failed this note: it's kept, and goes later */
    char text[96];
    rec_stats_t st = { 0 };

    size_t n = 0;
    if (barge_in) {
        /* Pressed during playback: the queued tail of the reply is still sounding. */
        for (int i = 0; i < SETTLE_CHUNKS; i++) {
            muse_audio_read(s_chunk, MUSE_AUDIO_CHUNK);
        }
    } else if (!follow_up) {
        for (size_t i = 0; i < s_pre_fill; i++) {
            pre_get(i, s_chunk);
            take(&st, s_chunk);
            n += MUSE_AUDIO_CHUNK;
        }
    }
    size_t pre = n;
    bool released = false;
    size_t stop_at = MAX_FRAMES;
#if CONFIG_MUSE_WAKE_WORD
    float floor_db = s_floor_db;
    int loud = 0, quiet = 0;
    bool cancel = false;
    const char *cancel_why = NULL;
    if (!start_frames) {
        start_frames = HF_START_FRAMES;
    }
#endif
    while (n + MUSE_AUDIO_CHUNK <= stop_at) {
        if (muse_audio_read(s_chunk, MUSE_AUDIO_CHUNK) != ESP_OK) {
            break;
        }
        muse_state_set_level(muse_audio_level(s_chunk, MUSE_AUDIO_CHUNK));
        take(&st, s_chunk);
        /* Live transcript as the caption. A failure stops the streaming; the
         * kept note goes later, or without one the failure is the caption. */
        muse_hatch_ev_t ev;
        while (!local && s_live && (ev = muse_hatch_turn_event(text, sizeof(text))) != MUSE_HATCH_EV_NONE) {
            if (ev == MUSE_HATCH_EV_HEARD && text[0]) {
                heard = true;
                muse_state_set_caption("%s", text);
            } else if (ev == MUSE_HATCH_EV_ERROR) {
                ESP_LOGW(TAG, "muse: %s", text);
                muse_hatch_turn_cancel();
                s_live = false;
                gave_up = true;
                if (!s_rec) {
                    ok = false;
                    strlcpy(why, text, cap);
                    muse_state_set_caption("%s", text);
                }
            }
        }
        n += MUSE_AUDIO_CHUNK;
        bool tick = n % (MUSE_AUDIO_CHUNK * 5) == 0;
        if (!local && tick && s_rec && !s_live && !gave_up && !s_held_count && muse_hatch_ready()) {
            ESP_LOGI(TAG, "Muse in reach: streaming the note so far");
            go_live();
        }
        muse_state_set_progress((float)n / MAX_FRAMES);
        if (!local && !heard && ok && tick) {
            muse_state_set_caption("%s %.1fs", s_live ? "LISTENING" : "RECORDING", (double)n / MUSE_AUDIO_RATE);
        }
        /*
         * Capture runs 60-80 ms behind real time and people let go on their
         * last syllable, so keep going briefly after release.
         */
        if (!released) {
            bool down = false, up = false;
            drain_talk(&down, &up);
#if CONFIG_MUSE_WAKE_WORD
            /* A talk press in the quiet window, before speech, ends the
             * session and must not send a near-empty clip. After speech
             * has started, a click still ends and sends, as before. */
            if (hands_free && (down || up) && (local || !*spoke)) {
                cancel = true;
                cancel_why = "talk press";
                released = true;
                stop_at = n;
            } else
#endif
            if (up) {
                released = true;
                *spoke = true;   /* the talk button ended it: send what there is */
                stop_at = n + TAIL_FRAMES < MAX_FRAMES ? n + TAIL_FRAMES : MAX_FRAMES;
            }
        }
#if CONFIG_MUSE_WAKE_WORD
        if (hands_free && !released) {
            if (muse_menu_is_open()) {
                cancel = true;
                cancel_why = "menu";
                released = true;
                stop_at = n;
            } else {
                int64_t stop = s_stop_us;
                if (stop && esp_timer_get_time() - stop < STOP_KEEP_US) {
                    s_stop_us = 0;
                    cancel = true;
                    cancel_why = "stop";
                    released = true;
                    stop_at = n;
                }
            }
        }
        if (hands_free && !released) {
            float db = muse_audio_dbfs(s_chunk, MUSE_AUDIO_CHUNK);
            /* Follow-up turns have already discarded the speaker tail; the
             * first 200 ms is only the tail of "hey Santa" itself. */
            bool grace = !follow_up && n - pre <= HF_GRACE_CHUNKS * MUSE_AUDIO_CHUNK;
            /*
             * Only a run of speech resets the quiet count: anything else,
             * room noise a little over the floor or a lone click, counts as
             * quiet, so the turn ends 1 s after the last word.
             */
            loud = !grace && db > floor_db + HF_SPEECH_DB ? loud + 1 : 0;
            if (loud >= HF_SPEECH_CHUNKS) {
                if (!*spoke) {
                    /* Keep 200 ms before the first speech so the clip isn't clipped. */
                    size_t pre_roll = MUSE_AUDIO_RATE / 5;
                    size_t from = n > pre + pre_roll ? n - pre_roll : pre;
                    if (clip_from) {
                        *clip_from = from;
                    }
                    if (local && cap_frames) {
                        size_t cap_at = from + cap_frames;
                        if (cap_at < stop_at) {
                            stop_at = cap_at;
                        }
                    }
                }
                *spoke = true;
                quiet = 0;
            } else {
                quiet++;
            }
            track_floor(&floor_db, db);
            if (!local) {
                *spoke |= heard;   /* Muse transcribed something */
            }
            if ((*spoke && quiet >= HF_QUIET_CHUNKS) || (!*spoke && n - pre >= start_frames)) {
                ESP_LOGI(TAG, "\"hey santa\" turn: %s after %.2fs, floor %.1f dBFS",
                         *spoke ? "1 s quiet" : "nothing said", (double)(n - pre) / MUSE_AUDIO_RATE,
                         (double)floor_db);
                released = true;
                stop_at = n;
            }
        }
#endif
    }
    muse_state_set_level(0);
    *held = n - pre;
#if CONFIG_MUSE_WAKE_WORD
    if (cancel) {
        *spoke = false;
        if (cancelled) {
            *cancelled = true;
        }
        if (why && cancel_why) {
            strlcpy(why, cancel_why, cap);
        }
    }
#endif

    char tail[160];
    int tl = 0;
    size_t shown = st.chunks < 25 ? st.chunks : 25;
    for (size_t i = st.chunks - shown; i < st.chunks; i++) {
        tl += snprintf(tail + tl, sizeof(tail) - tl, " %.0f", st.tail_db[i % 25]);
    }
    ESP_LOGI(TAG, "end levels:%s", tail);
    float rms_db = st.frames ? 10.0f * log10f((float)(st.acc / st.frames / (32768.0 * 32768.0)) + 1e-10f) : -100.0f;
    ESP_LOGI(TAG, "recorded %.2fs%s: rms %.1f dBFS, peak %.1f dBFS, floor %.1f dBFS, %d clipped, gain %d dB",
             (double)n / MUSE_AUDIO_RATE, released ? "" : " (max)", rms_db < -100.0f ? -100.0f : rms_db,
             20.0 * log10((st.peak + 1) / 32768.0), st.floor_db, st.clipped, muse_settings_mic_gain());
    s_rec_ends++;
    return ok;
}

static void go_idle(const char *caption);

/*
 * Plays Hatch's reply as it arrives, with its text as the caption. Returns
 * true if interrupted by a new press. *delivered: the VM has the note.
 * `waiting` is the caption until there's a transcript or reply.
 */
static bool hatch_reply_as(bool *delivered, const char *waiting)
{
    muse_state_set_mode(MUSE_MODE_THINKING);
    muse_state_set_caption("%s", waiting);
    static int16_t buf[MUSE_AUDIO_CHUNK];
    static const int16_t silence[MUSE_AUDIO_CHUNK];
    char text[96];
    static char page[MUSE_CAPTION_MAX];
    bool done = false, speaking = false, replied = false;
    size_t played = 0;
    int64_t t0 = esp_timer_get_time();
    *delivered = false;
    for (;;) {
        muse_hatch_ev_t ev;
        while ((ev = muse_hatch_turn_event(text, sizeof(text))) != MUSE_HATCH_EV_NONE) {
            switch (ev) {
            case MUSE_HATCH_EV_HEARD:
                if (!speaking && !replied) {
                    muse_state_set_caption("\"%s\"", text);
                }
                break;
            case MUSE_HATCH_EV_SENT:
                *delivered = true;
                if (!speaking && !replied) {
                    muse_state_set_caption("NOTE SENT - WAITING FOR MUSE");
                }
                break;
            case MUSE_HATCH_EV_REPLY:
                replied = *delivered = true;
                /* Once speech starts, the caption follows it. The event only
                 * has room for the page's start; the page itself comes below. */
                if (!speaking && !muse_hatch_turn_caption(played, page, sizeof(page))) {
                    muse_state_set_caption("%s", text);
                }
                break;
            case MUSE_HATCH_EV_DONE:
                done = *delivered = true;
                break;
            case MUSE_HATCH_EV_ERROR:
                ESP_LOGW(TAG, "muse: %s", text);
                muse_state_set_level(0);
                go_idle(text);
                return false;
            default:
                break;
            }
        }
        int64_t stop = s_stop_us;
        if (stop && esp_timer_get_time() - stop < STOP_KEEP_US) {
            s_stop_us = 0;
            ESP_LOGI(TAG, "reply stopped");
            muse_hatch_turn_cancel();
            muse_state_set_level(0);
            go_idle("");
            return false;
        }
#if CONFIG_MUSE_WAKE_WORD
        if (muse_menu_is_open()) {
            ESP_LOGI(TAG, "reply stopped: menu open");
            muse_hatch_turn_cancel();
            muse_state_set_level(0);
            go_idle("");
            return false;
        }
#endif
        if (got_event(MUSE_PTT_DOWN)) {
            ESP_LOGI(TAG, "reply interrupted");
            muse_hatch_turn_cancel();
            muse_state_set_level(0);
            return true;
        }
        size_t n = muse_hatch_turn_read(buf, MUSE_AUDIO_CHUNK, speaking || done ? 0 : 20);
        if (n) {
            if (!speaking) {
                speaking = true;
                muse_state_set_mode(MUSE_MODE_SPEAKING);
                ESP_LOGI(TAG, "reply audio after %.2fs", (esp_timer_get_time() - t0) / 1e6);
            }
            muse_state_set_level(muse_audio_level(buf, n));
            muse_audio_write(buf, n);
            played += n;
        } else if (done) {
            break;
        } else if (speaking) {
            /* Between messages: keep the speaker fed so it doesn't replay stale DMA. */
            muse_state_set_level(0);
            muse_audio_write(silence, MUSE_AUDIO_CHUNK);
        }
        /* The page being said, or before the speech the reply's opening page. */
        if ((speaking || replied) && muse_hatch_turn_caption(played, page, sizeof(page))) {
            muse_state_set_caption("%s", page);
        }
    }
    muse_state_set_level(0);
    ESP_LOGI(TAG, "muse reply: %.2fs of audio, %.2fs total", (double)played / MUSE_AUDIO_RATE,
             (esp_timer_get_time() - t0) / 1e6);
    if (!played) {
        /* No speech (TTS unavailable): leave the reply text up for a moment. */
        vTaskDelay(pdMS_TO_TICKS(2500));
    }
    return false;
}

static bool hatch_reply(bool *delivered)
{
    return hatch_reply_as(delivered, "SENDING VOICE NOTE");
}

/* A push from Muse: wake the screen and play it like a reply. True if a
 * press interrupted it, to be recorded as one. */
static bool play_push(void)
{
    ESP_LOGI(TAG, "playing a push from Muse");
    muse_state_set_asleep(false);
    muse_state_poke();
    muse_wifi_power(MUSE_WIFI_FULL);
    bool delivered;
    return hatch_reply_as(&delivered, "");
}

static void go_idle(const char *caption)
{
    muse_state_set_progress(0);
    muse_state_set_mode(MUSE_MODE_IDLE);
    muse_state_set_caption("%s", caption);
}

/* Why a press can't go to Hatch; voice notes only go there. */
static const char *not_ready_reason(void)
{
    muse_hatch_status_t st;
    muse_hatch_status(&st);
    switch (st.state) {
    case MUSE_HATCH_NOT_SET: return "SET UP MUSE FIRST";
    case MUSE_HATCH_OFFLINE: return "NO WI-FI";
    default: return "CAN'T REACH MUSE";
    }
}

static volatile bool s_resting;

/*
 * Asleep on battery with nothing to do: power the codecs down (the loop rests
 * Wi-Fi alongside). They come back within one wait on waking, and a talk press
 * that woke Muse is kept for the loop (a note may follow). On USB power
 * nothing changes asleep.
 */
static void set_resting(bool rest)
{
    if (rest == s_resting) {
        return;
    }
    s_resting = rest;
    muse_audio_power(!rest);
    muse_hatch_set_resting(rest);
    if (rest) {
        /* Presses from before sleeping are stale, but not one that just woke Muse. */
        muse_input_event_t ev;
        while (xQueueReceive(s_queue, &ev, 0) == pdTRUE) {
            if (ev.wake) {
                xQueueSendToFront(s_queue, &ev, 0);
                break;
            }
        }
    } else {
        pre_reset();
    }
    ESP_LOGI(TAG, "%s", rest ? "resting: codecs off, Wi-Fi modem sleep" : "awake: codecs on");
}

/*
 * A press that woke the screen: listen from now, into the pre-roll, and
 * record only if it's still held after a tap's length. False if it was let
 * go sooner: it only woke Muse.
 */
static bool held_on_waking(void)
{
    s_settle = 0;   /* nothing played: no tail of Muse's own to skip */
    for (size_t n = 0; n < MIN_HELD_FRAMES; n += MUSE_AUDIO_CHUNK) {
        if (got_event(MUSE_PTT_UP)) {
            return false;
        }
        idle_capture();
    }
    return !got_event(MUSE_PTT_UP);
}

static void drop_rec(void)
{
    free(s_rec);
    s_rec = NULL;
}

#if HOLD_NOTES
/* True if a press is queued, left there for the loop; releases ahead of it are dropped. */
static bool press_waiting(void)
{
    muse_input_event_t ev;
    while (xQueuePeek(s_queue, &ev, 0) == pdTRUE) {
        if (ev.type == MUSE_PTT_DOWN) {
            return true;
        }
        xQueueReceive(s_queue, &ev, 0);
    }
    return false;
}

static void back_off(void)
{
    s_next_send_us = esp_timer_get_time() + s_send_backoff_us;
    s_send_backoff_us = s_send_backoff_us * 2 > RETRY_MAX_US ? RETRY_MAX_US : s_send_backoff_us * 2;
}

/* Saves the note just recorded to send later. `tried`: Hatch failed it, so it waits out the backoff. */
static void hold_rec(bool tried)
{
    if (s_held_count >= HELD_MAX) {   /* can_record() leaves room: not expected */
        drop_rec();
        go_idle("COULDN'T SAVE THE NOTE");
        return;
    }
    if (!s_rec_n) {   /* nothing captured: realloc to 0 would free s_rec and return NULL */
        drop_rec();
        go_idle("HOLD LONGER TO TALK");
        return;
    }
    int16_t *pcm = heap_caps_realloc(s_rec, s_rec_n * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_held[s_held_count++] = (held_note_t){
        .pcm = pcm ? pcm : s_rec,
        .frames = s_rec_n,
        .at_us = esp_timer_get_time(),
        .tries = tried,
    };
    s_rec = NULL;
    if (tried) {
        back_off();
    }
    ESP_LOGI(TAG, "saved a %.1fs note to send later (%d waiting)", (double)s_rec_n / MUSE_AUDIO_RATE, s_held_count);
    go_idle(tried ? "SAVED, WILL TRY AGAIN" : "SAVED, SENDS WHEN ONLINE");
}

static void drop_oldest(void)
{
    free(s_held[0].pcm);
    s_held_count--;
    memmove(s_held, s_held + 1, s_held_count * sizeof(s_held[0]));
    if (!s_held_count) {
        s_next_send_us = 0;
        s_send_backoff_us = RETRY_MIN_US;
    }
}

typedef enum {
    FED,
    FEED_FAILED,    /* the turn failed, or Hatch stopped taking audio */
    FEED_PRESSED,   /* a press is queued: it goes first */
} feed_t;

/*
 * Gives Hatch a note from *sent on, waiting for room as it goes, then ends
 * the turn. Stops for a press only if `yield`.
 */
static feed_t feed_rest(const int16_t *pcm, size_t frames, size_t *sent, bool yield)
{
    char text[96];
    int64_t moved = esp_timer_get_time();
    for (;;) {
        muse_hatch_ev_t ev;
        while ((ev = muse_hatch_turn_event(text, sizeof(text))) != MUSE_HATCH_EV_NONE) {
            if (ev == MUSE_HATCH_EV_ERROR) {
                ESP_LOGW(TAG, "muse: %s", text);
                return FEED_FAILED;
            }
        }
        if (*sent == frames) {
            break;
        }
        if (yield && press_waiting()) {
            return FEED_PRESSED;
        }
        size_t n = muse_hatch_turn_audio_wait(pcm + *sent, frames - *sent, 100);
        *sent += n;
        int64_t now = esp_timer_get_time();
        if (n) {
            moved = now;
        } else if (now - moved > STALL_US) {
            ESP_LOGW(TAG, "Muse stopped taking the note");
            return FEED_FAILED;
        }
    }
    muse_hatch_turn_end();
    return FED;
}

/* Asleep: waits for the VM to have the note just sent. */
static bool wait_delivered(void)
{
    char text[96];
    int64_t give_up = esp_timer_get_time() + ACK_WAIT_US;
    while (esp_timer_get_time() < give_up) {
        muse_hatch_ev_t ev;
        while ((ev = muse_hatch_turn_event(text, sizeof(text))) != MUSE_HATCH_EV_NONE) {
            if (ev == MUSE_HATCH_EV_SENT || ev == MUSE_HATCH_EV_REPLY || ev == MUSE_HATCH_EV_DONE) {
                return true;
            }
            if (ev == MUSE_HATCH_EV_ERROR) {
                ESP_LOGW(TAG, "muse: %s", text);
                return false;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    ESP_LOGW(TAG, "no word that the note arrived");
    return false;
}

/*
 * Sends the oldest saved note. Awake, it's a turn like any other, reply and
 * all; asleep (quiet), it's done once the VM has the note, and the reply
 * waits in the app. A press while it goes up comes first, and the note stays
 * saved. Returns true if a press interrupted the reply.
 */
static bool send_held(bool quiet)
{
    held_note_t *h = &s_held[0];
    ESP_LOGI(TAG, "sending a saved note%s: %.1fs, from %llds ago, try %d", quiet ? " (asleep)" : "",
             (double)h->frames / MUSE_AUDIO_RATE, (long long)((esp_timer_get_time() - h->at_us) / 1000000),
             h->tries + 1);
    if (!quiet) {
        muse_state_set_mode(MUSE_MODE_THINKING);
        muse_state_set_caption("SENDING SAVED NOTE");
    }
    muse_hatch_turn_begin();
    size_t sent = 0;
    feed_t fed = feed_rest(h->pcm, h->frames, &sent, true);
    bool delivered = false, interrupted = false;
    if (fed == FED && quiet) {
        delivered = wait_delivered();
    } else if (fed == FED) {
        interrupted = hatch_reply(&delivered);
    }
    if (fed != FED || quiet) {
        muse_hatch_turn_cancel();   /* asleep, the reply is left for the app */
    }
    if (fed == FEED_PRESSED) {
        if (!quiet) {
            go_idle("");
        }
        return false;
    }
    const char *caption;
    if (delivered || interrupted) {
        drop_oldest();
        s_next_send_us = 0;   /* the next one right away */
        s_send_backoff_us = RETRY_MIN_US;
        if (quiet) {
            muse_state_set_caption("SAVED NOTE SENT");
        }
        return interrupted;
    } else if (++h->tries >= HELD_TRIES) {
        ESP_LOGW(TAG, "giving up on a saved note after %d tries", h->tries);
        drop_oldest();
        caption = "COULDN'T SEND A SAVED NOTE";
    } else {
        back_off();
        caption = "SAVED NOTE: WILL TRY AGAIN";
    }
    if (quiet) {
        muse_state_set_caption("%s", caption);
    } else {
        go_idle(caption);
    }
    return false;
}

/*
 * Whether to send a saved note now: Hatch within reach and the backoff over.
 * Coming within reach starts the backoff over. Keeps s_waiting too.
 */
static bool held_due(void)
{
    static bool was_ready = true;
    if (!s_held_count) {
        was_ready = true;
        s_waiting = false;
        return false;
    }
    int64_t now = esp_timer_get_time();
    s_waiting = now - s_held[0].at_us < HELD_KEEP_US;
    bool ready = muse_hatch_ready();
    if (ready && !was_ready) {
        s_next_send_us = 0;
        s_send_backoff_us = RETRY_MIN_US;
    }
    was_ready = ready;
    return ready && now >= s_next_send_us;
}
#endif

/*
 * After the release: the rest of the note goes to Hatch and the reply plays,
 * or if it can't go now it's saved to send later. Returns true if a press
 * interrupted the reply.
 */
static bool finish_note(void)
{
    bool delivered;
#if HOLD_NOTES
    if (s_rec) {
        bool fed = false, interrupted = false;
        delivered = false;
        if (s_live && s_sent == s_rec_n) {
            muse_hatch_turn_end();   /* before the chirp, which takes ~90 ms */
            fed = true;
        }
        muse_audio_chirp(0);
        if (s_live && !fed) {
            /* Hatch is behind (still connecting, say): the rest from the kept note. */
            muse_state_set_mode(MUSE_MODE_THINKING);
            muse_state_set_caption("SENDING VOICE NOTE");
            fed = feed_rest(s_rec, s_rec_n, &s_sent, false) == FED;
            if (!fed) {
                muse_hatch_turn_cancel();
            }
        }
        if (fed) {
            interrupted = hatch_reply(&delivered);
        }
        if (delivered || interrupted) {
            drop_rec();
        } else {
            hold_rec(s_tried);
        }
        return interrupted;
    }
#endif
    muse_hatch_turn_end();   /* before the chirp, which takes ~90 ms */
    muse_audio_chirp(0);
    return hatch_reply(&delivered);
}

/*
 * Whether a press can record: Hatch within reach, or (HOLD_NOTES) set up
 * with room to save the note. If not, the caption says why.
 */
static bool can_record(void)
{
    if (!muse_wifi_connected()) {
        muse_wifi_apply();   /* retry now, not after the backoff */
    }
    bool ready = muse_hatch_ready();
#if HOLD_NOTES
    muse_hatch_status_t st;
    muse_hatch_status(&st);
    if (st.state == MUSE_HATCH_NOT_SET) {
        go_idle("SET UP MUSE FIRST");
        return false;
    }
    if ((!ready || s_held_count) && s_held_count >= HELD_MAX) {
        go_idle("NOTES STILL WAITING TO SEND");
        return false;
    }
    if (ready && s_held_count) {
        s_next_send_us = 0;   /* the saved ones go first, right after */
    }
    s_rec = heap_caps_malloc(MAX_FRAMES * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_rec) {
        ESP_LOGE(TAG, "no memory to keep a note");
    }
#endif
    if (!ready && !s_rec) {
        go_idle(not_ready_reason());
        return false;
    }
    return true;
}

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

/*
 * One soft snore, about 0.8 s, made up as it plays: a fluttering rumble that
 * rises through the inhale (60-90 Hz, with harmonics a small speaker can
 * reproduce, over breathy low-passed noise), then a quieter exhale of air.
 * Stops early for a press or if drowsing is interrupted, so it never holds up
 * a recording.
 */
static void play_snore(void)
{
    enum { IN_FRAMES = MUSE_AUDIO_RATE * 55 / 100, OUT_FRAMES = MUSE_AUDIO_RATE / 4 };
    static int which;
    static uint32_t seed = 0x9E3779B9u;
    float base_hz = which++ & 1 ? 70.0f : 62.0f;   /* two in a row don't sound the same */
    float phase = 0, lp_in = 0, lp_out = 0;
    int16_t buf[MUSE_AUDIO_CHUNK];
    for (int done = 0; done < IN_FRAMES + OUT_FRAMES; done += MUSE_AUDIO_CHUNK) {
        if (uxQueueMessagesWaiting(s_queue) || (muse_state_sleepy() == 0 && !muse_state_asleep())) {
            break;
        }
        for (int i = 0; i < MUSE_AUDIO_CHUNK; i++) {
            int n = done + i;
            float t = (float)n / MUSE_AUDIO_RATE;
            seed = seed * 1664525u + 1013904223u;
            float noise = (float)(int32_t)seed / 2147483648.0f;
            float v = 0;
            if (n < IN_FRAMES) {
                float x = (float)n / IN_FRAMES;
                float env = sinf((float)M_PI * x);
                env *= env;
                phase += 2.0f * (float)M_PI * (base_hz + 20.0f * x) / MUSE_AUDIO_RATE;
                float tone = (sinf(phase) + 0.6f * sinf(2 * phase) + 0.35f * sinf(3 * phase)
                              + 0.2f * sinf(4 * phase)) / 2.15f;
                float flutter = 0.55f + 0.45f * sinf(2.0f * (float)M_PI * 25.0f * t);
                lp_in += 0.18f * (noise - lp_in);   /* one pole, ~500 Hz */
                v = env * (0.75f * tone * flutter + 0.5f * lp_in);
            } else if (n < IN_FRAMES + OUT_FRAMES) {
                float y = (float)(n - IN_FRAMES) / OUT_FRAMES;
                lp_out += 0.3f * (noise - lp_out);  /* ~900 Hz */
                v = 0.35f * sinf((float)M_PI * y) * lp_out;
            }
            buf[i] = (int16_t)(v * SNORE_AMP);
        }
        muse_audio_write(buf, MUSE_AUDIO_CHUNK);
    }
}

#if CONFIG_MUSE_NPSC_JINGLE
static void play_jingle(void)
{
    for (size_t i = 0; i < NPSC_JINGLE_FRAMES; i += MUSE_AUDIO_CHUNK) {
        if (uxQueueMessagesWaiting(s_queue)) {
            break;   /* a press goes first */
        }
        size_t n = NPSC_JINGLE_FRAMES - i < MUSE_AUDIO_CHUNK ? NPSC_JINGLE_FRAMES - i : MUSE_AUDIO_CHUNK;
        muse_audio_write(NPSC_JINGLE + i, n);
    }
}
#endif

#if CONFIG_MUSE_WAKE_WORD
/* Drop the speaker's tail so it isn't pre-roll and isn't the 3 s window.
 * Returns false if the session should end (menu, talk press, or stop). */
static bool convo_prepare_listen(const char **why)
{
    pre_reset();
    for (int i = 0; i < HF_DISCARD_CHUNKS; i++) {
        if (muse_menu_is_open()) {
            *why = "menu";
            return false;
        }
        int64_t stop = s_stop_us;
        if (stop && esp_timer_get_time() - stop < STOP_KEEP_US) {
            s_stop_us = 0;
            *why = "stop";
            return false;
        }
        bool down = false, up = false;
        drain_talk(&down, &up);
        if (down || up) {
            *why = "talk press";
            return false;
        }
        if (muse_audio_read(s_chunk, MUSE_AUDIO_CHUNK) != ESP_OK) {
            vTaskDelay(pdMS_TO_TICKS(20));
        }
        /* The discarded samples are not written to the pre-roll ring. */
    }
    s_pre_fill = 0;
    return true;
}

static void convo_end(const char *why)
{
    ESP_LOGI(TAG, "continuous conversation: ended (%s)", why);
    s_convo = false;
    s_convo_listen = false;
}
#endif


#if CONFIG_MUSE_NPSC_WAKE_RECORD
#define WR_DISCARD_CHUNKS 15            /* 300 ms after the cue beep, same as convo */
#define WR_RESULT_MS 800
#define WR_DONE_HOLD_MS 1200
#define WR_SAY_WAIT_US 20000000LL
#define WR_CAP_FRAMES (MUSE_AUDIO_RATE * NPSC_WAKE_RECORD_CAP_S)
#define WR_START_FRAMES (MUSE_AUDIO_RATE * NPSC_WAKE_RECORD_START_S)

static bool wr_cancelled(void)
{
    if (muse_npsc_wake_record_cancelled()) {
        return true;
    }
    if (muse_menu_is_open()) {
        muse_npsc_wake_record_cancel();
        return true;
    }
    return false;
}

static bool wr_say(const char *words)
{
    char *p = malloc(strlen(words) + 1);
    if (!p) {
        return false;
    }
    memcpy(p, words, strlen(words) + 1);
    muse_voice_clear_stop();
    if (!muse_hatch_say(p)) {
        return false;
    }
    int64_t t0 = esp_timer_get_time();
    while (!muse_hatch_push_take()) {
        if (wr_cancelled()) {
            muse_voice_stop_reply();
            return false;
        }
        if (esp_timer_get_time() - t0 > WR_SAY_WAIT_US) {
            ESP_LOGW(TAG, "wake record: say timed out");
            muse_voice_stop_reply();
            return false;
        }
        bool down = false, up = false;
        drain_talk(&down, &up);
        if (down || up) {
            muse_npsc_wake_record_cancel();
            muse_voice_stop_reply();
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    bool barge = play_push();
    pre_reset();
    if (barge) {
        muse_npsc_wake_record_cancel();
        return false;
    }
    return !wr_cancelled();
}

static void wr_discard(void)
{
    pre_reset();
    for (int i = 0; i < WR_DISCARD_CHUNKS; i++) {
        bool down = false, up = false;
        drain_talk(&down, &up);
        if (down || up || muse_menu_is_open()) {
            muse_npsc_wake_record_cancel();
            return;
        }
        if (muse_audio_read(s_chunk, MUSE_AUDIO_CHUNK) != ESP_OK) {
            vTaskDelay(pdMS_TO_TICKS(20));
        }
    }
    s_pre_fill = 0;
}

static const char *const WR_COUNT_WORD[] = { "no", "one", "two", "three", "four", "five" };

static void run_wake_record(void)
{
    muse_npsc_wake_record_begin();
    muse_npsc_wake_record_set(0, NPSC_WR_INTRO, 0);
    ESP_LOGI(TAG, "wake record: starting a session of %d", NPSC_WAKE_RECORD_ROUNDS);
    if (!wr_say("Let's do a test of five. I'll cue you each time.")) {
        if (wr_cancelled()) {
            goto done;
        }
        ESP_LOGW(TAG, "wake record: intro speech failed, continuing with beeps");
    }
    if (wr_cancelled()) {
        goto done;
    }

    for (int r = 1; r <= NPSC_WAKE_RECORD_ROUNDS; r++) {
        if (wr_cancelled()) {
            break;
        }
        muse_npsc_wake_record_set(r, NPSC_WR_CUE, muse_npsc_wake_record_session_saved());
        if (r > 1 && !wr_say("Again!") && wr_cancelled()) {
            break;
        }
        if (wr_cancelled()) {
            break;
        }
        muse_npsc_wake_record_set(r, NPSC_WR_LISTEN, muse_npsc_wake_record_session_saved());
        if (muse_settings_speaker_on()) {
            muse_audio_chirp(1);
        }
        wr_discard();
        if (wr_cancelled()) {
            break;
        }

        size_t held = 0, clip_from = 0;
        char why[96] = { 0 };
        bool spoke = false, cancelled = false;
        (void)record(false, true, &spoke, &held, why, sizeof(why), WR_START_FRAMES, true, &cancelled,
                     true, WR_CAP_FRAMES, &clip_from);
        muse_state_set_mode(MUSE_MODE_IDLE);
        pre_reset();
        if (cancelled || wr_cancelled()) {
            break;
        }
        if (spoke && s_rec && s_rec_n > clip_from) {
            size_t n = s_rec_n - clip_from;
            bool saved = muse_npsc_wake_record_save(s_rec + clip_from, n);
            ESP_LOGI(TAG, "wake record: round %d/%d %s (%.2fs, from %.2fs)", r, NPSC_WAKE_RECORD_ROUNDS,
                     saved ? "saved" : "skipped (no room)", (double)n / MUSE_AUDIO_RATE,
                     (double)clip_from / MUSE_AUDIO_RATE);
            muse_npsc_wake_record_set(r, saved ? NPSC_WR_SAVED : NPSC_WR_SKIPPED,
                                      muse_npsc_wake_record_session_saved());
        } else {
            ESP_LOGI(TAG, "wake record: round %d/%d skipped (%s after %.2fs)", r,
                     NPSC_WAKE_RECORD_ROUNDS, why[0] ? why : "nothing said",
                     (double)held / MUSE_AUDIO_RATE);
            muse_npsc_wake_record_set(r, NPSC_WR_SKIPPED, muse_npsc_wake_record_session_saved());
        }
        int64_t until = esp_timer_get_time() + (int64_t)WR_RESULT_MS * 1000;
        while (esp_timer_get_time() < until && !wr_cancelled()) {
            bool down = false, up = false;
            drain_talk(&down, &up);
            if (down || up) {
                muse_npsc_wake_record_cancel();
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(20));
        }
    }

    if (!wr_cancelled()) {
        int n = muse_npsc_wake_record_session_saved();
        muse_npsc_wake_record_set(0, NPSC_WR_DONE, n);
        char line[80];
        if (n >= 0 && n <= 5) {
            snprintf(line, sizeof(line), "You recorded %s clip%s.", WR_COUNT_WORD[n], n == 1 ? "" : "s");
        } else {
            snprintf(line, sizeof(line), "You recorded %d clips.", n);
        }
        (void)wr_say(line);
        int64_t until = esp_timer_get_time() + (int64_t)WR_DONE_HOLD_MS * 1000;
        while (esp_timer_get_time() < until && !wr_cancelled()) {
            vTaskDelay(pdMS_TO_TICKS(20));
        }
    }

done:
    ESP_LOGI(TAG, "wake record: session ended, %d of %d saved this time, %d clips in memory",
             muse_npsc_wake_record_session_saved(), NPSC_WAKE_RECORD_ROUNDS, muse_npsc_wake_record_stored());
    muse_npsc_wake_record_finish();
    go_idle("");
}
#endif

static void voice_task(void *arg)
{
    bool pending_down = false;
    muse_audio_selftest();
#if CONFIG_MUSE_WAKE_WORD
    s_wake_ok = muse_wake_init() == ESP_OK;
    s_wake_fresh = true;
#endif
    for (;;) {
        bool wake = false;
        bool hands_free = false;   /* "hey Santa" started this turn */
        bool follow_up = false;    /* auto-listen after a hands-free reply */
#if CONFIG_MUSE_WAKE_WORD
        if (s_convo_listen) {
            /* Stay out of idle: wake word stays paused until go_idle. */
            s_convo_listen = false;
            hands_free = true;
            follow_up = true;
            pending_down = false;
        } else
#endif
        if (!pending_down) {
            muse_input_event_t ev;
            bool asleep = muse_state_asleep();
            bool battery = muse_state_on_battery();
            bool rest = asleep && battery && !s_chirp && !s_mp3test && !s_loopback;
#if HOLD_NOTES
            /* A press goes first: send_held() leaves it queued and returns
             * without backing off, so retrying before it's read would spin. */
            if (held_due() && !press_waiting()) {
                set_resting(false);   /* full power while it goes, even asleep */
                muse_wifi_power(MUSE_WIFI_FULL);
                pending_down = send_held(asleep);
                pre_reset();
                if (!pending_down && !asleep && muse_state_mode(NULL) != MUSE_MODE_IDLE) {
                    muse_state_make_happy();
                    go_idle("");
                }
                continue;
            }
#endif
            set_resting(rest);
            /* On battery Wi-Fi rests with the codecs, and between turns dozes
             * between beacons as xiaozhi's idle does; a press wakes it fully. */
            muse_wifi_power(rest ? MUSE_WIFI_REST : battery ? MUSE_WIFI_DOZE : MUSE_WIFI_FULL);
            if (rest) {
                /* Wait for waking, USB power or a bench request without
                 * polling (the timeout is only a backstop, or a look for
                 * Wi-Fi to send saved notes), so the CPU can stay asleep. */
                muse_state_wait_awake(s_waiting ? HELD_POLL_MS : REST_BACKSTOP_MS);
                continue;
            }
            if (s_chirp) {
                s_chirp = false;
                muse_audio_chirp(1);
                pre_reset();
            }
            if (s_snore_ms) {
                uint32_t asked = s_snore_ms;
                s_snore_ms = 0;
                if (now_ms() - asked < SNORE_STALE_MS && muse_settings_speaker_on()) {
                    play_snore();
                    pre_reset();
                }
            }
            if (s_jingle) {
                s_jingle = false;
#if CONFIG_MUSE_NPSC_JINGLE
                if (muse_settings_speaker_on()) {
                    play_jingle();
                    pre_reset();
                }
#endif
            }
            if (s_mp3test) {
                s_mp3test = false;
                int16_t *pcm = NULL;
                size_t n = muse_hatch_mp3_selftest(&pcm);
                for (size_t i = 0; i < n; i += MUSE_AUDIO_CHUNK) {
                    muse_audio_write(pcm + i, n - i < MUSE_AUDIO_CHUNK ? n - i : MUSE_AUDIO_CHUNK);
                }
                free(pcm);
                pre_reset();
            }
            if (s_loopback) {
                s_loopback = false;
                muse_audio_loopback_test(muse_settings_volume());
                pre_reset();
            }
#if CONFIG_MUSE_NPSC_WAKE_RECORD
            if (muse_npsc_wake_record_wanted()) {
                set_resting(false);
                muse_wifi_power(MUSE_WIFI_FULL);
                run_wake_record();
                continue;
            }
#endif
            /* The 20 ms read paces this loop. */
            idle_capture();
#if CONFIG_MUSE_WAKE_WORD
            hands_free = s_wake_heard && !uxQueueMessagesWaiting(s_queue);   /* a press goes first */
            s_wake_heard = false;
#endif
            if (!hands_free && xQueueReceive(s_queue, &ev, 0) != pdTRUE) {
                if (muse_hatch_push_take()) {
                    pending_down = play_push();
                    pre_reset();
                    if (!pending_down && muse_state_mode(NULL) != MUSE_MODE_IDLE) {
                        muse_state_make_happy();
                        go_idle("");
                    }
                }
                continue;
            }
            muse_hatch_push_drop();   /* a press goes first; its turn replaces the push's */
            muse_state_poke();
            if (hands_free) {
                ESP_LOGI(TAG, "\"hey santa\": starting a turn");
            } else if (ev.type != MUSE_PTT_DOWN) {
                continue;
            } else {
                wake = ev.wake;
            }
        }
        if (wake && !held_on_waking()) {
            continue;   /* a tap: it only woke Muse */
        }
        muse_wifi_power(MUSE_WIFI_FULL);
        if (!can_record()) {
            pending_down = false;
#if CONFIG_MUSE_WAKE_WORD
            if (s_convo) {
                convo_end("can't record");
            }
#endif
            continue;
        }
        size_t held;
        char why[96] = { 0 };
        bool spoke;
        bool cancelled = false;
        size_t start_frames = 0;
#if CONFIG_MUSE_WAKE_WORD
        start_frames = follow_up ? HF_FOLLOW_FRAMES : HF_START_FRAMES;
#endif
        bool ok = record(pending_down, hands_free, &spoke, &held, why, sizeof(why),
                         start_frames, follow_up, &cancelled, false, 0, NULL);
        pending_down = false;
        if (!spoke) {   /* "hey Santa" and then nothing said, or a cancel */
            muse_hatch_turn_cancel();
            drop_rec();
            pre_reset();
#if CONFIG_MUSE_WAKE_WORD
            if (s_convo) {
                muse_state_make_happy();
                convo_end(cancelled && why[0] ? why : "nothing said");
            }
#endif
            go_idle("");
            continue;
        }
        if (held < MIN_HELD_FRAMES && !wake) {
            muse_hatch_turn_cancel();
            drop_rec();
            pre_reset();
#if CONFIG_MUSE_WAKE_WORD
            if (s_convo) {
                convo_end("too short");
            }
#endif
            go_idle("HOLD LONGER TO TALK");
            continue;
        }
        if (!ok) {
            pre_reset();
#if CONFIG_MUSE_WAKE_WORD
            if (s_convo) {
                convo_end("error");
            }
#endif
            go_idle(why);   /* Hatch failed it while recording */
            continue;
        }
#if CONFIG_MUSE_WAKE_WORD
        if (hands_free) {
            s_convo = true;
        }
#endif
        pending_down = finish_note();
        pre_reset();
#if CONFIG_MUSE_WAKE_WORD
        if (pending_down) {
            /* A talk barge-in during the reply leaves continuous mode. */
            if (s_convo) {
                convo_end("talk barge-in");
            }
        } else if (s_convo && muse_state_mode(NULL) != MUSE_MODE_IDLE) {
            const char *end_why = "stop";
            if (convo_prepare_listen(&end_why)) {
                ESP_LOGI(TAG, "continuous conversation: listening for 3 s");
                s_convo_listen = true;
            } else {
                muse_state_make_happy();
                go_idle("");
                convo_end(end_why);
            }
        } else {
            if (s_convo) {
                convo_end(muse_state_mode(NULL) == MUSE_MODE_IDLE ? "reply stopped" : "done");
            }
            if (muse_state_mode(NULL) != MUSE_MODE_IDLE) {
                muse_state_make_happy();
                go_idle("");
            }
        }
#else
        if (!pending_down && muse_state_mode(NULL) != MUSE_MODE_IDLE) {
            muse_state_make_happy();
            go_idle("");
        }
#endif
    }
}

esp_err_t muse_voice_start(QueueHandle_t queue)
{
    s_queue = queue;
    s_pre = heap_caps_malloc(PRE_CHUNKS * sizeof(pre_chunk_t), MUSE_BIG_CAPS);
    if (!s_pre || muse_audio_init(muse_settings_volume(), muse_settings_mic_gain()) != ESP_OK) {
        muse_state_set_mode(MUSE_MODE_ERROR);
        muse_state_set_caption("AUDIO INIT FAILED");
        return ESP_FAIL;
    }
    /* Stack in PSRAM if there is any (this task never writes flash) to spare internal RAM for Wi-Fi/BLE. */
    if (xTaskCreatePinnedToCoreWithCaps(voice_task, "muse_voice", 6144, NULL, 6, NULL, MUSE_AUDIO_CORE,
                                        MUSE_BIG_CAPS) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void muse_voice_set_monitor(bool on)
{
    s_monitor_db = -100.0f;
    s_monitor = on;
}

float muse_voice_monitor_db(void)
{
    return s_monitor_db;
}

void muse_voice_request_chirp(void)
{
    s_chirp = true;
    muse_state_nudge();
}

void muse_voice_stop_reply(void)
{
    int64_t now = esp_timer_get_time();
    s_stop_us = now ? now : 1;
}

void muse_voice_clear_stop(void)
{
    s_stop_us = 0;
}

void muse_voice_request_jingle(void)
{
    s_jingle = true;
    muse_state_nudge();
}

void muse_voice_request_snore(void)
{
    uint32_t ms = now_ms();
    s_snore_ms = ms ? ms : 1;
}

void muse_voice_request_loopback(void)
{
    s_loopback = true;
    muse_state_nudge();
}

void muse_voice_request_mp3test(void)
{
    s_mp3test = true;
    muse_state_nudge();
}

bool muse_voice_resting(void)
{
    return s_resting;
}

bool muse_voice_notes_waiting(void)
{
    return s_waiting;
}
