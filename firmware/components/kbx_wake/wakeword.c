/*
 * kombify box - local wake word. See wakeword.h.
 *
 * Engines (one runs at a time, chosen by the selected phrase):
 * - ESP-SR WakeNet9: "Hi,ESP" (wn9_hiesp, model info wakeNet9_v1h24_Hi,ESP,
 *   trained on recorded speech) and the TTS-trained Jarvis, Computer and
 *   Mycroft. The packed models (tools/pack_srmodels.py, built from the esp-sr
 *   component at build time) are embedded in the app image.
 * - microWakeWord (dev20, wakeword_mww.cc): TensorFlow Lite Micro streaming
 *   models on the microWakeWord frontend, as ESPHome runs them: the owner's
 *   own "Kombify", "Hey Kombify" and "Hey Kubi" (wakeword-training/) and the
 *   stock "Hey Jarvis" v2. Models, cutoffs and arena sizes come from their
 *   manifests in firmware/models/mww; arena and state live in PSRAM.
 * No extra flash partition or separate flash step is needed; OTA carries all
 * models. A phrase switch rebuilds the engine without a reboot.
 *
 * Fail-safe start (dev9). dev8 crash-looped once "wake on" was stored: the
 * engine was built at every boot from a model pack on an odd address. Now:
 * - the pack and every file in it sit on 16-byte boundaries;
 * - the detector task, the 480 ms capture history and all engine memory
 *   exist only while the wake word is on;
 * - the engine starts no earlier than 20 s after boot, once the voice path
 *   and its warm session are up and no TLS handshake runs;
 * - it never starts in a boot that follows a panic, watchdog or brownout,
 *   and an engine that did not survive 60 s turns the setting off;
 * - it waits, or yields its memory, while internal RAM runs short and
 *   retries later; only the crash guard switches the setting off.
 */
#include "wakeword.h"

#include <math.h>
#include <stdint.h>
#include <string.h>
#include <strings.h>

#include "sdkconfig.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"
#include "nvs.h"

#include "esp_wn_iface.h"
#include "esp_wn_models.h"
#include "model_path.h"

#include "audio.h"
#include "fw_ota.h"
#include "net_ws.h"
#include "wakeword_mww.h"

static const char *TAG = "kbx_wake";

/* kbx_srmodels.S (main/CMakeLists.txt): 16-byte aligned. */
extern const uint8_t kbx_srmodels_start[];
extern const uint8_t kbx_srmodels_end[];

/* microWakeWord models (main/CMakeLists.txt kbx_mww_models.S, 16-byte
 * aligned) with their manifest values (firmware/models/mww/<id>.json). */
extern const uint8_t kbx_mww_kombify_start[], kbx_mww_kombify_end[];
extern const uint8_t kbx_mww_hey_kombify_start[], kbx_mww_hey_kombify_end[];
extern const uint8_t kbx_mww_hey_kubi_start[], kbx_mww_hey_kubi_end[];
extern const uint8_t kbx_mww_hey_jarvis_start[], kbx_mww_hey_jarvis_end[];
typedef struct {
    const uint8_t *start;
    const uint8_t *end;
    uint16_t arena;       /* manifest tensor_arena_size */
    uint8_t window;       /* manifest sliding_window_size */
} mww_src_t;
static const mww_src_t k_mww[] = {
    { kbx_mww_kombify_start,     kbx_mww_kombify_end,     28344, 5 },
    { kbx_mww_hey_kombify_start, kbx_mww_hey_kombify_end, 28344, 5 },
    { kbx_mww_hey_kubi_start,    kbx_mww_hey_kubi_end,    28344, 5 },
    { kbx_mww_hey_jarvis_start,  kbx_mww_hey_jarvis_end,  22860, 5 },
};

/* Offered phrases (the released set). Only these models are packed into
 * the image (main/CMakeLists.txt) and only the selected one is built.
 * Index 0 is the default. Thresholds are kept per phrase; "Hi ESP" keeps
 * the dev8 key so its tuning survives. NVS stores the index: append new
 * phrases, never reorder.
 *
 * engine: every phrase names the detector that runs it; both kinds share
 * engine_t, the observer feed, the start gates, heap floors and crash guard. */
typedef enum {
    WAKE_ENGINE_ESPSR = 0,   /* ESP-SR WakeNet9, model from the srmodels pack */
    WAKE_ENGINE_MWW,         /* microWakeWord on TFLite Micro, model from k_mww */
} wake_engine_t;
typedef struct {
    const char *id;       /* KBX wake phrase <id>, NVS value is the index */
    const char *phrase;   /* shown in Quick Settings and logs */
    wake_engine_t engine;
    const char *model;    /* esp-sr model directory name, or the .tflite name */
    const char *thr_key;  /* NVS key, at most 15 characters */
    float default_thr;    /* 0 = the model's own threshold; mww: manifest cutoff */
    int8_t mww;           /* index into k_mww, -1 for ESP-SR */
    bool male_voice;      /* assist.voice auto: reply with a male voice */
} phrase_t;
static const phrase_t k_phrases[] = {
    { "hiesp",    "Hi ESP",   WAKE_ENGINE_ESPSR, "wn9_hiesp",        "thr_v2",       0.0f, -1, false },
    /* Espressif's TTS-trained WakeNet9 models; qualify each on the device. */
    /* Jarvis: the model's 0.627 gave no hits on the device; 0.50 does
     * (dev14 on-device test, 2026-10-20). */
    { "jarvis",   "Jarvis",   WAKE_ENGINE_ESPSR, "wn9_jarvis_tts",   "thr_jarvis",   0.50f, -1, true },
    { "computer", "Computer", WAKE_ENGINE_ESPSR, "wn9_computer_tts", "thr_computer", 0.0f, -1, true },
    { "mycroft",  "Mycroft",  WAKE_ENGINE_ESPSR, "wn9_mycroft_tts",  "thr_mycroft",  0.0f, -1, true },
    /* dev20: microWakeWord. Cutoffs are the manifests' probability_cutoff:
     * for the own models the lowest cutoff whose validation false accepts
     * stay under the target (training evaluation; held-out
     * 0.21, 0.18 and 0.09 per hour), for Hey Jarvis the stock 0.97. */
    { "kombify",     "Kombify",     WAKE_ENGINE_MWW, "kombify.tflite",     "thr_kombify",    0.994f, 0, false },
    { "hey_kombify", "Hey Kombify", WAKE_ENGINE_MWW, "hey_kombify.tflite", "thr_heykombify", 0.98f,  1, false },
    { "hey_kubi",    "Hey Kubi",    WAKE_ENGINE_MWW, "hey_kubi.tflite",    "thr_heykubi",    0.998f, 2, false },
    { "hey_jarvis",  "Hey Jarvis",  WAKE_ENGINE_MWW, "hey_jarvis.tflite",  "thr_heyjarvis",  0.97f,  3, true },
};
#define PHRASE_COUNT ((int)(sizeof(k_phrases) / sizeof(k_phrases[0])))
#define NVS_KEY_PHRASE     "phrase_v1"
#define NVS_NS             "kbx_wake"
#define NVS_KEY_ON         "on_v2"
#define NVS_KEY_ARM        "arm_v2"   /* set while a fresh engine is unproven */
#define NVS_KEY_OPT        "opt_v1"   /* KBX wake opt: crash-isolation variants */
/* dev21: measured internal cost per engine kind (bytes), so a phrase switch
 * or a reboot uses the measurement instead of the guess. */
