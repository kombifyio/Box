/*
 * kombify box - microWakeWord engine. See wakeword_mww.h.
 *
 * Follows ESPHome's micro_wake_word (streaming_model.cpp, micro_wake_word.cpp,
 * preprocessor_settings.h): the same frontend settings the models were trained
 * with, the same int8 feature scaling, the same 20 streaming ops, the same
 * sliding-window mean against a uint8 cutoff and the same 100-slice cool-off.
 */
#include "wakeword_mww.h"

#include <new>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "sdkconfig.h"

#include "frontend.h"
#include "frontend_util.h"
#include "tensorflow/lite/micro/micro_interpreter.h"
#include "tensorflow/lite/micro/micro_mutable_op_resolver.h"
#include "tensorflow/lite/micro/micro_resource_variable.h"
#include "tensorflow/lite/schema/schema_generated.h"

static const char *TAG = "kbx_wake";

/* preprocessor_settings.h: every microWakeWord model is trained with these. */
#define FEATURE_SIZE       40
#define FEATURE_MS         30
#define FEATURE_STEP_MS    10
#define SAMPLE_RATE        16000
#define MIN_SLICES         100   /* 1 s cool-off after a reset or a hit */
#define VAR_ARENA_SIZE     1024
#define MAX_RESOURCE_VARS  20
#define MAX_WINDOW         16
/* Boards without PSRAM (the ESP32-C6 variant) keep the engine in internal RAM. */
#if CONFIG_SPIRAM
#define CAPS_PSRAM         (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
#else
#define CAPS_PSRAM         (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)
#endif

struct mww {
    struct FrontendConfig fe_cfg;
    struct FrontendState fe;
    bool fe_ready;
    tflite::MicroMutableOpResolver<20> ops;
    uint8_t *arena;
    size_t arena_size;
    uint8_t *var_arena;
    tflite::MicroAllocator *ma;
    tflite::MicroResourceVariables *mrv;
    alignas(16) unsigned char interp_mem[sizeof(tflite::MicroInterpreter)];
    tflite::MicroInterpreter *interp;
    int stride;
    int stride_step;
    int window;
    int win_i;
    uint8_t probs[MAX_WINDOW];
    uint8_t cutoff_u8;
    int ignore;
    uint8_t last_p;
    uint8_t hit_mean;
};

static bool register_ops(tflite::MicroMutableOpResolver<20> &r)
{
    return r.AddCallOnce() == kTfLiteOk && r.AddVarHandle() == kTfLiteOk &&
           r.AddReshape() == kTfLiteOk && r.AddReadVariable() == kTfLiteOk &&
           r.AddStridedSlice() == kTfLiteOk && r.AddConcatenation() == kTfLiteOk &&
           r.AddAssignVariable() == kTfLiteOk && r.AddConv2D() == kTfLiteOk &&
           r.AddMul() == kTfLiteOk && r.AddAdd() == kTfLiteOk && r.AddMean() == kTfLiteOk &&
           r.AddFullyConnected() == kTfLiteOk && r.AddLogistic() == kTfLiteOk &&
           r.AddQuantize() == kTfLiteOk && r.AddDepthwiseConv2D() == kTfLiteOk &&
           r.AddAveragePool2D() == kTfLiteOk && r.AddMaxPool2D() == kTfLiteOk &&
           r.AddPad() == kTfLiteOk && r.AddPack() == kTfLiteOk && r.AddSplitV() == kTfLiteOk;
}

static bool frontend_setup(mww_t *m)
{
    struct FrontendConfig *c = &m->fe_cfg;
    FrontendFillConfigWithDefaults(c);
    c->window.size_ms = FEATURE_MS;
    c->window.step_size_ms = FEATURE_STEP_MS;
    c->filterbank.num_channels = FEATURE_SIZE;
    c->filterbank.lower_band_limit = 125.0f;
    c->filterbank.upper_band_limit = 7500.0f;
    c->noise_reduction.smoothing_bits = 10;
    c->noise_reduction.even_smoothing = 0.025f;
    c->noise_reduction.odd_smoothing = 0.06f;
    c->noise_reduction.min_signal_remaining = 0.05f;
    c->pcan_gain_control.enable_pcan = 1;
    c->pcan_gain_control.strength = 0.95f;
    c->pcan_gain_control.offset = 80.0f;
    c->pcan_gain_control.gain_bits = 21;
    c->log_scale.enable_log = 1;
    c->log_scale.scale_shift = 6;
    m->fe_ready = FrontendPopulateState(c, &m->fe, SAMPLE_RATE) != 0;
    return m->fe_ready;
}

