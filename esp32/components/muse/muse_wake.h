/*
 * "Hey Santa" wake word (CONFIG_MUSE_WAKE_WORD): a microWakeWord streaming
 * model (wake/hey_santa.tflite, run with esp-tflite-micro) on the 40-channel
 * TF micro frontend, fed the voice task's idle 16 kHz mic chunks. The voice
 * task starts a talk turn when it fires, as a talk-button press does.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Loads the model; false (and the wake word stays off) on failure. */
esp_err_t muse_wake_init(void);

/* Feeds 16 kHz mono PCM (any length); true once when the phrase is heard. */
bool muse_wake_feed(const int16_t *pcm, size_t frames);

/* Forgets the audio so far (after Muse makes a sound, or a turn), so its
 * own voice or a stale half-phrase can't trigger it. */
void muse_wake_reset(void);

#ifdef __cplusplus
}
#endif