static const char *const k_cost_keys[] = { "cost_espsr", "cost_mww" };
#define NVS_KEY_MWW_GAIN   "mwwgain_v1" /* KBX wake gain: mww input gain, x1 ... x16 */
#define MWW_GAIN_DEFAULT   1U
/* PSRAM stack: this image runs XIP from PSRAM, so flash writes (NVS) do not
 * disable the cache, and esp-dl's SIMD kernels work on cached PSRAM. The
 * high-water mark is reported in KBX wake?. */
#define TASK_STACK         (16 * 1024)
#define TASK_STACK_INTERNAL (10 * 1024)   /* KBX wake opt istack */
#define TASK_PRIO          5      /* above LVGL (4): a late reader loses audio */
#define TASK_CORE          1
#define REFRACTORY_US      1500000  /* one utterance, one hit */
#define LEVEL_FRAMES       75       /* 1.5 s of 20 ms frames */
#define SUMMARY_US         (60LL * 1000 * 1000)

#define START_UPTIME_US    (20LL * 1000 * 1000)  /* boot and warm-up first */
#define WARM_WAIT_MAX_US   (90LL * 1000 * 1000)  /* then start without a warm session */
#define STABLE_US          (60LL * 1000 * 1000)  /* engine proven: clear the arm marker */
/* Internal RAM: at least 15 KB stay free once the engine runs (TLS failed
 * near 12 KB). The start needs that plus the engine's own internal cost,
 * estimated until the first start has measured it. Below the keep floor a
 * running engine yields its memory. None of this switches the wake word
 * off: it retries when the heap allows (only the crash guard turns it off). */
#define MIN_INTERNAL_RUN   (15U * 1024U)
#define MIN_INTERNAL_KEEP  (10U * 1024U)
/* WakeNet9 measured ~6.7 KB on the device (dev20); 16 KB refused it at
 * 31.2 KB free. Only used until the engine has measured itself once. */
#define ENGINE_COST_GUESS  (8U * 1024U)
/* microWakeWord keeps its arena, variables and frontend in PSRAM; what is
 * left internal is the observer and allocator bookkeeping. */
#define MWW_COST_GUESS     (4U * 1024U)
#define HEAP_CHECK_US      (1000LL * 1000)
#define RETRY_HEAP_US      (15LL * 1000 * 1000)
#define REFUSE_LOG_US      (60LL * 1000 * 1000)

static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static TaskHandle_t s_task;
static volatile bool s_enabled;
static volatile bool s_ready;
static volatile bool s_listening;
static volatile bool s_test;
static volatile esp_err_t s_error;
static volatile float s_threshold;      /* 0 = model default */
static volatile int8_t s_store_on = -1; /* -1 none, else persist on=0/1 on the task */
static volatile bool s_store_thr;
static volatile int s_phrase;           /* index into k_phrases */
static volatile int s_phrase_pending = -1; /* switch requested, applied on the task */
static const char *volatile s_wait;     /* start gate the engine waits for */
static const char *s_held;              /* why this boot does not start it */
static volatile int64_t s_retry_at_us;  /* no start attempt before this */

/* dev11 crash isolation: dev10 panicked between "model pack" and "ready".
 * Each option changes one suspect; persisted, applied when the detector
 * task is created (istack, core0) or at the next engine start (others). */
#define OPT_ISTACK  (1U << 0)  /* detector stack in internal RAM, 10 KB */
#define OPT_NOEXT   (1U << 1)  /* no PSRAM steering of malloc during create */
#define OPT_NOARM   (1U << 2)  /* no NVS write before create; arm after it */
#define OPT_MODE90  (1U << 3)  /* DET_MODE_90 instead of DET_MODE_95 */
#define OPT_COPY    (1U << 4)  /* model pack from a PSRAM heap copy */
#define OPT_NOFLOOR (1U << 5)  /* start floor = keep floor (with istack) */
#define OPT_CORE0   (1U << 6)  /* detector task on core 0 */
#define OPT_ALL     0x7FU      /* bit 7 was dev12's noclean, now the default */
static const struct { const char *name; uint32_t bit; } k_opts[] = {
    { "istack", OPT_ISTACK }, { "noext", OPT_NOEXT }, { "noarm", OPT_NOARM },
    { "mode90", OPT_MODE90 }, { "copy", OPT_COPY }, { "nofloor", OPT_NOFLOOR },
    { "core0", OPT_CORE0 },
};
static volatile uint32_t s_opt;

/* Engine step trace at debug level (dev11/dev12 used it, with pauses, to
 * pin the WakeNet9 clean() crash). A panic is kept by panic_record.c. */
/* Remaining traced calls after an engine start (step_t). */
static int s_trace;