static void interp_drop(mww_t *m)
{
    if (m->interp) {
        m->interp->~MicroInterpreter();
        m->interp = nullptr;
    }
}

/* The resource-variable allocator lives in its own small arena; a failed
 * AllocateTensors may have used it, so every attempt starts it afresh. */
static bool vars_fresh(mww_t *m)
{
    m->ma = tflite::MicroAllocator::Create(m->var_arena, VAR_ARENA_SIZE);
    m->mrv = m->ma ? tflite::MicroResourceVariables::Create(m->ma, MAX_RESOURCE_VARS) : nullptr;
    return m->mrv != nullptr;
}

/* Manifest arena first, then 1.5x and 2x: esp-nn versions need different
 * scratch sizes (ESPHome probes the same way). */
static esp_err_t interp_build(mww_t *m, const tflite::Model *model, size_t manifest)
{
    const size_t tries[] = { (manifest + 15) & ~(size_t)15, (manifest * 3 / 2 + 15) & ~(size_t)15,
                             (manifest * 2 + 15) & ~(size_t)15 };
    bool any_alloc = false;
    for (size_t i = 0; i < sizeof(tries) / sizeof(tries[0]); i++) {
        heap_caps_free(m->arena);
        m->arena = (uint8_t *)heap_caps_aligned_alloc(16, tries[i], CAPS_PSRAM);
        m->arena_size = m->arena ? tries[i] : 0;
        if (!m->arena) continue;
        any_alloc = true;
        if (!vars_fresh(m)) return ESP_ERR_NO_MEM;
        m->interp = new (m->interp_mem)
            tflite::MicroInterpreter(model, m->ops, m->arena, m->arena_size, m->mrv);
        if (m->interp->AllocateTensors() == kTfLiteOk) return ESP_OK;
        ESP_LOGW(TAG, "mww: tensor arena %u B too small, trying larger", (unsigned)m->arena_size);
        interp_drop(m);
    }
    return any_alloc ? ESP_ERR_INVALID_SIZE : ESP_ERR_NO_MEM;
}

void mww_destroy(mww_t *m)
{
    if (!m) return;
    interp_drop(m);
    if (m->fe_ready) FrontendFreeStateContents(&m->fe);
    heap_caps_free(m->arena);
    heap_caps_free(m->var_arena);
    m->~mww();
    heap_caps_free(m);
}

esp_err_t mww_create(const mww_model_t *desc, float cutoff, mww_t **out)
{
    *out = nullptr;
    if (!desc || !desc->model || desc->window < 1 || desc->window > MAX_WINDOW) {
        return ESP_ERR_INVALID_ARG;
    }
    const tflite::Model *model = tflite::GetModel(desc->model);
    if (model->version() != TFLITE_SCHEMA_VERSION) {
        ESP_LOGE(TAG, "mww: model schema %u, expected %d", (unsigned)model->version(),
                 TFLITE_SCHEMA_VERSION);
        return ESP_ERR_NOT_SUPPORTED;
    }
    void *mem = heap_caps_aligned_calloc(16, 1, sizeof(mww), CAPS_PSRAM);
    if (!mem) return ESP_ERR_NO_MEM;
    mww_t *m = new (mem) mww();
    m->window = desc->window;
    esp_err_t err = ESP_OK;
    if (!register_ops(m->ops)) {
        ESP_LOGE(TAG, "mww: op resolver");
        err = ESP_ERR_NOT_SUPPORTED;
    } else if (!frontend_setup(m)) {
        ESP_LOGE(TAG, "mww: frontend state");
        err = ESP_ERR_NO_MEM;
    } else if (!(m->var_arena = (uint8_t *)heap_caps_aligned_alloc(16, VAR_ARENA_SIZE, CAPS_PSRAM))) {
        err = ESP_ERR_NO_MEM;
    } else {
        err = interp_build(m, model, desc->arena_size);
        if (err == ESP_ERR_INVALID_SIZE) {
            ESP_LOGE(TAG, "mww: AllocateTensors failed up to %u B", (unsigned)m->arena_size);
            err = ESP_ERR_NOT_SUPPORTED;
        }
    }
    if (err == ESP_OK) {
        /* micro_wake_word's checks: int8 [1, stride, 40] in, uint8 [1, 1] out. */
        TfLiteTensor *in = m->interp->input(0);
        TfLiteTensor *o = m->interp->output(0);
        if (!in || in->dims->size != 3 || in->dims->data[0] != 1 ||
            in->dims->data[2] != FEATURE_SIZE || in->type != kTfLiteInt8 || !o ||
            o->dims->size != 2 || o->dims->data[0] != 1 || o->dims->data[1] != 1 ||
            o->type != kTfLiteUInt8 || in->dims->data[1] < 1) {
            ESP_LOGE(TAG, "mww: unexpected model tensors");
            err = ESP_ERR_NOT_SUPPORTED;
        } else {
            m->stride = in->dims->data[1];
        }
    }
    if (err != ESP_OK) {
        mww_destroy(m);
        return err;
    }
    mww_set_cutoff(m, cutoff);
    mww_reset(m);
    *out = m;
    return ESP_OK;
}

