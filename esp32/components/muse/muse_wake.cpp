/*
 * "Hey Santa" wake word: see muse_wake.h.
 *
 * Mirrors ESPHome's micro_wake_word, which runs these models: every 10 ms of
 * audio the frontend gives 40 features (30 ms window); they're quantized to
 * the model's int8 input and the model runs on every 3 (30 ms stride). It
 * fires when the mean of the last SLIDING_WINDOW uint8 probabilities passes
 * the cutoff from hey_santa.json, then needs REFRACTORY_MS before another.
 */
#include "muse_wake.h"

#include <math.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "sdkconfig.h"

#include "muse_mem.h"

#include "tensorflow/lite/micro/micro_allocator.h"
#include "tensorflow/lite/micro/micro_interpreter.h"
#include "tensorflow/lite/micro/micro_mutable_op_resolver.h"
#include "tensorflow/lite/micro/micro_resource_variable.h"
#include "tensorflow/lite/micro/schema/schema_generated.h"

extern "C" {
#include "tensorflow/lite/experimental/microfrontend/lib/frontend.h"
#include "tensorflow/lite/experimental/microfrontend/lib/frontend_util.h"
}

static const char *TAG = "muse_wake";

/* Set by CMake only when wake/hey_santa.tflite is present to embed. */
#if defined(MUSE_WAKE_MODEL_EMBEDDED)
extern const uint8_t hey_santa_tflite_start[] asm("_binary_hey_santa_tflite_start");
extern const uint8_t hey_santa_tflite_end[] asm("_binary_hey_santa_tflite_end");
#endif

#define FEATURES 40
#define STEP_SAMPLES 160       /* 10 ms at 16 kHz */
#define STRIDE 3               /* feature frames per model run (its input is [1, 3, 40]) */
#define SLIDING_WINDOW 5       /* hey_santa.json sliding_window_size */
#define CUTOFF_U8 ((CONFIG_MUSE_WAKE_WORD_CUTOFF * 255 + 50) / 100)   /* json probability_cutoff 0.97 */
#define ARENA_SIZE (26080 + 6 * 1024)   /* json tensor_arena_size plus headroom */
#define VAR_ARENA_SIZE 1024
#define REFRACTORY_MS 2000

static bool s_ready;
static struct FrontendState s_fe;
static tflite::MicroInterpreter *s_interp;
static TfLiteTensor *s_in, *s_out;
static float s_in_scale;
static int s_in_zp;
static int16_t s_pend[STEP_SAMPLES];
static size_t s_pend_n;
static int s_frames;   /* feature frames in the input so far */
static uint8_t s_probs[SLIDING_WINDOW];
static int s_probs_n;   /* runs since reset, capped */
static int s_probs_i;
static int64_t s_quiet_until;