static void step(const char *what)
{
    ESP_LOGD(TAG, "wake: step %s (internal free %u)", what,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
}

/* Traced only for the first calls after an engine start. */
static void step_t(const char *what)
{
    if (s_trace > 0) {
        --s_trace;
        step(what);
    }
}
static int64_t s_refuse_logged_us;

/* qualification counters (task writes, readers tolerate tearing) */
static volatile uint32_t s_hits, s_hits_started, s_hits_refused;
static volatile uint32_t s_chunks, s_resets, s_detect_us_max;
static volatile uint64_t s_detect_us_sum, s_observe_us;
static int s_chunk_samples;
static float s_model_threshold;
static uint32_t s_internal_cost, s_psram_cost;
static uint32_t s_engine_cost[2];      /* measured, per wake_engine_t, persisted */
static volatile uint8_t s_mww_gain = MWW_GAIN_DEFAULT;

static uint8_t s_level_db[LEVEL_FRAMES]; /* -dBFS of each 20 ms frame peak */
static int s_level_pos;

static void nvs_put_u8(const char *key, uint8_t value)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u8(h, key, value);
    nvs_commit(h);
    nvs_close(h);
}

static void persist_threshold(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u16(h, k_phrases[s_phrase].thr_key, (uint16_t)(s_threshold * 1000.0f + 0.5f));
    nvs_commit(h);
    nvs_close(h);
}

static float load_threshold(int phrase)
{
    nvs_handle_t h;
    uint16_t thr = 0;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        (void)nvs_get_u16(h, k_phrases[phrase].thr_key, &thr);
        nvs_close(h);
    }
    return (thr >= 500 && thr <= 999) ? thr / 1000.0f : k_phrases[phrase].default_thr;
}

static uint8_t frame_peak_neg_db(const int16_t *pcm, int n)
{
    int peak = 1;
    for (int i = 0; i < n; i++) {
        int v = pcm[i] < 0 ? -pcm[i] : pcm[i];
        if (v > peak) peak = v;
    }
    float db = 20.0f * log10f((float)peak / 32768.0f);
    return (uint8_t)(db < -99.0f ? 99 : (int)(-db + 0.5f));
}

/* Loudest 20 ms frame of the last 1.5 s (the phrase), in dBFS. */
static int recent_peak_dbfs(void)
{
    int best = 99;
    for (int i = 0; i < LEVEL_FRAMES; i++) if (s_level_db[i] < best) best = s_level_db[i];
    return -best;
}

static bool may_observe(void)
{
    /* With echo cancellation the detector also listens while a reply or a
     * cue plays, so the wake word interrupts the reply (barge-in). */
    const bool duplex = audio_aec_active();
    if (audio_is_muted() || net_ws_talk_active() ||
        (!duplex && (audio_is_speaking() || !audio_play_agent_idle()))) {
        return false;
    }
    /* Test mode measures the detector alone, also offline. */
    return s_test || net_ws_ready_for_talk();
}

/* NULL when the engine may be built now, else what it waits for. */
static const char *start_gate(void)
{
    int64_t up = esp_timer_get_time();
    if (up < START_UPTIME_US) return "20 s uptime";
    if (net_ws_tls_busy()) return "TLS handshake";
    if (s_test) return NULL;
    if (!net_ws_ready_for_talk()) return "voice path";
    if (!net_ws_warm_ready() && up < WARM_WAIT_MAX_US) return "warm session";
    return NULL;
}

/* The embedded pack is aligned by kbx_srmodels.S; a PSRAM copy is the
 * fallback should a toolchain ever place it otherwise. */
static const void *model_pack(void)
{
    static const void *s_pack;
    if (s_pack) return s_pack;
    size_t size = (size_t)(kbx_srmodels_end - kbx_srmodels_start);
    if (((uintptr_t)kbx_srmodels_start & 15U) == 0 && !(s_opt & OPT_COPY)) {
        s_pack = kbx_srmodels_start;
    } else {
        void *copy = heap_caps_aligned_alloc(16, size, MALLOC_CAP_SPIRAM);
        if (!copy) return NULL;
        memcpy(copy, kbx_srmodels_start, size);
        ESP_LOGW(TAG, "model pack at %p: using an aligned PSRAM copy",
                 (const void *)kbx_srmodels_start);
        s_pack = copy;
    }
    ESP_LOGI(TAG, "model pack %u B at %p", (unsigned)size, s_pack);
    return s_pack;
}

typedef struct {
    const esp_wn_iface_t *wn;
    model_iface_data_t *model;   /* ESP-SR engine */
    mww_t *mww;                  /* microWakeWord engine */
    audio_observer_t *observer;
    int16_t *chunk;
    int fill;
    bool dirty;          /* real audio went into the model since the last flush */
    bool reset_pending;  /* flush before the next real chunk */
} engine_t;

/* WakeNet9's clean() dereferences a NULL feature queue in this mode
 * (dev12 panic record: LoadProhibited at dl_convq_queue_bzero+0x3 from
 * model_clean, excvaddr 0x60), so it is never called. The model's context
 * is flushed instead by feeding ~1.5 s of near-silent dither (not digital
 * zero, which a log filterbank may not expect); results are ignored. The
 * flush (~170 ms CPU) runs lazily before the first real chunk after a
 * reset, i.e. when the box is idle again, never when a turn starts. */
#define FLUSH_CHUNKS 48

static void engine_flush(engine_t *e)
{
    if (!e->model || !e->chunk || !e->dirty) return;
    uint32_t x = 0x2545F491u;
    for (int c = 0; c < FLUSH_CHUNKS; c++) {
        for (int i = 0; i < s_chunk_samples; i++) {
            x ^= x << 13; x ^= x >> 17; x ^= x << 5;
            e->chunk[i] = (int16_t)((int)(x & 3U) - 2);
        }
        (void)e->wn->detect(e->model, e->chunk);
    }
    memset(e->chunk, 0, (size_t)s_chunk_samples * sizeof(int16_t));
    e->dirty = false;
}

static void engine_reset(engine_t *e)
{
    step_t("engine_reset: observer reset");
    if (e->observer) audio_observer_reset_16k(e->observer);
    e->fill = 0;
    e->reset_pending = true;
    memset(s_level_db, 99, sizeof(s_level_db));
    ++s_resets;
}

static bool engine_built(const engine_t *e)
{
    return e->model || e->mww;
}

static const char *engine_name(wake_engine_t engine)
{
    return engine == WAKE_ENGINE_MWW ? "mww" : "esp-sr";
}

/* The threshold the running engine uses: the stored one, else the model's. */
static float effective_threshold(void)
{
    return s_threshold > 0.0f ? s_threshold : s_model_threshold;
}

