#include "c6_wake.h"

#include <string.h>
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"

#include "c6_audio.h"
#include "c6_voice.h"
#include "cloud_auth.h"
#include "net_wifi.h"
#include "wakeword_mww.h"

static const char *TAG = "c6_wake";

/* Embedded from firmware/models/mww by main/CMakeLists.txt, 16-byte aligned. */
extern const uint8_t kbx_mww_kombify_start[], kbx_mww_kombify_end[];
extern const uint8_t kbx_mww_hey_kombify_start[], kbx_mww_hey_kombify_end[];
extern const uint8_t kbx_mww_hey_kubi_start[], kbx_mww_hey_kubi_end[];
extern const uint8_t kbx_mww_hey_jarvis_start[], kbx_mww_hey_jarvis_end[];

typedef struct {
    const char *id;
    const char *name;
    const uint8_t *start;
    const uint8_t *end;
    size_t arena;
    int window;
    float cutoff;
    /* Stage one of the two-stage wake word: the cutoff used once the server
     * verifies candidates. */
    float candidate;
} phrase_t;

/* Arena sizes, windows and cutoffs are the manifests' values, as on the S3
 * Box (firmware/main/wakeword.c k_phrases). */
static const phrase_t K[C6_WAKE_COUNT] = {
    [C6_WAKE_KOMBIFY] = { "kombify", "Kombify", kbx_mww_kombify_start, kbx_mww_kombify_end, 28344, 5, 0.994f, 0.5f },
    [C6_WAKE_HEY_KOMBIFY] = { "hey_kombify", "Hey Kombify", kbx_mww_hey_kombify_start, kbx_mww_hey_kombify_end,
                              28344, 5, 0.98f, 0.5f },
    [C6_WAKE_HEY_KUBI] = { "hey_kubi", "Hey Kubi", kbx_mww_hey_kubi_start, kbx_mww_hey_kubi_end, 28344, 5, 0.998f, 0.5f },
    [C6_WAKE_HEY_JARVIS] = { "hey_jarvis", "Hey Jarvis", kbx_mww_hey_jarvis_start, kbx_mww_hey_jarvis_end, 22860,
                             5, 0.97f, 0.5f },
};

#define FRAME 320                     /* 20 ms at 16 kHz */
/* Hey Jarvis until the kombify models recognise real voices: on-device and
 * PC scoring (2026-10-22) showed the own models reacting to their training
 * TTS only, while the stock model triggers on unseen voices. */
#define DEFAULT_PHRASE C6_WAKE_HEY_JARVIS
#define RETRY_US ((int64_t)30 * 1000 * 1000)
/* At most this many low-cutoff candidates per hour; beyond it the engine
 * falls back to the manifest cutoff until the hour has passed. */
#define CANDIDATES_PER_HOUR 60
#define HOUR_US ((int64_t)3600 * 1000 * 1000)

static volatile int s_phrase = DEFAULT_PHRASE;
static volatile int s_pause;
static volatile bool s_parked = true;
static volatile bool s_reload;
static volatile bool s_listening;
static mww_t *s_mww;
static bool s_mic;                    /* this task opened the microphone */
static int64_t s_retry_us;
static uint32_t s_invokes, s_hits, s_max_us;
static uint64_t s_total_us;
static float s_max_p;
static int16_t s_pcm[FRAME];
static bool s_low_cutoff;
static float s_cand_override, s_manifest_override;   /* 0 = the phrase table */
static int64_t s_window_us;
static uint32_t s_window_candidates;

static float cutoff_now(void)
{
    if (s_low_cutoff) return s_cand_override > 0 ? s_cand_override : K[s_phrase].candidate;
    return s_manifest_override > 0 ? s_manifest_override : K[s_phrase].cutoff;
}

/* Low cutoff only where the server checks candidates and the hourly budget
 * allows; otherwise the manifest cutoff decides alone, as before. */
static void apply_cutoff(void)
{
    int64_t now = esp_timer_get_time();
    if (now - s_window_us > HOUR_US) {
        s_window_us = now;
        s_window_candidates = 0;
    }
    bool low = c6_voice_server_verifies() && s_window_candidates < CANDIDATES_PER_HOUR;
    if (low != s_low_cutoff || !s_mww) {
        s_low_cutoff = low;
        float cutoff = cutoff_now();
        if (s_mww) mww_set_cutoff(s_mww, cutoff);
        ESP_LOGI(TAG, "%s cutoff %.3f (%s)", K[s_phrase].name, (double)cutoff, low ? "server verifies" : "local only");
    }
}