void mww_reset(mww_t *m)
{
    if (!m) return;
    FrontendReset(&m->fe);
    if (m->mrv) m->mrv->ResetAll();
    TfLiteTensor *in = m->interp ? m->interp->input(0) : nullptr;
    if (in) memset(in->data.int8, 0, in->bytes);
    m->stride_step = 0;
    memset(m->probs, 0, sizeof(m->probs));
    m->win_i = 0;
    m->ignore = -MIN_SLICES;
}

void mww_set_cutoff(mww_t *m, float cutoff)
{
    if (!m) return;
    if (cutoff < 0.0f) cutoff = 0.0f;
    if (cutoff > 1.0f) cutoff = 1.0f;
    m->cutoff_u8 = (uint8_t)(cutoff * 255.0f);   /* ESPHome: int(cutoff * 255) */
}

/* int8 input = feature * 256 / (25.6 * 26.0) - 128 (micro_wake_word.cpp). */
static void scale_features(const struct FrontendOutput *fo, int8_t *dst)
{
    for (size_t i = 0; i < fo->size && i < FEATURE_SIZE; i++) {
        int32_t v = ((int32_t)fo->values[i] * 256 + 333) / 666 - 128;
        dst[i] = (int8_t)(v < -128 ? -128 : v > 127 ? 127 : v);
    }
}

int mww_feed(mww_t *m, const int16_t *pcm, int samples, bool *hit)
{
    *hit = false;
    int invokes = 0;
    while (samples > 0) {
        size_t used = 0;
        struct FrontendOutput fo = FrontendProcessSamples(&m->fe, pcm, (size_t)samples, &used);
        if (used == 0 && fo.size == 0) break;
        pcm += used;
        samples -= (int)used;
        if (fo.size == 0) continue;

        TfLiteTensor *in = m->interp->input(0);
        scale_features(&fo, in->data.int8 + FEATURE_SIZE * m->stride_step);
        bool fresh = false;
        if (++m->stride_step >= m->stride) {
            m->stride_step = 0;
            if (m->interp->Invoke() != kTfLiteOk) return -1;
            ++invokes;
            m->last_p = m->interp->output(0)->data.uint8[0];
            m->win_i = (m->win_i + 1) % m->window;
            m->probs[m->win_i] = m->last_p;
            fresh = true;
        }
        if (m->probs[m->win_i] < m->cutoff_u8 && m->ignore < 0) ++m->ignore;
        if (!fresh || m->ignore < 0 || *hit) continue;
        uint32_t sum = 0;
        for (int i = 0; i < m->window; i++) sum += m->probs[i];
        if (sum > (uint32_t)m->cutoff_u8 * (uint32_t)m->window) {
            *hit = true;
            m->hit_mean = (uint8_t)(sum / (uint32_t)m->window);
            memset(m->probs, 0, sizeof(m->probs));
            m->ignore = -MIN_SLICES;
        }
    }
    return invokes;
}

float mww_last_probability(const mww_t *m)
{
    return m ? m->last_p / 255.0f : 0.0f;
}

float mww_hit_mean(const mww_t *m)
{
    return m ? m->hit_mean / 255.0f : 0.0f;
}

size_t mww_arena_size(const mww_t *m)
{
    return m ? m->arena_size : 0;
}

size_t mww_arena_used(const mww_t *m)
{
    return m && m->interp ? m->interp->arena_used_bytes() : 0;
}