static void engine_destroy(engine_t *e)
{
    s_ready = false;
    s_listening = false;
    if (e->model) e->wn->destroy(e->model);
    mww_destroy(e->mww);
    audio_observer_destroy_16k(e->observer);
    heap_caps_free(e->chunk);
    memset(e, 0, sizeof(*e));
}

static esp_err_t engine_create(engine_t *e)
{
    static srmodel_list_t *s_models;
    const phrase_t *ph = &k_phrases[s_phrase];
    char *s_name = NULL;
    if (ph->engine == WAKE_ENGINE_MWW && (ph->mww < 0 || ph->mww >= (int)(sizeof(k_mww) / sizeof(k_mww[0])))) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    size_t int0 = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    size_t ps0 = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    /* The libraries allocate through plain malloc; while the engine is
     * built, steer even small blocks to PSRAM. Explicit-caps allocations
     * of other tasks (DMA, Wi-Fi) are unaffected. */
    const uint32_t opt = s_opt;
    ESP_LOGI(TAG, "wake: engine build (%s), opt=0x%02x", engine_name(ph->engine), (unsigned)opt);
    if (!(opt & OPT_NOEXT)) heap_caps_malloc_extmem_enable(16);
    esp_err_t err = ESP_OK;
    int rate = 0;
    if (ph->engine == WAKE_ENGINE_MWW) {
        const mww_src_t *src = &k_mww[ph->mww];
        const mww_model_t md = {
            .model = src->start,
            .model_size = (size_t)(src->end - src->start),
            .arena_size = src->arena,
            .window = src->window,
        };
        s_model_threshold = ph->default_thr;
        step("mww create");
        err = mww_create(&md, effective_threshold(), &e->mww);
        step("mww created");
        if (err == ESP_OK) {
            e->observer = audio_observer_create_16k();
            if (!e->observer) err = ESP_ERR_NO_MEM;
            /* dev21: no 14x uplink boost into the frontend. ESPHome feeds
             * the microphone's own level; 14x clipped speech and gave
             * dev20's Hey Jarvis 0 hits. KBX wake gain tunes it. */
            else audio_observer_set_gain_q8(e->observer, 256U * s_mww_gain);
        }
        goto out;
    }
    if (!s_models) {
        const void *pack = model_pack();
        step("srmodel_load");
        s_models = pack ? srmodel_load(pack) : NULL;
        step("srmodel_filter");
    }
    s_name = s_models ? esp_srmodel_filter(s_models, ph->model, NULL) : NULL;
    if (!s_name) {
        ESP_LOGE(TAG, "model %s not in the embedded pack", ph->model);
        err = ESP_ERR_NOT_FOUND;
        goto out;
    }
    step("handle_from_name");
    e->wn = esp_wn_handle_from_name(s_name);
    if (!e->wn) {
        err = ESP_ERR_NOT_SUPPORTED;
        goto out;
    }
    /* DET_MODE_95: weights copied to PSRAM; false accepts are held down
     * with the detection threshold instead (KBX wake thr). */
    step(opt & OPT_MODE90 ? "create DET_MODE_90" : "create DET_MODE_95");
    e->model = e->wn->create(s_name, (opt & OPT_MODE90) ? DET_MODE_90 : DET_MODE_95);
    step("created");
    if (!e->model) {
        err = ESP_ERR_NO_MEM;
        goto out;
    }
    s_model_threshold = e->wn->get_det_threshold(e->model, 1);
    if (s_threshold > 0.0f) e->wn->set_det_threshold(e->model, s_threshold, 1);
    s_chunk_samples = e->wn->get_samp_chunksize(e->model);
    rate = e->wn->get_samp_rate(e->model);
    if (rate != 16000 || s_chunk_samples <= 0 || s_chunk_samples > 4096) {
        ESP_LOGE(TAG, "unexpected model format: rate=%d chunk=%d", rate, s_chunk_samples);
        err = ESP_ERR_NOT_SUPPORTED;
        goto out;
    }
    step("chunk calloc");
    e->chunk = heap_caps_aligned_calloc(16, (size_t)s_chunk_samples, sizeof(int16_t),
                                        MALLOC_CAP_SPIRAM);
    step("observer create");
    e->observer = audio_observer_create_16k();
    step(e->observer ? "observer created" : "observer create failed");
    if (!e->chunk || !e->observer) err = ESP_ERR_NO_MEM;
out:
    if (!(opt & OPT_NOEXT)) heap_caps_malloc_extmem_enable(CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL);
    step("malloc steering restored");
    if (err != ESP_OK) {
        engine_destroy(e);
        return err;
    }
    size_t int1 = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    size_t ps1 = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    s_internal_cost = int0 > int1 ? (uint32_t)(int0 - int1) : 0;
    if (s_internal_cost + 512U < s_engine_cost[ph->engine] || s_internal_cost > s_engine_cost[ph->engine] + 512U) {
        s_engine_cost[ph->engine] = s_internal_cost;
        nvs_handle_t h;
        if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
            nvs_set_u32(h, k_cost_keys[ph->engine], s_internal_cost);
            nvs_commit(h);
            nvs_close(h);
        }
    }
    s_psram_cost = ps0 > ps1 ? (uint32_t)(ps0 - ps1) : 0;
    if (e->mww) {
        ESP_LOGI(TAG, "\"%s\" ready: mww %s (%u B) arena=%u B (used %u) window=%d "
                 "threshold=%.3f (model %.3f) heap internal=-%u B (free %u) psram=-%u B (free %u)",
                 ph->phrase, ph->model, (unsigned)(k_mww[ph->mww].end - k_mww[ph->mww].start),
                 (unsigned)mww_arena_size(e->mww), (unsigned)mww_arena_used(e->mww),
                 (int)k_mww[ph->mww].window, effective_threshold(), s_model_threshold,
                 (unsigned)s_internal_cost, (unsigned)int1, (unsigned)s_psram_cost, (unsigned)ps1);
    } else {
        ESP_LOGI(TAG, "\"%s\" ready: %s chunk=%d samples threshold=%.3f (model %.3f) "
                 "heap internal=-%u B (free %u) psram=-%u B (free %u)",
                 ph->phrase, s_name, s_chunk_samples, effective_threshold(), s_model_threshold,
                 (unsigned)s_internal_cost, (unsigned)int1, (unsigned)s_psram_cost, (unsigned)ps1);
    }
    s_trace = 12;
    step_t("ready logged");
    engine_reset(e);
    step_t("engine_reset done");
    s_ready = true;
    return ESP_OK;
}