static bool wanted(void)
{
    if (s_phrase < 0 || s_pause > 0 || s_reload) return false;
    if (c6_voice_state() != C6_VOICE_IDLE) return false;
    cloud_auth_status_t st;
    cloud_auth_get_status(&st);
    return st.paired && !st.pairing && net_wifi_is_connected(NULL, 0);
}

static void park(void)
{
    s_listening = false;
    if (s_mic) {
        c6_audio_mic_close();
        s_mic = false;
    }
    if (s_mww) {
        mww_destroy(s_mww);
        s_mww = NULL;
    }
    s_reload = false;
    s_parked = true;
}

static bool load(void)
{
    const phrase_t *p = &K[s_phrase];
    mww_model_t md = { .model = p->start, .model_size = (size_t)(p->end - p->start), .arena_size = p->arena,
                       .window = p->window };
    size_t before = esp_get_free_heap_size();
    int64_t t0 = esp_timer_get_time();
    esp_err_t err = mww_create(&md, s_manifest_override > 0 ? s_manifest_override : p->cutoff, &s_mww);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "%s: engine not built (%s, heap %u, largest %u); retry in 30 s", p->name, esp_err_to_name(err),
                 (unsigned)esp_get_free_heap_size(), (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
        s_retry_us = esp_timer_get_time() + RETRY_US;
        return false;
    }
    ESP_LOGI(TAG, "%s ready in %d ms: arena %u/%u B, engine %u B, heap %u", p->name,
             (int)((esp_timer_get_time() - t0) / 1000), (unsigned)mww_arena_used(s_mww),
             (unsigned)mww_arena_size(s_mww), (unsigned)(before - esp_get_free_heap_size()),
             (unsigned)esp_get_free_heap_size());
    return true;
}

static void wake_task(void *arg)
{
    (void)arg;
    for (;;) {
        if (!wanted()) {
            if (!s_parked || s_reload) park();
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }
        if (esp_timer_get_time() < s_retry_us) {
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }
        s_parked = false;
        if (!s_mww) {
            if (!load()) {
                park();
                continue;
            }
            s_low_cutoff = !c6_voice_server_verifies();  /* force apply_cutoff to set it */
        }
        apply_cutoff();
        if (!s_mic) {
            if (c6_audio_mic_open() != ESP_OK) {
                vTaskDelay(pdMS_TO_TICKS(100));
                continue;
            }
            s_mic = true;
            mww_reset(s_mww);
        }
        s_listening = true;
        if (c6_audio_mic_read(s_pcm, FRAME) != ESP_OK) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        StreamBufferHandle_t inject = c6_voice_inject_stream();
        if (inject) {
            /* KBX inject ... wake: the test clip replaces the microphone. */
            size_t got = 0;
            /* Whole frames: a short read padded with zeros would chop the clip. */
            for (int i = 0; i < 10 && got < sizeof(s_pcm); ++i)
                got += xStreamBufferReceive(inject, (uint8_t *)s_pcm + got, sizeof(s_pcm) - got, pdMS_TO_TICKS(20));
            if (got < sizeof(s_pcm)) memset((uint8_t *)s_pcm + got, 0, sizeof(s_pcm) - got);
        }
        c6_voice_preroll_push(s_pcm);
        bool hit = false;
        int64_t t0 = esp_timer_get_time();
        int invokes = mww_feed(s_mww, s_pcm, FRAME, &hit);
        uint32_t us = (uint32_t)(esp_timer_get_time() - t0);
        if (invokes > 0) {
            float p = mww_last_probability(s_mww);
            if (p > s_max_p) s_max_p = p;
            s_invokes += (uint32_t)invokes;
            s_total_us += us;
            if (us > s_max_us) s_max_us = us;
        }
        if (hit) {
            s_hits++;
            if (s_low_cutoff) s_window_candidates++;
            float mean = mww_hit_mean(s_mww);
            ESP_LOGI(TAG, "wake word \"%s\" (mean %.3f, %s)", K[s_phrase].name, mean,
                     s_low_cutoff ? "candidate for the server" : "local cutoff");
            /* Hand the open microphone straight to the turn's capture so
             * the words right after the phrase are kept. */
            s_mic = false;
            s_listening = false;
            c6_voice_wake(K[s_phrase].id, mean);
        }
    }
}

static void load_phrase(void)
{
    nvs_handle_t h;
    if (nvs_open("c6_wake", NVS_READONLY, &h) != ESP_OK) return;
    int8_t v = DEFAULT_PHRASE;
    if (nvs_get_i8(h, "phrase", &v) == ESP_OK && v >= C6_WAKE_OFF && v < C6_WAKE_COUNT) s_phrase = v;
    nvs_close(h);
}