esp_err_t muse_wake_init(void)
{
#if !defined(MUSE_WAKE_MODEL_EMBEDDED)
    /* Public tree omits the weights. Hearing "hey santa" stays off. */
    ESP_LOGW(TAG, "wake/hey_santa.tflite is not in the tree; wake word stays off");
    return ESP_ERR_NOT_FOUND;
#else
    struct FrontendConfig cfg;   /* microWakeWord's training features (pymicro_features) */
    cfg.window.size_ms = 30;
    cfg.window.step_size_ms = 10;
    cfg.filterbank.num_channels = FEATURES;
    cfg.filterbank.lower_band_limit = 125.0f;
    cfg.filterbank.upper_band_limit = 7500.0f;
    cfg.noise_reduction.smoothing_bits = 10;
    cfg.noise_reduction.even_smoothing = 0.025f;
    cfg.noise_reduction.odd_smoothing = 0.06f;
    cfg.noise_reduction.min_signal_remaining = 0.05f;
    cfg.pcan_gain_control.enable_pcan = 1;
    cfg.pcan_gain_control.strength = 0.95f;
    cfg.pcan_gain_control.offset = 80.0f;
    cfg.pcan_gain_control.gain_bits = 21;
    cfg.log_scale.enable_log = 1;
    cfg.log_scale.scale_shift = 6;
    if (!FrontendPopulateState(&cfg, &s_fe, 16000)) {
        ESP_LOGE(TAG, "frontend init failed");
        return ESP_ERR_NO_MEM;
    }

    /* A flatbuffer wants alignment EMBED_FILES doesn't promise: copy it. */
    size_t len = hey_santa_tflite_end - hey_santa_tflite_start;
    uint8_t *model_buf = (uint8_t *)heap_caps_aligned_alloc(16, len, MUSE_BIG_CAPS);
    uint8_t *arena = (uint8_t *)heap_caps_aligned_alloc(16, ARENA_SIZE, MUSE_BIG_CAPS);
    uint8_t *var_arena = (uint8_t *)heap_caps_aligned_alloc(16, VAR_ARENA_SIZE, MUSE_BIG_CAPS);
    if (!model_buf || !arena || !var_arena) {
        ESP_LOGE(TAG, "no memory for the model");
        return ESP_ERR_NO_MEM;
    }
    memcpy(model_buf, hey_santa_tflite_start, len);
    const tflite::Model *model = tflite::GetModel(model_buf);
    if (model->version() != TFLITE_SCHEMA_VERSION) {
        ESP_LOGE(TAG, "model schema %lu, want %d", (unsigned long)model->version(), TFLITE_SCHEMA_VERSION);
        return ESP_FAIL;
    }

    static tflite::MicroMutableOpResolver<14> ops;
    ops.AddCallOnce();
    ops.AddVarHandle();
    ops.AddReadVariable();
    ops.AddAssignVariable();
    ops.AddReshape();
    ops.AddConv2D();
    ops.AddDepthwiseConv2D();
    ops.AddFullyConnected();
    ops.AddLogistic();
    ops.AddQuantize();
    ops.AddConcatenation();
    ops.AddSplitV();
    ops.AddStridedSlice();
    ops.AddMean();   /* not in this model; harmless, in case a retrain adds pooling */

    tflite::MicroAllocator *var_alloc = tflite::MicroAllocator::Create(var_arena, VAR_ARENA_SIZE);
    tflite::MicroResourceVariables *vars = tflite::MicroResourceVariables::Create(var_alloc, 20);
    s_interp = new tflite::MicroInterpreter(model, ops, arena, ARENA_SIZE, vars);
    if (!s_interp || s_interp->AllocateTensors() != kTfLiteOk) {
        ESP_LOGE(TAG, "allocating tensors failed");
        return ESP_FAIL;
    }
    s_in = s_interp->input(0);
    s_out = s_interp->output(0);
    if (s_in->type != kTfLiteInt8 || s_in->dims->size != 3 || s_in->dims->data[1] != STRIDE ||
        s_in->dims->data[2] != FEATURES || s_out->type != kTfLiteUInt8) {
        ESP_LOGE(TAG, "unexpected model input/output");
        return ESP_FAIL;
    }
    s_in_scale = s_in->params.scale;
    s_in_zp = s_in->params.zero_point;
    ESP_LOGI(TAG, "\"hey santa\" ready: model %u B, arena used %u of %u B, cutoff %d/255", (unsigned)len,
             (unsigned)s_interp->arena_used_bytes(), (unsigned)ARENA_SIZE, CUTOFF_U8);
    s_ready = true;
    muse_wake_reset();
    return ESP_OK;
#endif
}

void muse_wake_reset(void)
{
    if (!s_ready) {
        return;
    }
    s_pend_n = 0;
    s_frames = 0;
    s_probs_n = s_probs_i = 0;
    s_interp->Reset();   /* the model's streaming state (resource variables) */
}

/* One 10 ms step: true if this made the phrase heard. */
static bool step(const int16_t *pcm)
{
    size_t used = 0;
    struct FrontendOutput fo = FrontendProcessSamples(&s_fe, pcm, STEP_SAMPLES, &used);
    if (fo.size != FEATURES) {
        return false;
    }
    int8_t *row = s_in->data.int8 + s_frames * FEATURES;
    for (int i = 0; i < FEATURES; i++) {
        /* features * 0.0390625 as in training, then the input quantization */
        int32_t q = (int32_t)lroundf((float)fo.values[i] * 0.0390625f / s_in_scale) + s_in_zp;
        row[i] = (int8_t)(q < -128 ? -128 : (q > 127 ? 127 : q));
    }
    if (++s_frames < STRIDE) {
        return false;
    }
    s_frames = 0;
    if (s_interp->Invoke() != kTfLiteOk) {
        return false;
    }
    s_probs[s_probs_i] = s_out->data.uint8[0];
    s_probs_i = (s_probs_i + 1) % SLIDING_WINDOW;
    if (s_probs_n < SLIDING_WINDOW && ++s_probs_n < SLIDING_WINDOW) {
        return false;
    }
    int sum = 0;
    for (int i = 0; i < SLIDING_WINDOW; i++) {
        sum += s_probs[i];
    }
    if (sum <= CUTOFF_U8 * SLIDING_WINDOW) {
        return false;
    }
    int64_t now = esp_timer_get_time();
    if (now < s_quiet_until) {
        return false;
    }
    s_quiet_until = now + REFRACTORY_MS * 1000LL;
    ESP_LOGI(TAG, "heard \"hey santa\" (mean %d/255)", sum / SLIDING_WINDOW);
    return true;
}

bool muse_wake_feed(const int16_t *pcm, size_t frames)
{
    if (!s_ready) {
        return false;
    }
    bool heard = false;
    while (frames) {
        size_t n = STEP_SAMPLES - s_pend_n < frames ? STEP_SAMPLES - s_pend_n : frames;
        memcpy(s_pend + s_pend_n, pcm, n * sizeof(int16_t));
        s_pend_n += n;
        pcm += n;
        frames -= n;
        if (s_pend_n == STEP_SAMPLES) {
            s_pend_n = 0;
            heard |= step(s_pend);
        }
    }
    return heard;
}