static const char *state_name(void)
{
    if (s_listening) return "listening";
    if (s_ready) return "paused";
    if (s_enabled && s_wait) return "waiting";
    if (s_enabled) return "starting";
    if (s_held) return "held";
    return "off";
}

static void log_summary(void)
{
    uint64_t obs = s_observe_us;
    uint32_t chunks = s_chunks;
    double hours = obs / 3.6e9;
    const char *wait = s_wait;
    ESP_LOGI(TAG, "wake: enabled=%d ready=%d test=%d listening=%d state=%s%s%s observed=%.2f h hits=%u "
             "(started=%u refused=%u, %.2f/h) detect avg=%u us max=%u us cpu=%.1f%% "
             "threshold=%.3f resets=%u heap internal=-%u psram=-%u",
             (int)s_enabled, (int)s_ready, (int)s_test, (int)s_listening, state_name(),
             s_enabled && wait ? " for " : "", s_enabled && wait ? wait : "", hours,
             (unsigned)s_hits, (unsigned)s_hits_started, (unsigned)s_hits_refused,
             hours > 0.0 ? s_hits / hours : 0.0,
             chunks ? (unsigned)(s_detect_us_sum / chunks) : 0u, (unsigned)s_detect_us_max,
             obs ? 100.0 * (double)s_detect_us_sum / (double)obs : 0.0,
             effective_threshold(), (unsigned)s_resets,
             (unsigned)s_internal_cost, (unsigned)s_psram_cost);
    ESP_LOGI(TAG, "wake: phrase=%s engine=%s internal free=%u largest=%u min=%u psram free=%u stack free=%u reset=%s%s%s err=%s",
             k_phrases[s_phrase].id, engine_name(k_phrases[s_phrase].engine),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
             s_task ? (unsigned)uxTaskGetStackHighWaterMark(s_task) : 0u,
             fw_ota_reset_reason_name(), s_held ? " held=" : "", s_held ? s_held : "",
             esp_err_to_name(s_error));
}

static size_t start_floor(void)
{
    if (s_opt & OPT_NOFLOOR) return MIN_INTERNAL_KEEP;
    const wake_engine_t kind = k_phrases[s_phrase].engine;
    uint32_t cost = s_internal_cost ? s_internal_cost : s_engine_cost[kind];
    if (!cost) cost = kind == WAKE_ENGINE_MWW ? MWW_COST_GUESS : ENGINE_COST_GUESS;
    return MIN_INTERNAL_RUN + cost;
}

/* The setting stays on; the task tries again after RETRY_HEAP_US. */
static void refuse_low_heap(size_t internal, const char *what)
{
    int64_t now = esp_timer_get_time();
    if (!s_refuse_logged_us || now - s_refuse_logged_us >= REFUSE_LOG_US) {
        s_refuse_logged_us = now;
        ESP_LOGW(TAG, "wake: %s (low internal heap %u, needs %u); retry in %d s",
                 what, (unsigned)internal, (unsigned)start_floor(),
                 (int)(RETRY_HEAP_US / 1000000));
    }
    s_error = ESP_ERR_NO_MEM;
    s_wait = "internal heap";
    s_retry_at_us = now + RETRY_HEAP_US;
}