static void load_cutoffs(void)
{
    nvs_handle_t h;
    if (nvs_open("c6_wake", NVS_READONLY, &h) != ESP_OK) return;
    /* A stored value outside 0.3 .. 1.0 (a damaged entry; the setter never
     * stores more than 1.0) is ignored, so a reboot cannot leave detection dead. */
    uint16_t v;
    if (nvs_get_u16(h, "cand", &v) == ESP_OK && v >= 300 && v <= 1000) s_cand_override = v / 1000.0f;
    if (nvs_get_u16(h, "cut", &v) == ESP_OK && v >= 300 && v <= 1000) s_manifest_override = v / 1000.0f;
    nvs_close(h);
}

static bool set_cutoff(const char *key, float *slot, float cutoff)
{
    /* NaN fails every comparison, so it is rejected here. */
    if (!(cutoff == 0.0f || (cutoff >= 0.3f && cutoff <= 1.1f))) return false;
    *slot = cutoff;
    /* Above 1.0 nothing can hit: a measurement mode that lasts until the next
     * reboot and is never stored. */
    if (cutoff <= 1.0f) {
        nvs_handle_t h;
        if (nvs_open("c6_wake", NVS_READWRITE, &h) == ESP_OK) {
            if (cutoff > 0) nvs_set_u16(h, key, (uint16_t)(cutoff * 1000 + 0.5f));
            else nvs_erase_key(h, key);
            nvs_commit(h);
            nvs_close(h);
        }
    }
    s_reload = true;
    s_retry_us = 0;
    return true;
}

bool c6_wake_set_candidate_cutoff(float cutoff)
{
    return set_cutoff("cand", &s_cand_override, cutoff);
}

bool c6_wake_set_manifest_cutoff(float cutoff)
{
    return set_cutoff("cut", &s_manifest_override, cutoff);
}

esp_err_t c6_wake_init(void)
{
    load_phrase();
    load_cutoffs();
    /* Above LVGL (4): at priority 3 the orb animation starved the engine and
     * a tap waited 1.5 s for it to release the microphone. Inference takes
     * about 6 ms per 30 ms, so the screen keeps most of the CPU. */
    return xTaskCreate(wake_task, "c6_wake", 8192, NULL, 5, NULL) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

void c6_wake_set_phrase(c6_wake_phrase_t phrase)
{
    if (phrase < C6_WAKE_OFF || phrase >= C6_WAKE_COUNT) return;
    s_phrase = phrase;
    s_reload = true;
    s_retry_us = 0;
    nvs_handle_t h;
    if (nvs_open("c6_wake", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_i8(h, "phrase", (int8_t)phrase);
        nvs_commit(h);
        nvs_close(h);
    }
}

c6_wake_phrase_t c6_wake_phrase(void)
{
    return (c6_wake_phrase_t)s_phrase;
}

const char *c6_wake_phrase_name(c6_wake_phrase_t phrase)
{
    return phrase >= 0 && phrase < C6_WAKE_COUNT ? K[phrase].name : NULL;
}

void c6_wake_pause(void)
{
    s_pause++;
    /* The caller opens the microphone next; the engine must have closed its
     * handle first, or it would close it under the new reader. */
    int64_t t0 = esp_timer_get_time();
    while (!s_parked) vTaskDelay(pdMS_TO_TICKS(5));
    int ms = (int)((esp_timer_get_time() - t0) / 1000);
    if (ms > 100) ESP_LOGW(TAG, "pause waited %d ms", ms);
}

void c6_wake_resume(void)
{
    if (s_pause > 0) s_pause--;
}

bool c6_wake_listening(void)
{
    return s_listening;
}

void c6_wake_log_status(void)
{
    ESP_LOGI(TAG, "wake: phrase=%s listening=%d paused=%d hits=%u invokes=%u avg=%u us max=%u us max_p=%.3f "
             "cutoff=%.3f candidate=%.3f active=%.3f (%s) candidates_this_hour=%u",
             s_phrase >= 0 ? K[s_phrase].id : "off", s_listening, s_pause, (unsigned)s_hits, (unsigned)s_invokes,
             s_invokes ? (unsigned)(s_total_us / s_invokes) : 0, (unsigned)s_max_us,
             s_max_p, s_phrase >= 0 ? (double)(s_manifest_override > 0 ? s_manifest_override : K[s_phrase].cutoff) : 0.0,
             s_phrase >= 0 ? (double)(s_cand_override > 0 ? s_cand_override : K[s_phrase].candidate) : 0.0,
             s_phrase >= 0 ? (double)cutoff_now() : 0.0, s_low_cutoff ? "candidate" : "manifest",
             (unsigned)s_window_candidates);
    s_max_p = 0;
}