static void detector_task(void *arg)
{
    (void)arg;
    engine_t e = {0};
    audio_observer_frame_t frame;
    int64_t last_us = 0, refractory_until = 0, last_summary = esp_timer_get_time();
    int64_t armed_since = 0, last_heap_check = 0;
    bool first_detect = false;
    bool paused = true;
    for (;;) {
        int8_t store_on = s_store_on;
        if (store_on >= 0) {
            s_store_on = -1;
            nvs_put_u8(NVS_KEY_ON, (uint8_t)store_on);
        }
        int pending = s_phrase_pending;
        if (pending >= 0) {
            /* Phrase switch without a reboot: drop the running engine; the
             * start path below builds the new model through the same gates,
             * heap floors and crash guard as any start. */
            s_phrase_pending = -1;
            if (engine_built(&e)) {
                engine_destroy(&e);
                if (armed_since) nvs_put_u8(NVS_KEY_ARM, 0);
                armed_since = 0;
            }
            s_phrase = pending;
            s_threshold = load_threshold(pending);
            s_internal_cost = 0;   /* re-measured for this model */
            s_retry_at_us = 0;
            nvs_put_u8(NVS_KEY_PHRASE, (uint8_t)pending);
            ESP_LOGI(TAG, "wake: phrase \"%s\" (%s %s)%s", k_phrases[pending].phrase,
                     engine_name(k_phrases[pending].engine), k_phrases[pending].model,
                     s_enabled ? ", engine restarts" : "");
        }
        if (s_store_thr) {
            s_store_thr = false;
            persist_threshold();
            if (e.model && s_ready) {
                if (s_threshold > 0.0f) e.wn->set_det_threshold(e.model, s_threshold, 1);
                else e.wn->reset_det_threshold(e.model);
            }
            if (e.mww && s_ready) mww_set_cutoff(e.mww, effective_threshold());
        }
        if (s_enabled && !engine_built(&e)) {
            const char *wait = esp_timer_get_time() < s_retry_at_us ? s_wait : start_gate();
            if (wait) {
                if (wait != s_wait) ESP_LOGI(TAG, "wake: start waits for %s", wait);
                s_wait = wait;
                ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000));
                continue;
            }
            s_wait = NULL;
            size_t internal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
            if (internal < start_floor()) {
                refuse_low_heap(internal, "refused");
                continue;
            }
            ESP_LOGI(TAG, "wake: starting engine (internal free %u)", (unsigned)internal);
            /* If the box does not survive the next 60 s, the next boot
             * turns the wake word off instead of trying again. */
            if (!(s_opt & OPT_NOARM)) nvs_put_u8(NVS_KEY_ARM, 1);
            esp_err_t err = engine_create(&e);
            s_error = err;
            if (err == ESP_ERR_NO_MEM) {
                nvs_put_u8(NVS_KEY_ARM, 0);
                refuse_low_heap(heap_caps_get_free_size(MALLOC_CAP_INTERNAL), "engine build failed");
                continue;
            }
            if (err != ESP_OK) {
                /* Model/format problem: no retry until the next KBX wake on. */
                nvs_put_u8(NVS_KEY_ARM, 0);
                ESP_LOGW(TAG, "wake word unavailable: %s (tap still works)", esp_err_to_name(err));
                s_wait = "engine unavailable";
                s_retry_at_us = INT64_MAX;
                continue;
            }
            internal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
            if (internal < ((s_opt & OPT_NOFLOOR) ? MIN_INTERNAL_KEEP : MIN_INTERNAL_RUN)) {
                engine_destroy(&e);
                nvs_put_u8(NVS_KEY_ARM, 0);
                refuse_low_heap(internal, "stopped after start");
                continue;
            }
            s_error = ESP_OK;
            if (s_opt & OPT_NOARM) nvs_put_u8(NVS_KEY_ARM, 1);
            armed_since = esp_timer_get_time();
            first_detect = true;
            paused = true;
        } else if (!s_enabled && engine_built(&e)) {
            engine_destroy(&e);
            if (armed_since) nvs_put_u8(NVS_KEY_ARM, 0);
            armed_since = 0;
            ESP_LOGI(TAG, "wake word off");
        }
        int64_t now = esp_timer_get_time();
        if (!engine_built(&e)) {
            s_listening = false;
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(500));
            continue;
        }
        if (armed_since && now - armed_since >= STABLE_US) {
            armed_since = 0;
            nvs_put_u8(NVS_KEY_ARM, 0);
            ESP_LOGI(TAG, "wake: engine stable for %d s", (int)(STABLE_US / 1000000));
        }
        if (now - last_heap_check >= HEAP_CHECK_US) {
            last_heap_check = now;
            size_t internal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
            if (internal < MIN_INTERNAL_KEEP) {
                engine_destroy(&e);
                if (armed_since) nvs_put_u8(NVS_KEY_ARM, 0);
                armed_since = 0;
                refuse_low_heap(internal, "paused");
                continue;
            }
        }
        if (now - last_summary >= SUMMARY_US) {
            last_summary = now;
            log_summary();
        }
        step_t("may_observe");
        if (!may_observe() || now < refractory_until) {
            if (!paused) engine_reset(&e);
            paused = true;
            last_us = 0;
            s_listening = false;
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        if (paused) engine_reset(&e);
        paused = false;
        step_t("observer read");
        s_listening = true;
        esp_err_t err = audio_observer_read_16k(e.observer, &frame);
        if (err == ESP_ERR_NOT_FOUND) {
            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }
        if (err != ESP_OK) {
            /* overrun or mute change: context is gone */
            engine_reset(&e);
            last_us = 0;
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        now = esp_timer_get_time();
        if (last_us) s_observe_us += (uint64_t)(now - last_us);
        last_us = now;
        s_level_db[s_level_pos] = frame_peak_neg_db(frame.samples, AUDIO_CAPTURE_16K_FRAME_SAMPLES);
        s_level_pos = (s_level_pos + 1) % LEVEL_FRAMES;

        if (e.reset_pending) {
            /* First frame after a reset; e.fill is 0, so e.chunk is free. */
            e.reset_pending = false;
            if (e.mww) {
                /* Cheap: frontend, streaming variables and window cleared;
                 * the model then sits out 1 s of fresh audio. */
                mww_reset(e.mww);
                e.dirty = false;
            } else if (e.dirty) {
                step_t("flush");
                engine_flush(&e);
            }
        }
        bool hit = false;
        if (e.mww) {
            /* 320 samples = two 10 ms feature slices; one model invocation
             * per three slices (30 ms). */
            int64_t t0 = esp_timer_get_time();
            int runs = mww_feed(e.mww, frame.samples, AUDIO_CAPTURE_16K_FRAME_SAMPLES, &hit);
            uint32_t dt = (uint32_t)(esp_timer_get_time() - t0);
            if (runs < 0) {
                /* Invoke failed: a model problem, not a heap one. */
                ESP_LOGE(TAG, "wake: mww invoke failed; wake word unavailable (tap still works)");
                engine_destroy(&e);
                if (armed_since) nvs_put_u8(NVS_KEY_ARM, 0);
                armed_since = 0;
                s_error = ESP_FAIL;
                s_wait = "engine unavailable";
                s_retry_at_us = INT64_MAX;
                memset(frame.samples, 0, sizeof(frame.samples));
                continue;
            }
            /* Frontend time counts too: detect avg is per invocation (30 ms
             * of audio), cpu= the share of observed time. */
            s_detect_us_sum += dt;
            if (dt > s_detect_us_max) s_detect_us_max = dt;
            if (runs > 0) {
                e.dirty = true;
                s_chunks += (uint32_t)runs;
                if (first_detect) {
                    first_detect = false;
                    ESP_LOGI(TAG, "wake: first detect ok (%u us, stack free %u)", (unsigned)dt,
                             (unsigned)uxTaskGetStackHighWaterMark(NULL));
                }
            }
        }
        /* ESP-SR: get_samp_chunksize (512 for wn9) is not a multiple of the
         * 320-sample frame: accumulate and run the detector per full chunk. */
        int off = e.mww ? AUDIO_CAPTURE_16K_FRAME_SAMPLES : 0;
        while (off < AUDIO_CAPTURE_16K_FRAME_SAMPLES) {
            int take = s_chunk_samples - e.fill;
            if (take > AUDIO_CAPTURE_16K_FRAME_SAMPLES - off) take = AUDIO_CAPTURE_16K_FRAME_SAMPLES - off;
            memcpy(e.chunk + e.fill, frame.samples + off, (size_t)take * sizeof(int16_t));
            e.fill += take;
            off += take;
            if (e.fill == s_chunk_samples) {
                e.fill = 0;
                if (first_detect) step("first detect");
                int64_t t0 = esp_timer_get_time();
                wakenet_state_t r = e.wn->detect(e.model, e.chunk);
                e.dirty = true;
                if (first_detect) {
                    first_detect = false;
                    ESP_LOGI(TAG, "wake: first detect ok (%u us, stack free %u)",
                             (unsigned)(esp_timer_get_time() - t0),
                             (unsigned)uxTaskGetStackHighWaterMark(NULL));
                }
                uint32_t dt = (uint32_t)(esp_timer_get_time() - t0);
                s_detect_us_sum += dt;
                if (dt > s_detect_us_max) s_detect_us_max = dt;
                ++s_chunks;
                if (r > 0) hit = true;
            }
        }
        memset(frame.samples, 0, sizeof(frame.samples));
        if (!hit) continue;

        ++s_hits;
        int level = recent_peak_dbfs();
        bool started = false;
        bool barge = false;
        if (!s_test && audio_observer_epoch_current(frame.mute_epoch)) {
            /* Barge-in: a hit while the reply plays stops it and listens. */
            barge = audio_aec_active() &&
                (audio_is_speaking() || !audio_play_agent_idle() || net_ws_conversation_active());
            started = barge ? net_ws_talk_barge_in(frame.mute_epoch)
                            : net_ws_talk_start_for_wakeword(frame.mute_epoch);
            if (started) ++s_hits_started;
            else ++s_hits_refused;
        }
        char score[24] = "";
        if (e.mww) snprintf(score, sizeof(score), " score=%.3f", mww_hit_mean(e.mww));
        ESP_LOGI(TAG, "wake: hit #%u \"%s\" peak=%d dBFS (last 1.5 s)%s%s", (unsigned)s_hits,
                 k_phrases[s_phrase].phrase, level,
                 s_test ? " [test: counted only]" : started ? (barge ? " -> barge-in, listening" : " -> listening")
                                                         : " -> refused (busy)",
                 score);
        refractory_until = esp_timer_get_time() + REFRACTORY_US;
        engine_reset(&e);
        paused = true;
    }
}

/* The task, its stack and everything the engine owns exist only once the
 * wake word has been switched on. */
static esp_err_t ensure_task(void)
{
    if (s_task) return ESP_OK;
    const uint32_t opt = s_opt;
    const BaseType_t core = (opt & OPT_CORE0) ? 0 : TASK_CORE;
    BaseType_t ok;
    if (opt & OPT_ISTACK) {
        ok = xTaskCreatePinnedToCore(detector_task, "kbx_wake", TASK_STACK_INTERNAL, NULL,
                                     TASK_PRIO, &s_task, core);
    } else {
        ok = xTaskCreatePinnedToCoreWithCaps(detector_task, "kbx_wake", TASK_STACK, NULL, TASK_PRIO,
                                             &s_task, core, MALLOC_CAP_SPIRAM);
    }
    if (ok != pdPASS) {
        s_task = NULL;
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "wake: detector task on core %d, %s stack %u B", (int)core,
             (opt & OPT_ISTACK) ? "internal" : "PSRAM",
             (unsigned)((opt & OPT_ISTACK) ? TASK_STACK_INTERNAL : TASK_STACK));
    return ESP_OK;
}

esp_err_t wakeword_init(void)
{
    static bool s_init;
    if (s_init) return ESP_ERR_INVALID_STATE;
    s_init = true;
    nvs_handle_t h;
    uint8_t on = 0, arm = 0;
    uint8_t phrase = 0;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        (void)nvs_get_u8(h, NVS_KEY_ON, &on);
        (void)nvs_get_u8(h, NVS_KEY_PHRASE, &phrase);
        (void)nvs_get_u8(h, NVS_KEY_ARM, &arm);
        uint32_t opt = 0;
        if (nvs_get_u32(h, NVS_KEY_OPT, &opt) == ESP_OK) s_opt = opt & OPT_ALL;
        for (int k = 0; k < 2; k++) (void)nvs_get_u32(h, k_cost_keys[k], &s_engine_cost[k]);
        uint8_t gain = MWW_GAIN_DEFAULT;
        if (nvs_get_u8(h, NVS_KEY_MWW_GAIN, &gain) == ESP_OK && gain >= 1 && gain <= 16) s_mww_gain = gain;
        nvs_close(h);
    }
    s_phrase = phrase < PHRASE_COUNT ? phrase : 0;
    s_threshold = load_threshold(s_phrase);
    memset(s_level_db, 99, sizeof(s_level_db));
    if (arm) {
        /* The engine was started and the box went down within 60 s.
         * A software restart (OTA, KBX reboot) does not count. */
        nvs_put_u8(NVS_KEY_ARM, 0);
        if (on && esp_reset_reason() != ESP_RST_SW) {
            on = 0;
            nvs_put_u8(NVS_KEY_ON, 0);
            s_error = ESP_FAIL;
            s_held = "unproven start";
            ESP_LOGW(TAG, "wake: turned off - the last engine start did not survive %d s "
                     "(reset %s); KBX wake on retries", (int)(STABLE_US / 1000000),
                     fw_ota_reset_reason_name());
        }
    }
    if (on && fw_ota_reset_was_crash()) {
        on = 0;   /* this boot only; the setting stays on */
        s_held = "crash reset";
        ESP_LOGW(TAG, "wake: not starting this boot (last reset %s); KBX wake on starts it",
                 fw_ota_reset_reason_name());
    }
    s_enabled = on != 0;
    if (s_enabled && ensure_task() != ESP_OK) {
        s_enabled = false;
        s_error = ESP_ERR_NO_MEM;
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "wake word \"%s\" %s (KBX wake on|off|phrase <id>|test on|off|thr <0.5-0.999>|?)",
             k_phrases[s_phrase].phrase, s_enabled ? "on, starts >= 20 s after boot" : "off");
    return ESP_OK;
}

esp_err_t wakeword_set_enabled(bool enabled)
{
    if (enabled) {
        s_error = ESP_OK;
        s_held = NULL;
        s_retry_at_us = 0;
        if (ensure_task() != ESP_OK) {
            s_error = ESP_ERR_NO_MEM;
            return ESP_ERR_NO_MEM;
        }
    }
    s_enabled = enabled;
    if (!s_task) {
        nvs_put_u8(NVS_KEY_ON, enabled ? 1 : 0);
        return ESP_OK;
    }
    s_store_on = enabled ? 1 : 0;
    xTaskNotifyGive(s_task);
    return ESP_OK;
}

void wakeword_set_test_mode(bool on)
{
    s_test = on;
    ESP_LOGI(TAG, "test mode %s (hits are %s)", on ? "on" : "off",
             on ? "counted only" : "live: they start a turn");
    if (s_task) xTaskNotifyGive(s_task);
}

esp_err_t wakeword_set_threshold(float threshold)
{
    /* Up to 0.999: microWakeWord cutoffs sit at 0.97 ... 0.998. */
    if (threshold != 0.0f && (threshold < 0.5f || threshold > 0.999f)) return ESP_ERR_INVALID_ARG;
    /* 0 = the phrase's default (which may be the model's own threshold). */
    s_threshold = threshold > 0.0f ? threshold : k_phrases[s_phrase].default_thr;
    if (!s_task) {
        persist_threshold();
        return ESP_OK;
    }
    s_store_thr = true;
    xTaskNotifyGive(s_task);
    return ESP_OK;
}

void wakeword_get_status(wakeword_status_t *st)
{
    if (!st) return;
    portENTER_CRITICAL(&s_mux);
    st->enabled = s_enabled;
    st->ready = s_ready;
    st->listening = s_listening;
    st->test_mode = s_test;
    st->error = s_error;
    st->hits = s_hits;
    portEXIT_CRITICAL(&s_mux);
}

void wakeword_reset_counters(void)
{
    s_hits = s_hits_started = s_hits_refused = 0;
    s_chunks = s_resets = s_detect_us_max = 0;
    s_detect_us_sum = s_observe_us = 0;
    ESP_LOGI(TAG, "counters reset");
}

void wakeword_log_status(void)
{
    log_summary();
}

int wakeword_phrase_count(void)
{
    return PHRASE_COUNT;
}

const char *wakeword_phrase_id(int index)
{
    return index >= 0 && index < PHRASE_COUNT ? k_phrases[index].id : NULL;
}

const char *wakeword_phrase_text(int index)
{
    return index >= 0 && index < PHRASE_COUNT ? k_phrases[index].phrase : "";
}

int wakeword_get_phrase(void)
{
    int pending = s_phrase_pending;
    return pending >= 0 ? pending : s_phrase;
}

esp_err_t wakeword_set_phrase(int index)
{
    if (index < 0 || index >= PHRASE_COUNT) return ESP_ERR_INVALID_ARG;
    if (index == wakeword_get_phrase()) return ESP_OK;
    if (!s_task) {
        /* Engine not built yet: switch directly, it starts with this model. */
        s_phrase = index;
        s_threshold = load_threshold(index);
        s_internal_cost = 0;
        nvs_put_u8(NVS_KEY_PHRASE, (uint8_t)index);
        ESP_LOGI(TAG, "wake: phrase \"%s\" (%s %s)", k_phrases[index].phrase,
                 engine_name(k_phrases[index].engine), k_phrases[index].model);
        return ESP_OK;
    }
    s_phrase_pending = index;
    xTaskNotifyGive(s_task);
    return ESP_OK;
}

esp_err_t wakeword_set_phrase_id(const char *id)
{
    for (int i = 0; id && i < PHRASE_COUNT; i++) {
        if (!strcasecmp(id, k_phrases[i].id)) return wakeword_set_phrase(i);
    }
    return ESP_ERR_INVALID_ARG;
}

void wakeword_log_phrases(void)
{
    for (int i = 0; i < PHRASE_COUNT; i++) {
        ESP_LOGI(TAG, "wake phrase %s%s: \"%s\" (%s %s, default thr %.3f)", k_phrases[i].id,
                 i == wakeword_get_phrase() ? " [selected]" : "", k_phrases[i].phrase,
                 engine_name(k_phrases[i].engine), k_phrases[i].model, k_phrases[i].default_thr);
    }
}

static void log_opts(void)
{
    char text[96];
    size_t n = 0;
    text[0] = '\0';
    for (size_t i = 0; i < sizeof(k_opts) / sizeof(k_opts[0]); i++) {
        if (s_opt & k_opts[i].bit) {
            n += (size_t)snprintf(text + n, sizeof(text) - n, "%s%s", n ? " " : "", k_opts[i].name);
            if (n >= sizeof(text)) break;
        }
    }
    ESP_LOGI(TAG, "wake: opt=0x%02x [%s] (istack/core0 apply when the task is created: "
             "set them before KBX wake on, after a reboot)", (unsigned)s_opt, text[0] ? text : "none");
}

esp_err_t wakeword_set_opt(const char *name, bool on)
{
    if (name && !strcmp(name, "clear")) {
        s_opt = 0;
    } else {
        uint32_t bit = 0;
        for (size_t i = 0; name && i < sizeof(k_opts) / sizeof(k_opts[0]); i++) {
            if (!strcmp(name, k_opts[i].name)) bit = k_opts[i].bit;
        }
        if (!bit) {
            ESP_LOGW(TAG, "wake opt: istack|noext|noarm|mode90|copy|nofloor|core0 on|off, or clear");
            return ESP_ERR_INVALID_ARG;
        }
        s_opt = on ? (s_opt | bit) : (s_opt & ~bit);
    }
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u32(h, NVS_KEY_OPT, s_opt);
        nvs_commit(h);
        nvs_close(h);
    }
    log_opts();
    return ESP_OK;
}

void wakeword_log_opts(void)
{
    log_opts();
}

bool wakeword_phrase_male_voice(int index)
{
    return index >= 0 && index < PHRASE_COUNT && k_phrases[index].male_voice;
}

esp_err_t wakeword_set_mww_gain(unsigned gain)
{
    if (gain == 0) gain = MWW_GAIN_DEFAULT;
    if (gain > 16) return ESP_ERR_INVALID_ARG;
    s_mww_gain = (uint8_t)gain;
    nvs_put_u8(NVS_KEY_MWW_GAIN, (uint8_t)gain);
    const bool rebuild = s_task && k_phrases[s_phrase].engine == WAKE_ENGINE_MWW;
    ESP_LOGI(TAG, "wake: microWakeWord input gain x%u%s", gain, rebuild ? ", engine restarts" : "");
    if (rebuild) {
        /* Same phrase, rebuilt through the normal start path. */
        s_phrase_pending = s_phrase;
        xTaskNotifyGive(s_task);
    }
    return ESP_OK;
}

unsigned wakeword_mww_gain(void)
{
    return s_mww_gain;
}
