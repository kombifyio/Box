/*
 * kombify box - audio path: I2S mic -> USB (UAC in), USB (UAC out) -> I2S codec/DAC
 */
#include <string.h>
#include <math.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/ringbuf.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/idf_additions.h"
#include "driver/gpio.h"
#include "driver/i2s_std.h"
#include "driver/i2s_tdm.h"
#include "esp_err.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs.h"
#include "usb_device_uac.h"
#include "sdkconfig.h"
#include "board.h"
#include "aec_stage.h"
#include "audio.h"
#include "es8311_min.h"
#include "es7210_min.h"
#include "system_i2c.h"

static const char *TAG = "kbx_audio";

// Protects read-modify-write on s_volume and the check-then-set of
// s_test_tone_running against concurrent callers (USB task, LVGL task,
// touch task) — dual-core safe (portMUX is a cross-core spinlock).
static portMUX_TYPE s_audio_mux = portMUX_INITIALIZER_UNLOCKED;

static i2s_chan_handle_t s_tx = NULL;  // speaker
static i2s_chan_handle_t s_rx = NULL;  // mic

static volatile uint32_t s_volume = 70;      // 0..100 from host
static volatile bool     s_muted = false;    // host mute
static volatile bool     s_local_mute = false; // box-local mute (touch toggle)
static uint32_t          s_mute_epoch = 1;

/* Volume and box-local mute survive a power cycle (mute is a privacy promise).
 * Writes are debounced: every change re-arms a one-shot timer and the NVS
 * commit happens once the value has been stable for AUDIO_PERSIST_DEBOUNCE_US. */
#define AUDIO_NVS_NAMESPACE "audio"
#define AUDIO_NVS_KEY_VOLUME "vol"
#define AUDIO_NVS_KEY_LMUTE "lmute"
#define AUDIO_PERSIST_DEBOUNCE_US (1000 * 1000)
static esp_timer_handle_t s_persist_timer = NULL;

static void audio_persist_flush(void *arg)
{
    (void)arg;
    taskENTER_CRITICAL(&s_audio_mux);
    const uint8_t vol = (uint8_t)s_volume;
    const uint8_t lmute = s_local_mute ? 1 : 0;
    taskEXIT_CRITICAL(&s_audio_mux);
    nvs_handle_t nvs;
    if (nvs_open(AUDIO_NVS_NAMESPACE, NVS_READWRITE, &nvs) != ESP_OK) {
        ESP_LOGW(TAG, "persist: nvs open failed");
        return;
    }
    esp_err_t err = nvs_set_u8(nvs, AUDIO_NVS_KEY_VOLUME, vol);
    if (err == ESP_OK) {
        err = nvs_set_u8(nvs, AUDIO_NVS_KEY_LMUTE, lmute);
    }
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    nvs_close(nvs);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "persist: nvs write failed: %s", esp_err_to_name(err));
    }
}

static void audio_persist_schedule(void)
{
    if (s_persist_timer == NULL) {
        return;  // before audio_init finished restoring: nothing to save yet
    }
    (void)esp_timer_stop(s_persist_timer);
    (void)esp_timer_start_once(s_persist_timer, AUDIO_PERSIST_DEBOUNCE_US);
}

static void audio_persist_restore(void)
{
    nvs_handle_t nvs;
    if (nvs_open(AUDIO_NVS_NAMESPACE, NVS_READONLY, &nvs) != ESP_OK) {
        return;  // first boot: keep defaults
    }
    uint8_t vol = 0;
    uint8_t lmute = 0;
    if (nvs_get_u8(nvs, AUDIO_NVS_KEY_VOLUME, &vol) == ESP_OK && vol <= 100) {
        s_volume = vol;
    }
    if (nvs_get_u8(nvs, AUDIO_NVS_KEY_LMUTE, &lmute) == ESP_OK) {
        s_local_mute = lmute != 0;
    }
    nvs_close(nvs);
}
static volatile int64_t  s_last_spk_us = 0;  // last time real audio played
static volatile uint32_t s_mic_level = 0;    // 0..100 crude VU
static volatile uint32_t s_mic_peak_left = 0;
static volatile uint32_t s_mic_peak_right = 0;
static volatile uint32_t s_spk_level = 0;    // 0..100 crude VU
static RingbufHandle_t s_spk_rb = NULL;
static volatile uint32_t s_spk_drop_count = 0;
static RingbufHandle_t s_agent_rb = NULL;
/* Serializes ring/EOF state, never I2S writes or sleeps. Flush is synchronous
 * so an old queued flush cannot discard the next stream's PCM or EOF. */
static SemaphoreHandle_t s_agent_lock = NULL;
static bool s_agent_complete = false;
static uint32_t s_agent_epoch = 0;
static volatile bool s_agent_holdoff = false;
static volatile bool s_agent_draining = false;
static volatile uint32_t s_agent_drops = 0;
static volatile uint32_t s_agent_underruns = 0;
/* SpeechKit's live-agent wire contract, including the deployed Deepgram
 * v0.77.1 provider, emits 24 kHz mono PCM. */
static volatile uint32_t s_agent_src_hz = 24000;
static SemaphoreHandle_t s_i2s_lock = NULL;
/* Ducking (audio_duck): the reply plays 12 dB lower until this time, so a
 * spoken "Stop" stands out of the echo residue (stop_word.c). */
static volatile int64_t s_duck_until_us;

static esp_err_t s_es7210_status = ESP_ERR_INVALID_STATE;
static uint8_t s_es7210_addr = 0;
static esp_err_t s_es8311_status = ESP_ERR_INVALID_STATE;
static uint8_t s_es8311_addr = 0;
static bool s_pa_enabled = false;

#define SPK_DIGITAL_GAIN_PERCENT 85
/* ES8311 DAC volume (reg 0x32 = volume * 256 / 100 - 1; 75 = 0 dB, each
 * step 0.5 dB). Per board: the PA and speaker differ (board_*.h). */
#ifdef KBX_SPK_CODEC_VOLUME
#define SPK_CODEC_VOLUME KBX_SPK_CODEC_VOLUME
#else
#define SPK_CODEC_VOLUME 60
#endif
#define SPK_ACTIVE_THRESHOLD 32
#define SPK_VU_MULTIPLIER_PERCENT 100
#define SPK_VU_DIVISOR 328
#define SPK_RINGBUF_BYTES (12 * 1024)

typedef struct {
    bool muted;
    bool host_muted;
    bool local_muted;
    uint32_t epoch;
} audio_mute_snapshot_t;

static uint32_t audio_next_mute_epoch(uint32_t epoch)
{
    epoch++;
    return epoch == 0 ? 1 : epoch;
}

static audio_mute_snapshot_t audio_mute_snapshot(void)
{
    taskENTER_CRITICAL(&s_audio_mux);
    const audio_mute_snapshot_t snapshot = {
        .muted = s_muted || s_local_mute,
        .host_muted = s_muted,
        .local_muted = s_local_mute,
        .epoch = s_mute_epoch,
    };
    taskEXIT_CRITICAL(&s_audio_mux);
    return snapshot;
}

static void audio_set_mute_state(volatile bool *state, bool muted)
{
    taskENTER_CRITICAL(&s_audio_mux);
    if (*state != muted) {
        *state = muted;
        s_mute_epoch = audio_next_mute_epoch(s_mute_epoch);
    }
    taskEXIT_CRITICAL(&s_audio_mux);
}

bool audio_is_speaking(void)  { return (esp_timer_get_time() - s_last_spk_us) < 400000; }
bool audio_is_muted(void)     { return audio_mute_snapshot().muted; }
uint32_t audio_mic_level(void){ return s_mic_level; }
uint32_t audio_speaker_level(void) { return audio_is_speaking() ? s_spk_level : 0; }
uint32_t audio_volume(void)   { return s_volume; }
bool audio_speaker_ready(void)
{
    return s_es8311_status == ESP_OK;
}

void audio_set_local_mute(bool muted) { audio_set_mute_state(&s_local_mute, muted); audio_persist_schedule(); }
void audio_toggle_local_mute(void)
{
    taskENTER_CRITICAL(&s_audio_mux);
    s_local_mute = !s_local_mute;
    s_mute_epoch = audio_next_mute_epoch(s_mute_epoch);
    taskEXIT_CRITICAL(&s_audio_mux);
    audio_persist_schedule();
}
bool audio_local_mute(void) { return audio_mute_snapshot().local_muted; }

uint32_t audio_adjust_volume(int delta)
{
    taskENTER_CRITICAL(&s_audio_mux);
    int v = (int)s_volume + delta;
    if (v < 0) {
        v = 0;
    } else if (v > 100) {
        v = 100;
    }
    s_volume = (uint32_t)v;
    uint32_t out = s_volume;
    taskEXIT_CRITICAL(&s_audio_mux);
    audio_persist_schedule();
    return out;
}

uint32_t audio_set_volume(uint32_t volume)
{
    if (volume > 100) {
        volume = 100;
    }
    s_volume = volume;  // single aligned store: atomic, no RMW
    audio_persist_schedule();
    return volume;
}

static void audio_set_pa(bool enabled)
{
    if (s_pa_enabled == enabled) {
        return;
    }
    gpio_set_level(AUDIO_PA_CTRL, enabled ? 1 : 0);
    s_pa_enabled = enabled;
}

/* Shared speaker submit for USB host audio AND agent downlink:
 * volume/soft-clip in place, VU/speaking detection, PA enable, then into
 * s_spk_rb. wait_ticks=0 = drop-oldest (USB path, host clocks); >0 = blocking
 * (agent path clocks itself from I2S drain). agent_vol=true ignores host
 * mute (local mute still applies). */
static bool speaker_process_and_queue(int16_t *s, size_t n, TickType_t wait_ticks,
                                      bool agent_vol)
{
    if (!s_tx || !s_spk_rb || n == 0) {
        return false;
    }
    size_t len = n * sizeof(int16_t);
    bool active = false;
    uint32_t peak = 0;
    /* apply volume (simple linear) + detect activity */
    uint32_t vol = (agent_vol ? s_local_mute : s_muted) ? 0 : s_volume;
    if (vol > 100) {
        vol = 100;
    }
    if (agent_vol && esp_timer_get_time() < s_duck_until_us) vol /= 4;   /* -12 dB */
    for (size_t i = 0; i < n; i++) {
        int32_t v = ((int32_t)s[i] * (int32_t)vol * SPK_DIGITAL_GAIN_PERCENT) / 10000;
        if (v > 32767) {
            v = 32767;
        } else if (v < -32768) {
            v = -32768;
        }
        s[i] = (int16_t)v;
        uint32_t av = (uint32_t)(v < 0 ? -v : v);
        if (av > peak) peak = av;
        if (av > SPK_ACTIVE_THRESHOLD) active = true;
    }
    if (active) {
        s_last_spk_us = esp_timer_get_time();
        uint32_t lvl = (peak * SPK_VU_MULTIPLIER_PERCENT) / (SPK_VU_DIVISOR * 100);
        if (lvl > 100) lvl = 100;
        uint32_t cur = s_spk_level;
        s_spk_level = (lvl > cur) ? lvl : (cur - (cur - lvl) / 3);
    }
    if (s_es8311_status == ESP_OK) {
        if (active) {
            audio_set_pa(true);
        }
    }
    if (xRingbufferSend(s_spk_rb, s, len, wait_ticks) != pdTRUE) {
        size_t old_len = 0;
        void *old = xRingbufferReceive(s_spk_rb, &old_len, 0);
        if (old) {
            vRingbufferReturnItem(s_spk_rb, old);
        }
        if (xRingbufferSend(s_spk_rb, s, len, 0) != pdTRUE) {
            s_spk_drop_count++;
            return active;
        }
        s_spk_drop_count++;
    }
    return active;
}

static void speaker_apply_gain(int16_t *s, size_t n, bool agent_vol)
{
    uint32_t vol = (agent_vol ? s_local_mute : s_muted) ? 0 : s_volume;
    if (vol > 100) {
        vol = 100;
    }
    if (agent_vol && esp_timer_get_time() < s_duck_until_us) vol /= 4;   /* -12 dB */
    bool active = false;
    uint32_t peak = 0;
    for (size_t i = 0; i < n; i++) {
        int32_t v = ((int32_t)s[i] * (int32_t)vol * SPK_DIGITAL_GAIN_PERCENT) / 10000;
        if (v > 32767) {
            v = 32767;
        } else if (v < -32768) {
            v = -32768;
        }
        s[i] = (int16_t)v;
        uint32_t av = (uint32_t)(v < 0 ? -v : v);
        if (av > peak) {
            peak = av;
        }
        if (av > SPK_ACTIVE_THRESHOLD) {
            active = true;
        }
    }
    if (active) {
        s_last_spk_us = esp_timer_get_time();
        uint32_t lvl = (peak * SPK_VU_MULTIPLIER_PERCENT) / (SPK_VU_DIVISOR * 100);
        if (lvl > 100) {
            lvl = 100;
        }
        uint32_t cur = s_spk_level;
        s_spk_level = (lvl > cur) ? lvl : (cur - (cur - lvl) / 3);
        if (s_es8311_status == ESP_OK) {
            audio_set_pa(true);
        }
    }
}

static void speaker_i2s_write(const int16_t *s, size_t n)
{
    if (!s_tx || !s || n == 0) {
        return;
    }
    const uint8_t *p = (const uint8_t *)s;
    size_t left = n * sizeof(int16_t);
    if (s_i2s_lock) {
        xSemaphoreTake(s_i2s_lock, portMAX_DELAY);
    }
    int tries = 0;
    while (left > 0) {
        size_t written = 0;
        esp_err_t err = i2s_channel_write(s_tx, p, left, &written, pdMS_TO_TICKS(200));
        if (err != ESP_OK || written == 0) {
            tries++;
            if (tries >= 5) {
                s_spk_drop_count++;
                ESP_LOGW(TAG, "i2s write drop left=%u drops=%u",
                         (unsigned)left, (unsigned)s_spk_drop_count);
                break;
            }
            continue;
        }
        tries = 0;
        p += written;
        left -= written;
    }
    if (s_i2s_lock) {
        xSemaphoreGive(s_i2s_lock);
    }
}

static void speaker_ring_flush(void)
{
    if (!s_spk_rb) {
        return;
    }
    size_t len = 0;
    void *it;
    while ((it = xRingbufferReceive(s_spk_rb, &len, 0)) != NULL) {
        vRingbufferReturnItem(s_spk_rb, it);
    }
}

static bool agent_playback_active(void)
{
    if (s_agent_draining) {
        return true;
    }
    if (!s_agent_rb) {
        return false;
    }
    UBaseType_t waiting = 0;
    vRingbufferGetInfo(s_agent_rb, NULL, NULL, NULL, NULL, &waiting);
    return waiting > 0;
}

static esp_err_t uac_output_cb(uint8_t *buf, size_t len, void *ctx)
{
    (void)ctx;
    if (len == 0) {
        return ESP_OK;
    }
    /* USB host keeps sending speaker frames while the box is the Windows
     * playback device. Those frames share s_spk_rb with the agent downlink
     * and drop it (12 KB ring, wait_ticks=0). Mute the host path while a
     * cloud turn is playing. */
    if (agent_playback_active()) {
        return ESP_OK;
    }
    speaker_process_and_queue((int16_t *)buf, len / 2, 0, false);
    return ESP_OK;
}

static void spk_writer_task(void *arg)
{
    (void)arg;
    while (1) {
        if (s_agent_draining) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        size_t len = 0;
        uint8_t *data = (uint8_t *)xRingbufferReceive(s_spk_rb, &len, pdMS_TO_TICKS(20));
        if (!data) {
            if (s_es8311_status == ESP_OK && (esp_timer_get_time() - s_last_spk_us) > 600000) {
                audio_set_pa(false);
            }
            continue;
        }
        if (s_i2s_lock) {
            xSemaphoreTake(s_i2s_lock, portMAX_DELAY);
        }
        size_t off = 0;
        while (off < len) {
            size_t written = 0;
            esp_err_t err = i2s_channel_write(s_tx, data + off, len - off, &written, pdMS_TO_TICKS(20));
            if (err != ESP_OK || written == 0) {
                s_spk_drop_count++;
                break;
            }
            off += written;
        }
        if (s_i2s_lock) {
            xSemaphoreGive(s_i2s_lock);
        }
        vRingbufferReturnItem(s_spk_rb, data);
    }
}

/* Agent downlink: S16 mono from the voice-agent WS -> 48 kHz stereo I2S.
 * Default source is the SpeechKit 24 kHz wire contract (2x upsample).
 * CDC `KBX spkrate 16|24` offers an operator override. Direct I2S, not the
 * 12 KB USB speaker ring. */
#define AGENT_RING_BYTES      (256 * 1024)
#define AGENT_PREBUFFER_BYTES 32000          /* 1.0 s @ 16 kHz / 0.67 s @ 24 kHz */
#define AGENT_CHUNK_SAMPLES   480            /* 30 ms @ 16 kHz / 20 ms @ 24 kHz */
#define AGENT_CHUNK_BYTES     (AGENT_CHUNK_SAMPLES * (int)sizeof(int16_t))
#define AGENT_UPSAMPLE_MAX    3

static void agent_spk_task(void *arg)
{
    (void)arg;
    static int16_t mono48[AGENT_CHUNK_SAMPLES * AGENT_UPSAMPLE_MAX];
    static int16_t stereo[AGENT_CHUNK_SAMPLES * AGENT_UPSAMPLE_MAX * 2];
    int16_t prev = 0;
    bool draining = false;
    int underruns = 0;
    uint32_t epoch = 0;
    for (;;) {
        xSemaphoreTake(s_agent_lock, portMAX_DELAY);
        uint32_t current = __atomic_load_n(&s_agent_epoch, __ATOMIC_ACQUIRE);
        if (epoch != current) {
            epoch = current;
            draining = false;
            s_agent_draining = false;
            underruns = 0;
            prev = 0;
        }
        if (!draining) {
            UBaseType_t waiting = 0;
            vRingbufferGetInfo(s_agent_rb, NULL, NULL, NULL, NULL, &waiting);
            if (waiting >= AGENT_PREBUFFER_BYTES || (s_agent_complete && waiting > 0)) {
                draining = true;
                s_agent_draining = true;
                underruns = 0;
                prev = 0;
                xSemaphoreGive(s_agent_lock);
                speaker_ring_flush();
                if (s_es8311_status == ESP_OK) {
                    audio_set_pa(true);
                    vTaskDelay(pdMS_TO_TICKS(15));
                }
                continue;
            }
            xSemaphoreGive(s_agent_lock);
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        size_t got = 0;
        int16_t *in = (int16_t *)xRingbufferReceiveUpTo(s_agent_rb, &got,
                                                        0, AGENT_CHUNK_BYTES);
        if (!in) {
            bool complete = s_agent_complete;
            if (complete) {
                /* EOF after the last I2S write is normal, including an answer
                 * too short to reach the initial jitter prebuffer. */
                draining = false;
                s_agent_draining = false;
                underruns = 0;
                prev = 0;
            } else {
                underruns++;
                s_agent_underruns++;
                if (underruns == 1 || (underruns % 8) == 0) {
                    ESP_LOGW(TAG, "agent underrun n=%d src=%u", underruns,
                             (unsigned)s_agent_src_hz);
                }
                if (underruns >= 25) {
                    draining = false;
                    s_agent_draining = false;
                    underruns = 0;
                }
            }
            xSemaphoreGive(s_agent_lock);
            vTaskDelay(pdMS_TO_TICKS(complete ? 20 : 40));
            continue;
        }
        underruns = 0;
        size_t n_in = got / sizeof(int16_t);
        uint32_t src = s_agent_src_hz;
        int factor = (src == 16000) ? 3 : 2;
        if (n_in > AGENT_CHUNK_SAMPLES) n_in = AGENT_CHUNK_SAMPLES;
        size_t n48 = (factor == 3)
            ? pcm_dsp_upsample_3x(in, n_in, mono48, &prev)
            : pcm_dsp_upsample_2x(in, n_in, mono48, &prev);
        vRingbufferReturnItem(s_agent_rb, in);
        xSemaphoreGive(s_agent_lock);
        for (size_t i = 0; i < n48; i++) {
            stereo[2 * i] = mono48[i];
            stereo[2 * i + 1] = mono48[i];
        }
        /* A flush can revoke this selected chunk before I2S begins. The
         * hardware write itself stays outside the ring/control mutex. */
        if (__atomic_load_n(&s_agent_epoch, __ATOMIC_ACQUIRE) != epoch) continue;
        speaker_apply_gain(stereo, n48 * 2, true);
        speaker_i2s_write(stereo, n48 * 2);
    }
}

esp_err_t audio_play_agent_24k(const int16_t *pcm, size_t samples)
{
    if (!s_agent_rb || !s_agent_lock) return ESP_ERR_INVALID_STATE;
    if (!pcm || samples == 0) return ESP_ERR_INVALID_ARG;
    TickType_t started = xTaskGetTickCount();
    uint32_t epoch = __atomic_load_n(&s_agent_epoch, __ATOMIC_ACQUIRE);
    for (;;) {
        xSemaphoreTake(s_agent_lock, portMAX_DELAY);
        if (s_agent_holdoff || epoch != __atomic_load_n(&s_agent_epoch, __ATOMIC_ACQUIRE)) {
            ++s_agent_drops;
            xSemaphoreGive(s_agent_lock);
            return ESP_OK; /* A cancelled chunk must not enter a new stream. */
        }
        bool sent = xRingbufferSend(s_agent_rb, pcm, samples * sizeof(int16_t), 0) == pdTRUE;
        xSemaphoreGive(s_agent_lock);
        if (sent) return ESP_OK;
        if (xTaskGetTickCount() - started >= pdMS_TO_TICKS(50)) {
            ++s_agent_drops;
            ESP_LOGW(TAG, "agent ring drop samples=%u drops=%u",
                     (unsigned)samples, (unsigned)s_agent_drops);
            return ESP_ERR_NO_MEM;
        }
        /* The feeder must acquire the mutex to make room. Never wait for
         * ring space while holding the mutex needed by that consumer. */
        vTaskDelay(pdMS_TO_TICKS(1));
    }
}

static void agent_flush_locked(void)
{
    s_agent_complete = false;
    (void)__atomic_add_fetch(&s_agent_epoch, 1U, __ATOMIC_RELEASE);
    size_t length = 0;
    void *item;
    while ((item = xRingbufferReceiveUpTo(s_agent_rb, &length, 0, AGENT_RING_BYTES)) != NULL)
        vRingbufferReturnItem(s_agent_rb, item);
    s_agent_draining = false;
}

void audio_play_agent_flush(void)
{
    if (!s_agent_rb || !s_agent_lock) return;
    xSemaphoreTake(s_agent_lock, portMAX_DELAY);
    agent_flush_locked();
    xSemaphoreGive(s_agent_lock);
}

void audio_play_agent_complete(void)
{
    if (!s_agent_rb || !s_agent_lock) return;
    xSemaphoreTake(s_agent_lock, portMAX_DELAY);
    if (!s_agent_holdoff) s_agent_complete = true;
    xSemaphoreGive(s_agent_lock);
}

void audio_play_agent_holdoff(void)
{
    if (!s_agent_rb || !s_agent_lock) return;
    xSemaphoreTake(s_agent_lock, portMAX_DELAY);
    s_agent_holdoff = true;
    agent_flush_locked();
    xSemaphoreGive(s_agent_lock);
}

void audio_play_agent_resume(void)
{
    if (!s_agent_lock) return;
    xSemaphoreTake(s_agent_lock, portMAX_DELAY);
    s_agent_holdoff = false;
    xSemaphoreGive(s_agent_lock);
}

bool audio_play_agent_idle(void)
{
    if (!s_agent_rb || !s_agent_lock) return true;
    xSemaphoreTake(s_agent_lock, portMAX_DELAY);
    UBaseType_t waiting = 0;
    vRingbufferGetInfo(s_agent_rb, NULL, NULL, NULL, NULL, &waiting);
    bool idle = waiting == 0 && !s_agent_draining;
    xSemaphoreGive(s_agent_lock);
    return idle;
}

uint32_t audio_play_agent_src_hz(void)
{
    return s_agent_src_hz;
}

esp_err_t audio_play_agent_set_src_hz(uint32_t hz)
{
    if (hz != 16000 && hz != 24000) {
        return ESP_ERR_INVALID_ARG;
    }
    s_agent_src_hz = hz;
    ESP_LOGI(TAG, "agent src=%u Hz upsample=%dx",
             (unsigned)hz, hz == 16000 ? 3 : 2);
    return ESP_OK;
}

void audio_log_play_status(void)
{
    UBaseType_t waiting = 0;
    if (s_agent_rb) {
        vRingbufferGetInfo(s_agent_rb, NULL, NULL, NULL, NULL, &waiting);
    }
    ESP_LOGI(TAG,
             "play: src=%u upsample=%dx draining=%d waiting=%u drops=%u underruns=%u i2s_drops=%u holdoff=%d",
             (unsigned)s_agent_src_hz,
             s_agent_src_hz == 16000 ? 3 : 2,
             (int)s_agent_draining,
             (unsigned)waiting,
             (unsigned)s_agent_drops,
             (unsigned)s_agent_underruns,
             (unsigned)s_spk_drop_count,
             (int)s_agent_holdoff);
}

// Optional extra capture gain after per-buffer DC removal.
#define MIC_GAIN 14
#define MIC_VU_PEAK_DIVISOR 240
#define MIC_DIAG_LOG_INTERVAL_US 5000000

#define CAPTURE_BLOCK_FRAMES 480U
#define CAPTURE_BLOCKS_PER_20MS 2U
/* 480 ms ring in PSRAM (1.9 KB per block, ~92 KB), fixed for the whole
 * run: the low-priority wake-word observer may fall behind briefly without
 * losing audio. The tap and UAC readers keep their dev7 bound of 160 ms
 * (CAPTURE_READER_MAX_LAG_BLOCKS), which is always inside the ring. */
#define CAPTURE_RING_BLOCKS 48U
#define CAPTURE_READER_MAX_LAG_BLOCKS 16U
_Static_assert(CAPTURE_READER_MAX_LAG_BLOCKS <= CAPTURE_RING_BLOCKS,
               "reader lag bound must fit the ring");
#define CAPTURE_BLOCK_FLAG_GAP (1U << 0)
#define CAPTURE_TASK_STACK_BYTES 4096U
#define CAPTURE_TASK_PRIORITY 7U

_Static_assert(UAC_SAMPLE_RATE == PCM_DSP_INPUT_RATE_HZ,
               "capture DSP must match the hardware sample rate");
_Static_assert(CAPTURE_BLOCK_FRAMES * 2U == PCM_DSP_INPUT_FRAMES_20MS,
               "two broker blocks must be exactly 20 ms");

typedef struct {
    volatile uint32_t sequence;
    uint32_t flags;
    size_t frames;
    int16_t stereo[CAPTURE_BLOCK_FRAMES * 2U];
} capture_block_t;

typedef struct {
    bool active;
    bool valid;
    uint32_t next_sequence;
    uint32_t mute_epoch;
    audio_capture_telemetry_t telemetry;
    pcm_dsp_t dsp;
} capture_turn_t;

typedef struct {
    uint64_t energy_left;
    uint64_t energy_right;
    uint32_t peak_left;
    uint32_t peak_right;
    uint32_t output_peak;
    const char *source;
} mic_fold_stats_t;

/* Allocated once at broker start and never moved: the broker (the only
 * writer) still reads its own last slot after releasing the lock. */
static capture_block_t *s_capture_ring = NULL;
static volatile uint32_t s_capture_latest_sequence = 0;
/* Newest block whose microphone slot the AEC task has cleaned; consumers
 * read up to it while the AEC runs (capture_ready_sequence). */
#if CONFIG_KBX_AUDIO_AEC
static volatile uint32_t s_capture_clean_sequence = 0;
static volatile bool s_aec_task_running = false;
#endif
static uint32_t capture_ready_sequence(void);
static uint32_t s_capture_next_sequence = 1;
static uint32_t s_uac_next_sequence = 0;
static volatile uint32_t s_uac_gap_count = 0;
static SemaphoreHandle_t s_capture_ring_lock = NULL;
static SemaphoreHandle_t s_capture_turn_lock = NULL;
static capture_block_t *s_capture_turn_block = NULL;
static int16_t *s_capture_turn_stereo = NULL;
static capture_turn_t s_capture_turn;

/* Echo cancellation (aec_stage.c): on by default where the board's loopback
 * is verified (Kconfig KBX_AEC_DEFAULT_ON). One stage runs in the capture
 * broker on every block, so it is always converged, costs the CPU once and
 * every consumer (uplink, wake word, USB microphone) reads the cleaned
 * microphone from the ring. */
#if CONFIG_KBX_AEC_DEFAULT_ON
static volatile bool s_aec_enabled = true;
#else
static volatile bool s_aec_enabled = false;
#endif
static aec_stage_t *s_broker_aec;

static uint32_t capture_sequence_next(uint32_t sequence)
{
    sequence++;
    return sequence == 0 ? 1 : sequence;
}

static void audio_stereo_peaks(const int16_t *stereo, size_t frames,
                               uint32_t *peak_left, uint32_t *peak_right,
                               int32_t *mean_left, int32_t *mean_right)
{
    int64_t sum_left = 0;
    int64_t sum_right = 0;
    uint32_t count = 0;
    const size_t step = frames > 64 ? 8 : 1;
    for (size_t i = 0; i < frames; i += step) {
        sum_left += stereo[2 * i];
        sum_right += stereo[2 * i + 1];
        count++;
    }
    int32_t dc_left = count ? (int32_t)(sum_left / (int64_t)count) : 0;
    int32_t dc_right = count ? (int32_t)(sum_right / (int64_t)count) : 0;
    uint32_t left = 0;
    uint32_t right = 0;
    for (size_t i = 0; i < frames; i += step) {
        int32_t l = (int32_t)stereo[2 * i] - dc_left;
        int32_t r = (int32_t)stereo[2 * i + 1] - dc_right;
        uint32_t al = (uint32_t)(l < 0 ? -l : l);
        uint32_t ar = (uint32_t)(r < 0 ? -r : r);
        if (al > left) {
            left = al;
        }
        if (ar > right) {
            right = ar;
        }
    }
    *peak_left = left;
    *peak_right = right;
    if (mean_left) {
        *mean_left = dc_left;
    }
    if (mean_right) {
        *mean_right = dc_right;
    }
}

/* Voice sits on the RIGHT I2S slot (firmware/README.md, pcm_dsp tests).
 * Use RIGHT unless this block's left peak is clearly louder, so a left
 * strap still works without a 3-pass energy scan. */
static size_t audio_fold_stereo_to_mono(const int16_t *stereo,
                                        size_t frames,
                                        int16_t *mono,
                                        mic_fold_stats_t *stats)
{
    uint32_t peak_left = 0;
    uint32_t peak_right = 0;
    int32_t dc_left = 0;
    int32_t dc_right = 0;
    audio_stereo_peaks(stereo, frames, &peak_left, &peak_right, &dc_left, &dc_right);
    const bool use_left = peak_left > 0 && peak_right < (peak_left / 16U);

    uint32_t output_peak = 0;
    for (size_t i = 0; i < frames; ++i) {
        int32_t value = use_left
                            ? ((int32_t)stereo[2 * i] - dc_left)
                            : ((int32_t)stereo[2 * i + 1] - dc_right);
        value *= MIC_GAIN;
        if (value > INT16_MAX) {
            value = INT16_MAX;
        } else if (value < INT16_MIN) {
            value = INT16_MIN;
        }
        mono[i] = (int16_t)value;
        const uint32_t abs_value = (uint32_t)(value < 0 ? -value : value);
        if (abs_value > output_peak) {
            output_peak = abs_value;
        }
    }

    if (stats) {
        stats->energy_left = peak_left;
        stats->energy_right = peak_right;
        stats->peak_left = peak_left;
        stats->peak_right = peak_right;
        stats->output_peak = output_peak;
        stats->source = use_left ? "L" : "R";
    }
    return frames;
}

static void audio_update_mic_level_stereo(const int16_t *stereo, size_t frames)
{
    if (frames == 0) {
        return;
    }
    uint32_t peak_left = 0;
    uint32_t peak_right = 0;
    audio_stereo_peaks(stereo, frames, &peak_left, &peak_right, NULL, NULL);
    s_mic_peak_left = peak_left;
    s_mic_peak_right = peak_right;
    uint32_t peak = peak_left > peak_right ? peak_left : peak_right;
    /* Same mapping as the USB fold: raw MEMS peaks stay below the listen
     * threshold without MIC_GAIN, so the orb looks dead while SpeechKit
     * still hears the gained stream. */
    uint32_t lvl = (peak * (uint32_t)MIC_GAIN) / MIC_VU_PEAK_DIVISOR;
    if (lvl > 100) {
        lvl = 100;
    }
    uint32_t cur = s_mic_level;
    if (lvl >= cur) {
        s_mic_level = lvl;
    } else {
        uint32_t step_down = (cur - lvl) / 4;
        if (step_down == 0) {
            step_down = 1;
        }
        s_mic_level = (cur > step_down) ? (cur - step_down) : lvl;
    }
}

void audio_log_mic_status(void)
{
    const audio_mute_snapshot_t mute = audio_mute_snapshot();
    ESP_LOGI(TAG,
             "status: vu=%u lpeak=%u rpeak=%u seq=%u uac_gaps=%u host_mute=%d local_mute=%d es7210=%s@0x%02x",
             (unsigned)s_mic_level,
             (unsigned)s_mic_peak_left,
             (unsigned)s_mic_peak_right,
             (unsigned)__atomic_load_n(&s_capture_latest_sequence, __ATOMIC_ACQUIRE),
             (unsigned)s_uac_gap_count,
             (int)mute.host_muted,
             (int)mute.local_muted,
             esp_err_to_name(s_es7210_status),
             s_es7210_addr);
}

/* Microphone slot (dev21).
 *
 * The ES7210 delivers two slots; only one is the microphone, the other
 * carries what the speaker plays. Up to dev20 pcm_dsp picked the louder slot
 * from the first block of every capture turn and of every wake-word observer
 * reset. Since dev18 a listening cue plays right at turn start, so the first
 * block after the cue gate is the cue's tail, far louder on the speaker
 * slot: turns locked onto it and the server got the speaker slot (cue tail,
 * then near-silence) instead of speech. dev19's learner needed 2 s of blocks
 * that were all 16x louder on one slot and restarted at every cue or reply,
 * so it never finished.
 *
 * Now no turn picks a slot from its own first block. The slot in use is the
 * owner's setting (KBX mic slot left|right|mix, NVS), else the slot learned
 * this boot, else the mix of both (which always contains the mic, as dev17's
 * typical pick did). The learner counts only blocks while the speaker is
 * silent in which one slot rises 12 dB over its own noise floor and is 6 dB
 * louder than the other; a slot needs 100 such blocks (1 s of speech or other
 * room sound) and 4x the votes of the other. If both slots are real
 * microphones, neither wins and the mix stays. */
#define MIC_SLOT_NVS_NS   "kbx_audio"
#define MIC_SLOT_NVS_KEY  "micslot_v1"
#define MIC_LEARN_VOTES   100U
static volatile pcm_dsp_channel_t s_mic_forced = PCM_DSP_CHANNEL_UNSELECTED;
#if CONFIG_KBX_AUDIO_CAPTURE_TDM4
/* Wired, not learned: the capture puts MIC1 left and the loopback right. */
#define MIC_LEARNED_DEFAULT PCM_DSP_CHANNEL_LEFT
#else
#define MIC_LEARNED_DEFAULT PCM_DSP_CHANNEL_UNSELECTED
#endif
static volatile pcm_dsp_channel_t s_mic_learned = MIC_LEARNED_DEFAULT;
static uint32_t s_mic_votes_l, s_mic_votes_r;

static const char *mic_slot_name(pcm_dsp_channel_t c)
{
    switch (c) {
    case PCM_DSP_CHANNEL_LEFT: return "left";
    case PCM_DSP_CHANNEL_RIGHT: return "right";
    case PCM_DSP_CHANNEL_MIX: return "mix";
    default: return "auto";
    }
}

static pcm_dsp_channel_t mic_channel_current(void)
{
    pcm_dsp_channel_t c = s_mic_forced;
    if (c != PCM_DSP_CHANNEL_UNSELECTED) return c;
    c = s_mic_learned;
    return c != PCM_DSP_CHANNEL_UNSELECTED ? c : PCM_DSP_CHANNEL_MIX;
}

/* Energy (DC removed) and peak of one stereo block per slot. */
static void block_energy(const int16_t *stereo, size_t frames, uint64_t *e_l, uint64_t *e_r,
                         uint32_t *pk_l, uint32_t *pk_r)
{
    int64_t sum_l = 0, sum_r = 0;
    for (size_t i = 0; i < frames; ++i) {
        sum_l += stereo[2 * i];
        sum_r += stereo[2 * i + 1];
    }
    int32_t mean_l = (int32_t)(sum_l / (int64_t)frames), mean_r = (int32_t)(sum_r / (int64_t)frames);
    uint64_t el = 0, er = 0;
    uint32_t pl = 0, pr = 0;
    for (size_t i = 0; i < frames; ++i) {
        int32_t l = stereo[2 * i] - mean_l, r = stereo[2 * i + 1] - mean_r;
        el += (uint64_t)((int64_t)l * l);
        er += (uint64_t)((int64_t)r * r);
        uint32_t al = (uint32_t)(l < 0 ? -l : l), ar = (uint32_t)(r < 0 ? -r : r);
        if (al > pl) pl = al;
        if (ar > pr) pr = ar;
    }
    *e_l = el;
    *e_r = er;
    if (pk_l) *pk_l = pl;
    if (pk_r) *pk_r = pr;
}

static void mic_channel_learn(const int16_t *stereo, size_t frames)
{
    static uint64_t s_floor_l, s_floor_r;
    if (s_mic_learned != PCM_DSP_CHANNEL_UNSELECTED) return;
    /* Speaker output would vote for the speaker slot: skip, keep the votes. */
    if (audio_is_speaking() || agent_playback_active()) return;
    uint64_t e_l, e_r;
    block_energy(stereo, frames, &e_l, &e_r, NULL, NULL);
    /* Noise floors: follow drops at once, rise ~3 % per 10 ms block. */
    s_floor_l = (!s_floor_l || e_l < s_floor_l) ? e_l + 1U : s_floor_l + s_floor_l / 32U + 1U;
    s_floor_r = (!s_floor_r || e_r < s_floor_r) ? e_r + 1U : s_floor_r + s_floor_r / 32U + 1U;
    if (e_l > 16U * s_floor_l && e_l / 4U > e_r) ++s_mic_votes_l;
    else if (e_r > 16U * s_floor_r && e_r / 4U > e_l) ++s_mic_votes_r;
    pcm_dsp_channel_t c = PCM_DSP_CHANNEL_UNSELECTED;
    if (s_mic_votes_l >= MIC_LEARN_VOTES && s_mic_votes_l > 4U * s_mic_votes_r) c = PCM_DSP_CHANNEL_LEFT;
    else if (s_mic_votes_r >= MIC_LEARN_VOTES && s_mic_votes_r > 4U * s_mic_votes_l) c = PCM_DSP_CHANNEL_RIGHT;
    if (c == PCM_DSP_CHANNEL_UNSELECTED) return;
    s_mic_learned = c;
    ESP_LOGI(TAG, "microphone slot learned: %s (votes left=%u right=%u)%s", mic_slot_name(c),
             (unsigned)s_mic_votes_l, (unsigned)s_mic_votes_r,
             s_mic_forced != PCM_DSP_CHANNEL_UNSELECTED ? "; the KBX mic slot setting wins" : "");
}

static void mic_channel_apply(pcm_dsp_t *dsp)
{
    pcm_dsp_set_channel(dsp, mic_channel_current());
}

static void mic_slot_load(void)
{
    nvs_handle_t h;
    uint8_t v = 0;
    if (nvs_open(MIC_SLOT_NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        (void)nvs_get_u8(h, MIC_SLOT_NVS_KEY, &v);
        nvs_close(h);
    }
    s_mic_forced = (v == PCM_DSP_CHANNEL_LEFT || v == PCM_DSP_CHANNEL_RIGHT || v == PCM_DSP_CHANNEL_MIX)
        ? (pcm_dsp_channel_t)v : PCM_DSP_CHANNEL_UNSELECTED;
    ESP_LOGI(TAG, "mic slot setting %s (KBX mic slot left|right|mix|auto, KBX mic probe)",
             mic_slot_name(s_mic_forced));
}

esp_err_t audio_mic_set_slot(const char *name)
{
    pcm_dsp_channel_t c;
    if (!name) return ESP_ERR_INVALID_ARG;
    if (!strcmp(name, "left")) c = PCM_DSP_CHANNEL_LEFT;
    else if (!strcmp(name, "right")) c = PCM_DSP_CHANNEL_RIGHT;
    else if (!strcmp(name, "mix")) c = PCM_DSP_CHANNEL_MIX;
    else if (!strcmp(name, "auto")) c = PCM_DSP_CHANNEL_UNSELECTED;
    else return ESP_ERR_INVALID_ARG;
    s_mic_forced = c;
    if (c == PCM_DSP_CHANNEL_UNSELECTED) {
        /* auto: learn again from scratch. */
        s_mic_learned = MIC_LEARNED_DEFAULT;
        s_mic_votes_l = s_mic_votes_r = 0;
    }
    nvs_handle_t h;
    if (nvs_open(MIC_SLOT_NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, MIC_SLOT_NVS_KEY, (uint8_t)c);
        nvs_commit(h);
        nvs_close(h);
    }
    ESP_LOGI(TAG, "mic slot setting %s; next turn and wake word reset use %s", mic_slot_name(c),
             mic_slot_name(mic_channel_current()));
    return ESP_OK;
}

void audio_mic_log_slot(void)
{
    ESP_LOGI(TAG, "mic slot: in use %s (setting %s, learned %s, votes left=%u right=%u)",
             mic_slot_name(mic_channel_current()), mic_slot_name(s_mic_forced),
             mic_slot_name(s_mic_learned), (unsigned)s_mic_votes_l, (unsigned)s_mic_votes_r);
}

/* KBX mic probe: per-slot level of the raw ES7210 blocks (before the cue
 * gate and before any gain) for 3 s, one line per second and a verdict. */
#define MIC_PROBE_BLOCKS_PER_S 100U
static volatile uint32_t s_probe_left;   /* blocks still to measure, 0 = off */
static struct {
    uint64_t e_l, e_r, tot_l, tot_r;
    uint32_t pk_l, pk_r, blocks, gated, spk, tot_blocks, second;
} s_probe;

void audio_mic_probe_start(void)
{
    s_probe_left = 0;
    memset(&s_probe, 0, sizeof(s_probe));
    s_probe_left = 3U * MIC_PROBE_BLOCKS_PER_S;
    ESP_LOGI(TAG, "mic probe: 3 s of raw slots (before cue gate and gain) - speak now");
}

/* dBFS x10 (integer print keeps the broker's stack small). */
static int ms_dbfs10(uint64_t energy, uint64_t n)
{
    if (!n || !energy) return -960;
    return (int)lround(100.0 * log10((double)energy / (double)n / (32768.0 * 32768.0)));
}

static int pk_dbfs10(uint32_t peak)
{
    return peak ? (int)lround(200.0 * log10((double)peak / 32768.0)) : -960;
}

static void mic_probe_block(const int16_t *stereo, size_t frames, bool gated)
{
    if (!s_probe_left || frames == 0) return;
    uint64_t e_l, e_r;
    uint32_t pk_l, pk_r;
    block_energy(stereo, frames, &e_l, &e_r, &pk_l, &pk_r);
    s_probe.e_l += e_l;
    s_probe.e_r += e_r;
    s_probe.tot_l += e_l;
    s_probe.tot_r += e_r;
    if (pk_l > s_probe.pk_l) s_probe.pk_l = pk_l;
    if (pk_r > s_probe.pk_r) s_probe.pk_r = pk_r;
    s_probe.gated += gated ? 1U : 0U;
    s_probe.spk += audio_is_speaking() ? 1U : 0U;
    ++s_probe.tot_blocks;
    if (++s_probe.blocks == MIC_PROBE_BLOCKS_PER_S) {
        const uint64_t n = (uint64_t)s_probe.blocks * frames;
        int l = ms_dbfs10(s_probe.e_l, n), r = ms_dbfs10(s_probe.e_r, n);
        int pl = pk_dbfs10(s_probe.pk_l), pr = pk_dbfs10(s_probe.pk_r);
        ESP_LOGI(TAG, "mic probe %u/3: left rms=%s%d.%d peak=%s%d.%d dBFS | right rms=%s%d.%d peak=%s%d.%d dBFS "
                 "| speaker blocks=%u gated=%u",
                 (unsigned)++s_probe.second,
                 l < 0 ? "-" : "", abs(l) / 10, abs(l) % 10, pl < 0 ? "-" : "", abs(pl) / 10, abs(pl) % 10,
                 r < 0 ? "-" : "", abs(r) / 10, abs(r) % 10, pr < 0 ? "-" : "", abs(pr) / 10, abs(pr) % 10,
                 (unsigned)s_probe.spk, (unsigned)s_probe.gated);
        s_probe.e_l = s_probe.e_r = 0;
        s_probe.pk_l = s_probe.pk_r = 0;
        s_probe.blocks = s_probe.gated = s_probe.spk = 0;
    }
    if (--s_probe_left == 0) {
        const uint64_t n = (uint64_t)s_probe.tot_blocks * frames;
        int l = ms_dbfs10(s_probe.tot_l, n), r = ms_dbfs10(s_probe.tot_r, n);
        int d = l - r;
        ESP_LOGI(TAG, "mic probe: left %s%d.%d dBFS, right %s%d.%d dBFS -> %s louder by %d.%d dB; "
                 "slot in use %s (setting %s, learned %s)",
                 l < 0 ? "-" : "", abs(l) / 10, abs(l) % 10, r < 0 ? "-" : "", abs(r) / 10, abs(r) % 10,
                 d >= 0 ? "left" : "right", abs(d) / 10, abs(d) % 10,
                 mic_slot_name(mic_channel_current()), mic_slot_name(s_mic_forced),
                 mic_slot_name(s_mic_learned));
    }
}

/* ── Listening earcons (dev18) ──────────────────────────────────────────────
 * Short, soft start/end cues so the owner always knows when the box listens.
 * Synthesized at 48 kHz on a small PSRAM-stack task and queued into the
 * shared speaker ring (volume and local mute apply), so the caller - the
 * wake hit, a tap, a follow-up stage - never waits and capture starts at
 * once. While a cue plays (plus the speaker path's latency) the microphone
 * broker zeroes its blocks: the cue never reaches the uplink, the USB mic or
 * the wake word detector. The board has no echo reference for AEC.
 * Styles (ids for device_config assist.sound_cue_style):
 *   chime - two soft notes, rising for start, falling for end
 *   pop   - one short pop, higher for start, lower for end
 *   glass - a gentle glassy ping (inharmonic partial), high/lower */
#define CUE_RATE        48000U
#define CUE_CHUNK       480U          /* 10 ms of stereo frames */
#ifdef KBX_CUE_AMP
#define CUE_AMP         KBX_CUE_AMP   /* board_*.h */
#else
#define CUE_AMP         5200.0f       /* before volume, ~-16 dBFS peak */
#endif
#define CUE_PA_WARM_MS  40U           /* silence while a cold PA settles */
#ifndef KBX_CUE_STYLE_DEFAULT
#define KBX_CUE_STYLE_DEFAULT AUDIO_CUE_STYLE_CHIME
#endif
#define CUE_GATE_TAIL_MS 180U        /* ring + I2S DMA + room decay */
#define CUE_GATE_MAX_MS  400U         /* hard bound: the gate can never stick */

static const char *const k_cue_style_ids[AUDIO_CUE_STYLE_COUNT] = { "chime", "pop", "glass", "bell" };
static volatile bool s_cues_on = true;
static volatile audio_cue_style_t s_cue_style = AUDIO_CUE_STYLE_CHIME;
static TaskHandle_t s_cue_task;
/* Gate window in esp_timer milliseconds (32-bit: one atomic store, wrap
 * safe with signed differences). 0 = no gate. Opened only when the cue
 * actually starts playing, closed at its end plus the speaker-path tail,
 * never longer than CUE_GATE_MAX_MS after it opened. */
static volatile uint32_t s_cue_gate_from_ms;
static volatile uint32_t s_cue_gate_until_ms;

static inline uint32_t now_ms32(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static void cue_gate_open(uint32_t len_ms)
{
    uint32_t now = now_ms32();
    s_cue_gate_from_ms = now ? now : 1;
    s_cue_gate_until_ms = now + len_ms + CUE_GATE_TAIL_MS;
}

static void cue_gate_extend_to_tail(void)
{
    uint32_t from = s_cue_gate_from_ms;
    if (!from) return;
    uint32_t until = now_ms32() + CUE_GATE_TAIL_MS;
    if ((int32_t)(until - (from + CUE_GATE_MAX_MS)) > 0) until = from + CUE_GATE_MAX_MS;
    if ((int32_t)(until - s_cue_gate_until_ms) > 0) s_cue_gate_until_ms = until;
}

static int cue_length_ms(audio_cue_style_t style)
{
    switch (style) {
    case AUDIO_CUE_STYLE_POP: return 70;
    case AUDIO_CUE_STYLE_GLASS: return 150;
    case AUDIO_CUE_STYLE_BELL: return 360;
    case AUDIO_CUE_STYLE_CHIME:
    default: return 130;
    }
}

/* Soft attack (5 ms raised cosine) and exponential-like decay to zero. */
static float cue_env(float t, float len, float attack, float tau)
{
    if (t < 0.0f || t >= len) return 0.0f;
    float a = t < attack ? 0.5f - 0.5f * cosf(3.14159265f * t / attack) : 1.0f;
    float d = expf(-t / tau);
    float tail = len - t < 0.008f ? (len - t) / 0.008f : 1.0f; /* no click at the end */
    return a * d * tail;
}

/* t in seconds. start=true: the rising/higher variant. */
static float cue_sample(audio_cue_style_t style, bool start, float t)
{
    const float two_pi = 6.2831853f;
    switch (style) {
    case AUDIO_CUE_STYLE_POP: {
        float f0 = start ? 1400.0f : 760.0f;
        float glide = start ? 1.0f + 2.5f * t : 1.0f - 2.0f * t; /* slight up/down */
        return cue_env(t, 0.070f, 0.003f, 0.018f) * sinf(two_pi * f0 * glide * t);
    }
    case AUDIO_CUE_STYLE_GLASS: {
        float f0 = start ? 1760.0f : 1318.5f;
        float e = cue_env(t, 0.150f, 0.004f, 0.050f);
        return e * (0.75f * sinf(two_pi * f0 * t) + 0.25f * sinf(two_pi * f0 * 2.76f * t));
    }
    case AUDIO_CUE_STYLE_BELL: {
        /* Two ringing notes, 150 ms apart: C6 -> G6 (start) or G6 -> C6
         * (end), each with a soft octave partial and a 90 ms decay. */
        float lo = 1046.5f, hi = 1568.0f;
        float f1 = start ? lo : hi, f2 = start ? hi : lo;
        float e1 = cue_env(t, 0.200f, 0.006f, 0.090f);
        float t2 = t - 0.150f;
        float e2 = cue_env(t2, 0.210f, 0.006f, 0.090f);
        float s1 = e1 * (0.85f * sinf(two_pi * f1 * t) + 0.15f * sinf(two_pi * 2.0f * f1 * t));
        float s2 = e2 * (0.85f * sinf(two_pi * f2 * t2) + 0.15f * sinf(two_pi * 2.0f * f2 * t2));
        return 0.8f * (s1 + s2);
    }
    case AUDIO_CUE_STYLE_CHIME:
    default: {
        /* Two 60 ms notes, 10 ms apart: A5 -> E6 (start) or E6 -> A5 (end). */
        float lo = 880.0f, hi = 1318.5f;
        float f1 = start ? lo : hi, f2 = start ? hi : lo;
        float s1 = cue_env(t, 0.060f, 0.005f, 0.030f) * sinf(two_pi * f1 * t);
        float t2 = t - 0.070f;
        float s2 = cue_env(t2, 0.060f, 0.005f, 0.030f) * sinf(two_pi * f2 * t2);
        return 0.8f * (s1 + s2);
    }
    }
}

/* Rendered cue samples (mono, PSRAM), per style and direction, built once
 * at low priority: synthesising the 360 ms bell at play time held the cue
 * task long enough to stall the UI timer for ~0.5 s. */
static int16_t *s_cue_pcm[AUDIO_CUE_STYLE_COUNT][2];

static const int16_t *cue_pcm(audio_cue_style_t style, bool start)
{
    int16_t **slot = &s_cue_pcm[style][start ? 1 : 0];
    if (*slot) return *slot;
    const int total = (int)(CUE_RATE / 1000U) * cue_length_ms(style);
    int16_t *pcm = heap_caps_malloc((size_t)total * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!pcm) return NULL;
    for (int i = 0; i < total; i++) {
        pcm[i] = (int16_t)(CUE_AMP * cue_sample(style, start, (float)i / (float)CUE_RATE));
        if ((i % 2400) == 2399) vTaskDelay(1);   /* let the UI and audio run */
    }
    *slot = pcm;
    return pcm;
}

static void cue_render(bool start)
{
    static int16_t buf[CUE_CHUNK * 2];
    audio_cue_style_t style = s_cue_style;
    int len_ms = cue_length_ms(style);
    int total = (int)(CUE_RATE / 1000U) * len_ms;
    const int16_t *pcm = cue_pcm(style, start);
    if (!pcm) return;
    uint32_t warm_ms = 0;
    /* The writer switches the PA off after 600 ms of silence; a cue that
     * starts on a cold PA lost its first note while the amplifier woke. */
    if (s_es8311_status == ESP_OK) {
        if (!s_pa_enabled) warm_ms = CUE_PA_WARM_MS;
        audio_set_pa(true);
    }
    /* With echo cancellation the microphone keeps listening through the cue
     *; without it the bounded gate keeps the cue out. */
    if (!audio_aec_active()) cue_gate_open((uint32_t)len_ms + warm_ms);
    if (warm_ms) {
        memset(buf, 0, sizeof(buf));
        for (uint32_t ms = 0; ms < warm_ms; ms += 10U) {
            (void)speaker_process_and_queue(buf, (size_t)CUE_CHUNK * 2U, pdMS_TO_TICKS(40), true);
        }
    }
    for (int pos = 0; pos < total; pos += (int)CUE_CHUNK) {
        int n = total - pos < (int)CUE_CHUNK ? total - pos : (int)CUE_CHUNK;
        for (int i = 0; i < n; i++) {
            buf[2 * i] = pcm[pos + i];
            buf[2 * i + 1] = pcm[pos + i];
        }
        /* agent volume: host mute does not silence it, local mute does. */
        (void)speaker_process_and_queue(buf, (size_t)n * 2U, pdMS_TO_TICKS(40), true);
    }
    cue_gate_extend_to_tail();
    ESP_LOGI(TAG, "cue %s (%s, %d ms)", start ? "listen-start" : "listen-end",
             k_cue_style_ids[style], len_ms);
}

static void cue_task(void *arg)
{
    (void)arg;
    (void)cue_pcm(s_cue_style, true);
    (void)cue_pcm(s_cue_style, false);
    for (;;) {
        uint32_t bits = 0;
        xTaskNotifyWait(0, UINT32_MAX, &bits, portMAX_DELAY);
        /* Both requested before the task ran: start first, then end. */
        if (bits & (1U << AUDIO_CUE_LISTEN_START)) cue_render(true);
        if (bits & (1U << AUDIO_CUE_LISTEN_END)) cue_render(false);
    }
}

static void cue_load_settings(void)
{
    nvs_handle_t h;
    uint8_t on = 1, style = KBX_CUE_STYLE_DEFAULT;
    if (nvs_open("kbx_assist", NVS_READONLY, &h) == ESP_OK) {
        (void)nvs_get_u8(h, "cues_v1", &on);
        (void)nvs_get_u8(h, "cuestyle_v1", &style);
        nvs_close(h);
    }
    s_cues_on = on != 0;
    s_cue_style = style < AUDIO_CUE_STYLE_COUNT ? (audio_cue_style_t)style : KBX_CUE_STYLE_DEFAULT;
}

static void cue_store(const char *key, uint8_t value)
{
    nvs_handle_t h;
    if (nvs_open("kbx_assist", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u8(h, key, value);
    nvs_commit(h);
    nvs_close(h);
}

static void cue_start_task(void)
{
    if (s_cue_task || !s_spk_rb) return;
    if (xTaskCreateWithCaps(cue_task, "kbx_cue", 3072, NULL, 5, &s_cue_task,
                            MALLOC_CAP_SPIRAM) != pdPASS) {
        s_cue_task = NULL;
        ESP_LOGW(TAG, "cue task create failed - sound cues off");
    }
}

void audio_cue_play(audio_cue_t cue)
{
    if (!s_cues_on || !s_cue_task || cue >= AUDIO_CUE_COUNT) return;
    /* The microphone is gated only once the cue really plays (cue_render):
     * everything captured before - the pre-roll after a hit - passes. */
    xTaskNotify(s_cue_task, 1U << cue, eSetBits);
}

bool audio_cues_enabled(void)
{
    return s_cues_on;
}

void audio_cues_set_enabled(bool on)
{
    s_cues_on = on;
    cue_store("cues_v1", on ? 1 : 0);
    ESP_LOGI(TAG, "sound cues %s (style %s)", on ? "on" : "off", k_cue_style_ids[s_cue_style]);
}

audio_cue_style_t audio_cue_style(void)
{
    return s_cue_style;
}

const char *audio_cue_style_id(audio_cue_style_t style)
{
    return style < AUDIO_CUE_STYLE_COUNT ? k_cue_style_ids[style] : NULL;
}

esp_err_t audio_cue_set_style_id(const char *id)
{
    for (int i = 0; id && i < AUDIO_CUE_STYLE_COUNT; i++) {
        if (!strcmp(id, k_cue_style_ids[i])) {
            s_cue_style = (audio_cue_style_t)i;
            cue_store("cuestyle_v1", (uint8_t)i);
            ESP_LOGI(TAG, "sound cue style %s", k_cue_style_ids[i]);
            /* Preview so the owner hears the choice (also when cues are off). */
            if (s_cue_task) {
                xTaskNotify(s_cue_task, 1U << AUDIO_CUE_LISTEN_START, eSetBits);
            }
            return ESP_OK;
        }
    }
    return ESP_ERR_INVALID_ARG;
}

/* Broker only: is this block inside the gate? Logs the transitions and
 * closes a gate that outlived its bound. */
static bool cue_gate_check(void)
{
    static bool s_on;
    uint32_t from = s_cue_gate_from_ms;
    if (!from) return false;
    uint32_t now = now_ms32();
    uint32_t until = s_cue_gate_until_ms;
    if ((int32_t)(until - (from + CUE_GATE_MAX_MS)) > 0) until = from + CUE_GATE_MAX_MS;
    bool on = (int32_t)(now - from) >= 0 && (int32_t)(until - now) > 0;
    if (on && !s_on) {
        ESP_LOGI(TAG, "cue gate on");
    } else if (!on && s_on) {
        ESP_LOGI(TAG, "cue gate off (%u ms)", (unsigned)(now - from));
    }
    if (!on && (int32_t)(now - until) >= 0) s_cue_gate_from_ms = 0;
    s_on = on;
    return on;
}

typedef enum {
    COPY_OK = 0,
    COPY_BUSY,      /* ring lock not taken in time: the block is still there */
    COPY_MISSING,   /* the slot holds another block: overwritten or not written */
} copy_result_t;

static copy_result_t capture_try_copy_block(uint32_t sequence, capture_block_t *copy)
{
    if (!s_capture_ring || !s_capture_ring_lock || !copy || sequence == 0) {
        return COPY_MISSING;
    }
    if (xSemaphoreTake(s_capture_ring_lock, pdMS_TO_TICKS(2)) != pdTRUE) {
        return COPY_BUSY;
    }
    capture_block_t *slot = &s_capture_ring[sequence % CAPTURE_RING_BLOCKS];
    const uint32_t before = __atomic_load_n(&slot->sequence, __ATOMIC_ACQUIRE);
    if (before != sequence) {
        xSemaphoreGive(s_capture_ring_lock);
        return COPY_MISSING;
    }
    copy->flags = slot->flags;
    copy->frames = slot->frames;
    memcpy(copy->stereo, slot->stereo, sizeof(copy->stereo));
    const uint32_t after = __atomic_load_n(&slot->sequence, __ATOMIC_ACQUIRE);
    xSemaphoreGive(s_capture_ring_lock);
    return after == sequence ? COPY_OK : COPY_MISSING;
}

static bool capture_copy_block(uint32_t sequence, capture_block_t *copy)
{
    return capture_try_copy_block(sequence, copy) == COPY_OK;
}

static esp_err_t capture_wait_for_block(uint32_t sequence,
                                        capture_block_t *copy,
                                        uint32_t timeout_ms,
                                        bool *overflow)
{
    if (overflow) {
        *overflow = false;
    }
    const TickType_t start = xTaskGetTickCount();
    const TickType_t timeout_ticks = pdMS_TO_TICKS(timeout_ms);
    for (;;) {
        const uint32_t latest = capture_ready_sequence();
        if (latest != 0) {
            const int32_t distance = (int32_t)(latest - sequence);
            if (distance >= (int32_t)CAPTURE_READER_MAX_LAG_BLOCKS) {
                if (overflow) {
                    *overflow = true;
                }
                return ESP_ERR_INVALID_STATE;
            }
            if (distance >= 0 && capture_copy_block(sequence, copy)) {
                return ESP_OK;
            }
        }
        if (timeout_ticks == 0 || (xTaskGetTickCount() - start) >= timeout_ticks) {
            return ESP_ERR_TIMEOUT;
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }
}

#if CONFIG_KBX_AUDIO_CAPTURE_TDM4
/* KBX tdm?: energy of each of the four TDM slots over the last second, to
 * map MIC1/MIC2/MIC3 (loopback)/MIC4 on the board. */
static uint64_t s_tdm_acc[4], s_tdm_last[4];
static uint32_t s_tdm_blocks;

static void tdm_probe_block(const int16_t *raw, size_t frames)
{
    for (size_t i = 0; i < frames; ++i) {
        for (int c = 0; c < 4; ++c) {
            int32_t v = raw[4U * i + (size_t)c];
            s_tdm_acc[c] += (uint64_t)((int64_t)v * v);
        }
    }
    if (++s_tdm_blocks >= 100U) {
        for (int c = 0; c < 4; ++c) {
            s_tdm_last[c] = s_tdm_acc[c] / (100U * CAPTURE_BLOCK_FRAMES);
            s_tdm_acc[c] = 0;
        }
        s_tdm_blocks = 0;
    }
}

void audio_tdm_log(void)
{
    char line[160];
    int n = 0;
    for (int c = 0; c < 4; ++c) {
        double ms = (double)s_tdm_last[c];
        double db = ms > 0.0 ? 10.0 * log10(ms / (32768.0 * 32768.0)) : -120.0;
        n += snprintf(line + n, sizeof(line) - (size_t)n, " slot%d=%.1f", c, db);
    }
    ESP_LOGI(TAG, "tdm rms dBFS (last 1 s):%s", line);
}
#else
void audio_tdm_log(void)
{
    ESP_LOGI(TAG, "tdm: this board captures standard I2S");
}
#endif

#if CONFIG_KBX_AUDIO_AEC
/* Shared AEC in the broker: blocks are 10 ms, the stage takes 20 ms, so
 * two blocks are processed together and the cleaned 16 kHz microphone is
 * upsampled back to 48 kHz into the left slot one 20 ms frame later (a
 * constant 20 ms + AEC chunk latency for every consumer). The right slot
 * keeps the loopback. Blocks are only rewritten while the AEC is active. */
#define BROKER_AEC_FIFO (PCM_DSP_INPUT_FRAMES_20MS * 3U)
static int16_t s_aec_pair[PCM_DSP_INPUT_FRAMES_20MS * 2U];
static size_t s_aec_pair_frames;
static int16_t s_aec_fifo[BROKER_AEC_FIFO];
static size_t s_aec_fifo_n = PCM_DSP_INPUT_FRAMES_20MS;   /* prefilled silence */
static int16_t s_aec_up_prev;

static void broker_aec_block(int16_t *stereo, size_t frames)
{
    if (!s_broker_aec || !audio_aec_active() || frames != CAPTURE_BLOCK_FRAMES) return;
    memcpy(s_aec_pair + s_aec_pair_frames * 2U, stereo, frames * 2U * sizeof(int16_t));
    s_aec_pair_frames += frames;
    if (s_aec_pair_frames >= PCM_DSP_INPUT_FRAMES_20MS) {
        s_aec_pair_frames = 0;
        int16_t clean[PCM_DSP_OUTPUT_SAMPLES_20MS];
        if (aec_stage_process_20ms(s_broker_aec, s_aec_pair, clean, 256U) &&
            s_aec_fifo_n + PCM_DSP_INPUT_FRAMES_20MS <= BROKER_AEC_FIFO) {
            s_aec_fifo_n += pcm_dsp_upsample_3x(clean, PCM_DSP_OUTPUT_SAMPLES_20MS,
                                                s_aec_fifo + s_aec_fifo_n, &s_aec_up_prev);
        }
    }
    const size_t n = s_aec_fifo_n < frames ? s_aec_fifo_n : frames;
    for (size_t i = 0; i < frames; ++i) stereo[2U * i] = i < n ? s_aec_fifo[i] : 0;
    memmove(s_aec_fifo, s_aec_fifo + n, (s_aec_fifo_n - n) * sizeof(int16_t));
    s_aec_fifo_n -= n;
}

#endif

static uint32_t capture_ready_sequence(void)
{
#if CONFIG_KBX_AUDIO_AEC
    if (s_aec_task_running && audio_aec_active()) {
        return __atomic_load_n(&s_capture_clean_sequence, __ATOMIC_ACQUIRE);
    }
#endif
    return __atomic_load_n(&s_capture_latest_sequence, __ATOMIC_ACQUIRE);
}

#if CONFIG_KBX_AUDIO_AEC
/* Cleans each new block's microphone slot in the ring, in order, then
 * publishes it as ready. Runs beside the broker so the I2S reader never
 * waits for ESP-SR (the broker-inline version starved I2S: gaps reset the
 * wake word ~5x per second). PSRAM stack; it never writes flash. */
static void capture_aec_task(void *arg)
{
    (void)arg;
    static capture_block_t block;
    uint32_t next = 0;
    for (;;) {
        const uint32_t latest = __atomic_load_n(&s_capture_latest_sequence, __ATOMIC_ACQUIRE);
        if (!audio_aec_active() || latest == 0) {
            __atomic_store_n(&s_capture_clean_sequence, latest, __ATOMIC_RELEASE);
            next = 0;
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        if (next == 0 || (int32_t)(latest - next) >= (int32_t)(CAPTURE_RING_BLOCKS / 2U)) {
            /* start or fell behind: resume at the newest block */
            next = latest;
            s_aec_pair_frames = 0;
        }
        if ((int32_t)(latest - next) < 0) {
            vTaskDelay(pdMS_TO_TICKS(2));
            continue;
        }
        if (capture_copy_block(next, &block)) {
            if (!block.flags) broker_aec_block(block.stereo, block.frames);
            if (xSemaphoreTake(s_capture_ring_lock, pdMS_TO_TICKS(5)) == pdTRUE) {
                capture_block_t *slot = &s_capture_ring[next % CAPTURE_RING_BLOCKS];
                if (__atomic_load_n(&slot->sequence, __ATOMIC_ACQUIRE) == next) {
                    memcpy(slot->stereo, block.stereo, sizeof(slot->stereo));
                }
                xSemaphoreGive(s_capture_ring_lock);
            }
        }
        __atomic_store_n(&s_capture_clean_sequence, next, __ATOMIC_RELEASE);
        next = capture_sequence_next(next);
    }
}

#endif

static void capture_broker_task(void *arg)
{
    (void)arg;
#if CONFIG_KBX_AUDIO_CAPTURE_TDM4
    static int16_t raw[CAPTURE_BLOCK_FRAMES * 4U];
#else
    /* i2s_channel_read copies from its internal DMA buffers; this staging
     * buffer, like the capture ring, can live in PSRAM. */
    static EXT_RAM_BSS_ATTR int16_t raw[CAPTURE_BLOCK_FRAMES * 2U];
#endif
    uint32_t read_errors = 0;
    uint32_t gap_blocks = 0;
    int64_t last_diag_us = 0;

    for (;;) {
        size_t got = 0;
        const size_t requested_bytes = sizeof(raw);
        const esp_err_t err = i2s_channel_read(s_rx,
                                               raw,
                                               requested_bytes,
                                               &got,
                                               pdMS_TO_TICKS(20));
#if CONFIG_KBX_AUDIO_CAPTURE_TDM4
        size_t frames = (got / sizeof(int16_t)) / 4U;
#else
        size_t frames = (got / sizeof(int16_t)) / 2U;
#endif
        if (frames > CAPTURE_BLOCK_FRAMES) {
            frames = CAPTURE_BLOCK_FRAMES;
        }

        const uint32_t sequence = s_capture_next_sequence;
        s_capture_next_sequence = capture_sequence_next(s_capture_next_sequence);
        if (xSemaphoreTake(s_capture_ring_lock, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        capture_block_t *slot = &s_capture_ring[sequence % CAPTURE_RING_BLOCKS];
        __atomic_store_n(&slot->sequence, 0, __ATOMIC_RELEASE);
        memset(slot->stereo, 0, sizeof(slot->stereo));
#if CONFIG_KBX_AUDIO_CAPTURE_TDM4
        tdm_probe_block(raw, frames);
        /* Keep the 1.85C V2 block layout: left = MIC1, right = loopback
         * (MIC3, TDM slot 2). */
        for (size_t i = 0; i < frames; ++i) {
            slot->stereo[2U * i] = raw[4U * i];
            slot->stereo[2U * i + 1U] = raw[4U * i + 2U];
        }
#else
        memcpy(slot->stereo, raw, frames * 2U * sizeof(int16_t));
#endif
        const bool gated = cue_gate_check();
        if (err == ESP_OK) mic_probe_block(slot->stereo, frames, gated);
        if (gated) {
            /* A listening cue is on the speaker: no microphone copy of it
             * reaches the uplink, the USB mic or the wake word. */
            memset(slot->stereo, 0, sizeof(slot->stereo));
        }
        slot->frames = frames;
        slot->flags = (err == ESP_OK && frames == CAPTURE_BLOCK_FRAMES)
                          ? 0
                          : CAPTURE_BLOCK_FLAG_GAP;
        if (err != ESP_OK) {
            read_errors++;
        }
        if (slot->flags != 0) {
            gap_blocks++;
        }

        __atomic_store_n(&slot->sequence, sequence, __ATOMIC_RELEASE);
        xSemaphoreGive(s_capture_ring_lock);
        __atomic_store_n(&s_capture_latest_sequence, sequence, __ATOMIC_RELEASE);

        if (err == ESP_OK && frames > 0) {
            audio_update_mic_level_stereo(slot->stereo, frames);
        }
        /* Outside the ring lock; the broker is the only writer of slot. */
        if (!gated && err == ESP_OK && frames == CAPTURE_BLOCK_FRAMES) {
            mic_channel_learn(slot->stereo, frames);
        }

        const int64_t now_us = esp_timer_get_time();
        const bool fault = (err != ESP_OK) || (slot->flags != 0);
        if (now_us - last_diag_us >= MIC_DIAG_LOG_INTERVAL_US) {
            if (fault || read_errors || gap_blocks || s_uac_gap_count) {
                ESP_LOGW(TAG,
                         "mic broker: err=%s frames=%u vu=%u read_errs=%u gaps=%u uac_gaps=%u",
                         esp_err_to_name(err),
                         (unsigned)frames,
                         (unsigned)s_mic_level,
                         (unsigned)read_errors,
                         (unsigned)gap_blocks,
                         (unsigned)s_uac_gap_count);
            }
            read_errors = 0;
            gap_blocks = 0;
            last_diag_us = now_us;
        }
    }
}

static esp_err_t capture_broker_start(void)
{
    const uint32_t psram_caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
    s_capture_ring = heap_caps_calloc(CAPTURE_RING_BLOCKS,
                                      sizeof(*s_capture_ring),
                                      psram_caps);
    s_capture_turn_block = heap_caps_calloc(1, sizeof(*s_capture_turn_block), psram_caps);
    s_capture_turn_stereo = heap_caps_calloc(PCM_DSP_INPUT_FRAMES_20MS * 2U,
                                             sizeof(*s_capture_turn_stereo),
                                             psram_caps);
    s_capture_ring_lock = xSemaphoreCreateMutex();
    s_capture_turn_lock = xSemaphoreCreateMutex();
    if (!s_capture_ring || !s_capture_turn_block || !s_capture_turn_stereo ||
        !s_capture_ring_lock || !s_capture_turn_lock) {
        goto no_mem;
    }
#if CONFIG_KBX_AUDIO_CAPTURE_TDM4
    /* RLCD 4.2: the echo canceller takes 28 KB of internal RAM, so the
     * broker's stack goes to PSRAM (it never writes flash; with
     * SPIRAM_XIP_FROM_PSRAM PSRAM stays mapped while others do). */
    if (xTaskCreateWithCaps(capture_broker_task, "kbx_mic_broker", CAPTURE_TASK_STACK_BYTES, NULL,
                            CAPTURE_TASK_PRIORITY, NULL, MALLOC_CAP_SPIRAM) != pdPASS) {
#else
    if (xTaskCreate(capture_broker_task,
                    "kbx_mic_broker",
                    CAPTURE_TASK_STACK_BYTES,
                    NULL,
                    CAPTURE_TASK_PRIORITY,
                    NULL) != pdPASS) {
#endif
        goto no_mem;
    }
#if CONFIG_KBX_AUDIO_AEC
    if (s_broker_aec) {
        if (xTaskCreateWithCaps(capture_aec_task, "kbx_aec", 12288, NULL, CAPTURE_TASK_PRIORITY - 1U, NULL,
                                MALLOC_CAP_SPIRAM) == pdPASS) {
            s_aec_task_running = true;
        } else {
            ESP_LOGW(TAG, "aec task create failed - half duplex");
        }
    }
#endif
    return ESP_OK;

no_mem:
    ESP_LOGE(TAG, "capture broker requires %u bytes of PSRAM plus two mutexes",
             (unsigned)(CAPTURE_RING_BLOCKS * sizeof(*s_capture_ring) +
                        sizeof(*s_capture_turn_block) +
                        PCM_DSP_INPUT_FRAMES_20MS * 2U * sizeof(*s_capture_turn_stereo)));
    if (s_capture_turn_lock) {
        vSemaphoreDelete(s_capture_turn_lock);
        s_capture_turn_lock = NULL;
    }
    if (s_capture_ring_lock) {
        vSemaphoreDelete(s_capture_ring_lock);
        s_capture_ring_lock = NULL;
    }
    heap_caps_free(s_capture_turn_stereo);
    s_capture_turn_stereo = NULL;
    heap_caps_free(s_capture_turn_block);
    s_capture_turn_block = NULL;
    heap_caps_free(s_capture_ring);
    s_capture_ring = NULL;
    return ESP_ERR_NO_MEM;
}

void audio_play_ready_chime(void)
{
    /* No boot tone: on this codec/PA path the first I2S TX buffer can stay
     * audible if the host has not started a real playback stream yet. */
}

#define TEST_TONE_HZ 880.0f
#define TEST_TONE_MS 1200
#define TEST_TONE_AMP 9000.0f
#define TEST_TONE_FRAMES 240

static volatile bool s_test_tone_running = false;

static void audio_test_tone_task(void *arg)
{
    (void)arg;
    if (!s_tx) {
        ESP_LOGW(TAG, "test tone skipped: speaker I2S is not ready");
        s_test_tone_running = false;
        vTaskDelete(NULL);
        return;
    }
    if (s_es8311_status != ESP_OK) {
        ESP_LOGW(TAG, "test tone skipped: ES8311 is not ready (%s)", esp_err_to_name(s_es8311_status));
        s_test_tone_running = false;
        vTaskDelete(NULL);
        return;
    }
    audio_set_pa(true);

    ESP_LOGI(TAG, "playing local test tone: %u Hz, stereo s16, %d ms", UAC_SAMPLE_RATE, TEST_TONE_MS);
    const int total_frames = (UAC_SAMPLE_RATE * TEST_TONE_MS) / 1000;
    const int fade_frames = UAC_SAMPLE_RATE / 50;
    int16_t buf[TEST_TONE_FRAMES * 2];

    for (int pos = 0; pos < total_frames; ) {
        int frames = total_frames - pos;
        if (frames > TEST_TONE_FRAMES) {
            frames = TEST_TONE_FRAMES;
        }
        for (int i = 0; i < frames; i++) {
            int frame = pos + i;
            float env = 1.0f;
            if (frame < fade_frames) {
                env = (float)frame / (float)fade_frames;
            } else {
                int remain = total_frames - 1 - frame;
                if (remain < fade_frames) {
                    env = (float)remain / (float)fade_frames;
                }
            }
            float phase = 6.2831853f * TEST_TONE_HZ * (float)frame / (float)UAC_SAMPLE_RATE;
            int16_t sample = (int16_t)(TEST_TONE_AMP * env * sinf(phase));
            buf[2 * i] = sample;
            buf[2 * i + 1] = sample;
        }
        size_t written = 0;
        esp_err_t err = i2s_channel_write(s_tx, buf, frames * 2 * sizeof(int16_t), &written, pdMS_TO_TICKS(100));
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "test tone write failed: %s", esp_err_to_name(err));
            break;
        }
        s_last_spk_us = esp_timer_get_time();
        s_spk_level = 55;
        pos += frames;
    }

    memset(buf, 0, sizeof(buf));
    size_t written = 0;
    (void)i2s_channel_write(s_tx, buf, sizeof(buf), &written, pdMS_TO_TICKS(100));
    vTaskDelay(pdMS_TO_TICKS(120));
    audio_set_pa(false);
    s_spk_level = 0;
    s_test_tone_running = false;
    vTaskDelete(NULL);
}

static void audio_agent_test_tone_task(void *arg)
{
    (void)arg;
    uint32_t src = s_agent_src_hz;
    if (src != 16000 && src != 24000) {
        src = 24000;
    }
    const int ms = 1200;
    const int samples = (int)((src * ms) / 1000);
    const float hz = 440.0f;
    const float amp = 9000.0f;
    int16_t buf[160];
    audio_play_agent_flush();
    audio_play_agent_resume();
    ESP_LOGI(TAG, "playing agent test tone: %u Hz src, 440 Hz, %d ms",
             (unsigned)src, ms);
    for (int pos = 0; pos < samples; ) {
        int n = samples - pos;
        if (n > (int)(sizeof(buf) / sizeof(buf[0]))) {
            n = (int)(sizeof(buf) / sizeof(buf[0]));
        }
        for (int i = 0; i < n; i++) {
            float phase = 6.2831853f * hz * (float)(pos + i) / (float)src;
            buf[i] = (int16_t)(amp * sinf(phase));
        }
        if (audio_play_agent_24k(buf, (size_t)n) != ESP_OK) {
            vTaskDelay(pdMS_TO_TICKS(20));
            (void)audio_play_agent_24k(buf, (size_t)n);
        }
        pos += n;
    }
    audio_play_agent_complete();
    for (int i = 0; i < 80 && !audio_play_agent_idle(); i++) {
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    s_test_tone_running = false;
    vTaskDelete(NULL);
}

void audio_play_agent_test_tone(void)
{
    bool already;
    taskENTER_CRITICAL(&s_audio_mux);
    already = s_test_tone_running;
    if (!already) {
        s_test_tone_running = true;
    }
    taskEXIT_CRITICAL(&s_audio_mux);
    if (already) {
        return;
    }
    if (xTaskCreate(audio_agent_test_tone_task, "kbx_tone24", 4096, NULL, 5, NULL) != pdPASS) {
        taskENTER_CRITICAL(&s_audio_mux);
        s_test_tone_running = false;
        taskEXIT_CRITICAL(&s_audio_mux);
        ESP_LOGW(TAG, "agent test tone task create failed");
    }
}

void audio_play_test_tone(void)
{
    // Atomic check-then-set: prevents two parallel test-tone tasks if CDC
    // ("KBX tone") and the settings-panel button fire at once (otherwise
    // both write interleaved into s_tx and the first finished task turns
    // the PA off under the second).
    bool already;
    taskENTER_CRITICAL(&s_audio_mux);
    already = s_test_tone_running;
    if (!already) {
        s_test_tone_running = true;
    }
    taskEXIT_CRITICAL(&s_audio_mux);
    if (already) {
        return;
    }
    if (xTaskCreate(audio_test_tone_task, "kbx_test_tone", 4096, NULL, 5, NULL) != pdPASS) {
        taskENTER_CRITICAL(&s_audio_mux);
        s_test_tone_running = false;
        taskEXIT_CRITICAL(&s_audio_mux);
        ESP_LOGW(TAG, "test tone task create failed");
    }
}

static esp_err_t uac_input_cb(uint8_t *buf, size_t len, size_t *bytes_read, void *ctx)
{
    (void)ctx;
    if (!buf || !bytes_read || len != CAPTURE_BLOCK_FRAMES * sizeof(int16_t)) {
        if (bytes_read) {
            *bytes_read = 0;
        }
        return ESP_ERR_INVALID_SIZE;
    }

    static capture_block_t block;
    static int16_t mono[CAPTURE_BLOCK_FRAMES];
    const uint32_t latest = capture_ready_sequence();
    if (latest == 0) {
        memset(buf, 0, len);
        *bytes_read = len;
        return ESP_OK;
    }

    uint32_t seq = s_uac_next_sequence;
    if (seq == 0) {
        seq = latest;
    } else {
        const int32_t distance = (int32_t)(latest - seq);
        if (distance >= (int32_t)CAPTURE_READER_MAX_LAG_BLOCKS || distance < 0) {
            seq = latest;
            s_uac_gap_count++;
        }
    }

    if (!capture_copy_block(seq, &block) ||
        block.flags != 0 ||
        block.frames != CAPTURE_BLOCK_FRAMES) {
        memset(buf, 0, len);
        *bytes_read = len;
        s_uac_gap_count++;
        s_uac_next_sequence = capture_sequence_next(latest);
        return ESP_OK;
    }

    s_uac_next_sequence = capture_sequence_next(seq);
    audio_fold_stereo_to_mono(block.stereo, block.frames, mono, NULL);
    if (s_local_mute) {
        memset(mono, 0, sizeof(mono));
    }
    memcpy(buf, mono, sizeof(mono));
    *bytes_read = sizeof(mono);
    return ESP_OK;
}

static void capture_turn_invalidate(audio_capture_fault_t fault)
{
    s_capture_turn.valid = false;
    s_capture_turn.telemetry.valid = false;
    s_capture_turn.telemetry.fault = fault;
    if (fault == AUDIO_CAPTURE_FAULT_GAP) {
        s_capture_turn.telemetry.gap_count++;
    } else if (fault == AUDIO_CAPTURE_FAULT_OVERFLOW) {
        s_capture_turn.telemetry.overflow_count++;
    }
    pcm_dsp_invalidate(&s_capture_turn.dsp);
    s_capture_turn.telemetry.dsp = s_capture_turn.dsp.telemetry;
}

esp_err_t audio_capture_start_16k(void)
{
    if (!s_capture_ring || !s_capture_turn_block || !s_capture_turn_stereo ||
        !s_capture_turn_lock || !s_rx) {
        return ESP_ERR_INVALID_STATE;
    }
    if (audio_is_muted()) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(s_capture_turn_lock, pdMS_TO_TICKS(20)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    if (s_capture_turn.active) {
        xSemaphoreGive(s_capture_turn_lock);
        return ESP_ERR_INVALID_STATE;
    }

    const audio_mute_snapshot_t mute = audio_mute_snapshot();
    if (mute.muted) {
        xSemaphoreGive(s_capture_turn_lock);
        return ESP_ERR_INVALID_STATE;
    }

    memset(&s_capture_turn, 0, sizeof(s_capture_turn));
    const uint32_t latest = capture_ready_sequence();
    s_capture_turn.next_sequence = latest == 0 ? 1 : capture_sequence_next(latest);
    s_capture_turn.mute_epoch = mute.epoch;
    s_capture_turn.active = true;
    s_capture_turn.valid = true;
    s_capture_turn.telemetry.valid = true;
    s_capture_turn.telemetry.first_sequence = s_capture_turn.next_sequence;
    pcm_dsp_init(&s_capture_turn.dsp, PCM_DSP_DEFAULT_GAIN_Q8);
    mic_channel_apply(&s_capture_turn.dsp);
    s_capture_turn.telemetry.dsp = s_capture_turn.dsp.telemetry;
    xSemaphoreGive(s_capture_turn_lock);
    return ESP_OK;
}

esp_err_t audio_capture_read_16k(int16_t frame[AUDIO_CAPTURE_16K_FRAME_SAMPLES],
                                uint32_t timeout_ms,
                                audio_capture_telemetry_t *telemetry)
{
    if (!frame || !s_capture_turn_lock ||
        timeout_ms > AUDIO_CAPTURE_MAX_READ_TIMEOUT_MS) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(frame, 0, AUDIO_CAPTURE_16K_FRAME_BYTES);
    if (xSemaphoreTake(s_capture_turn_lock, pdMS_TO_TICKS(timeout_ms + 20U)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    audio_mute_snapshot_t mute = audio_mute_snapshot();
    const bool mute_changed = mute.muted || mute.epoch != s_capture_turn.mute_epoch;
    if (!s_capture_turn.active || !s_capture_turn.valid || mute_changed) {
        if (s_capture_turn.active && s_capture_turn.valid && mute_changed) {
            capture_turn_invalidate(AUDIO_CAPTURE_FAULT_MUTED);
        }
        if (telemetry) {
            *telemetry = s_capture_turn.telemetry;
        }
        xSemaphoreGive(s_capture_turn_lock);
        return ESP_ERR_INVALID_STATE;
    }

    const TickType_t start = xTaskGetTickCount();
    const TickType_t timeout_ticks = pdMS_TO_TICKS(timeout_ms);
    for (size_t block_index = 0; block_index < CAPTURE_BLOCKS_PER_20MS; ++block_index) {
        const TickType_t elapsed = xTaskGetTickCount() - start;
        const uint32_t remaining_ms = elapsed >= timeout_ticks
                                          ? 0
                                          : (uint32_t)((timeout_ticks - elapsed) * portTICK_PERIOD_MS);
        bool overflow = false;
        const uint32_t sequence = s_capture_turn.next_sequence;
        const esp_err_t err = capture_wait_for_block(sequence,
                                                     s_capture_turn_block,
                                                     remaining_ms,
                                                     &overflow);
        if (err != ESP_OK || s_capture_turn_block->flags != 0 ||
            s_capture_turn_block->frames != CAPTURE_BLOCK_FRAMES) {
            const audio_capture_fault_t fault = overflow
                                                    ? AUDIO_CAPTURE_FAULT_OVERFLOW
                                                    : (err == ESP_ERR_TIMEOUT
                                                           ? AUDIO_CAPTURE_FAULT_TIMEOUT
                                                           : AUDIO_CAPTURE_FAULT_GAP);
            capture_turn_invalidate(fault);
            if (telemetry) {
                *telemetry = s_capture_turn.telemetry;
            }
            xSemaphoreGive(s_capture_turn_lock);
            return err == ESP_OK ? ESP_ERR_INVALID_RESPONSE : err;
        }
        memcpy(&s_capture_turn_stereo[block_index * CAPTURE_BLOCK_FRAMES * 2U],
               s_capture_turn_block->stereo,
               sizeof(s_capture_turn_block->stereo));
        s_capture_turn.telemetry.last_sequence = sequence;
        s_capture_turn.next_sequence = capture_sequence_next(sequence);
    }

    size_t samples = 0;
    mute = audio_mute_snapshot();
    if (mute.muted || mute.epoch != s_capture_turn.mute_epoch) {
        capture_turn_invalidate(AUDIO_CAPTURE_FAULT_MUTED);
        if (telemetry) {
            *telemetry = s_capture_turn.telemetry;
        }
        xSemaphoreGive(s_capture_turn_lock);
        return ESP_ERR_INVALID_STATE;
    }
    if (!pcm_dsp_process_20ms(&s_capture_turn.dsp, s_capture_turn_stereo, PCM_DSP_INPUT_FRAMES_20MS,
                              frame, AUDIO_CAPTURE_16K_FRAME_SAMPLES, &samples,
                              &s_capture_turn.telemetry.dsp) ||
        samples != AUDIO_CAPTURE_16K_FRAME_SAMPLES) {
        capture_turn_invalidate(AUDIO_CAPTURE_FAULT_DSP);
        if (telemetry) {
            *telemetry = s_capture_turn.telemetry;
        }
        xSemaphoreGive(s_capture_turn_lock);
        return ESP_ERR_INVALID_STATE;
    }

    mute = audio_mute_snapshot();
    if (mute.muted || mute.epoch != s_capture_turn.mute_epoch) {
        memset(frame, 0, AUDIO_CAPTURE_16K_FRAME_BYTES);
        capture_turn_invalidate(AUDIO_CAPTURE_FAULT_MUTED);
        if (telemetry) {
            *telemetry = s_capture_turn.telemetry;
        }
        xSemaphoreGive(s_capture_turn_lock);
        return ESP_ERR_INVALID_STATE;
    }

    if (telemetry) {
        *telemetry = s_capture_turn.telemetry;
    }
    xSemaphoreGive(s_capture_turn_lock);
    return ESP_OK;
}

esp_err_t audio_capture_stop_16k(void)
{
    if (!s_capture_turn_lock) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(s_capture_turn_lock,
                       pdMS_TO_TICKS(AUDIO_CAPTURE_MAX_READ_TIMEOUT_MS + 20U)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    s_capture_turn.active = false;
    s_capture_turn.valid = false;
    pcm_dsp_invalidate(&s_capture_turn.dsp);
    s_capture_turn.telemetry.dsp = s_capture_turn.dsp.telemetry;
    xSemaphoreGive(s_capture_turn_lock);
    return ESP_OK;
}

bool audio_capture_valid_16k(void)
{
    if (!s_capture_turn_lock) {
        return false;
    }
    bool valid = false;
    if (xSemaphoreTake(s_capture_turn_lock, pdMS_TO_TICKS(5)) == pdTRUE) {
        valid = s_capture_turn.active && s_capture_turn.valid && s_capture_turn.dsp.valid;
        xSemaphoreGive(s_capture_turn_lock);
    }
    return valid;
}

struct audio_observer {
    const char *loss;   /* why the last read lost the context (KBX wake?) */
    uint32_t gain_q8;   /* kept across resets; 0 = PCM_DSP_DEFAULT_GAIN_Q8 */
    uint32_t next_sequence;
    uint32_t mute_epoch;
    pcm_dsp_t dsp;
    capture_block_t block;
    int16_t stereo[PCM_DSP_INPUT_FRAMES_20MS * 2U];
};

void audio_observer_reset_16k(audio_observer_t *observer)
{
    if (!observer) return;
    const uint32_t gain_q8 = observer->gain_q8;
    const char *loss = observer->loss;
    memset(observer, 0, sizeof(*observer));
    observer->gain_q8 = gain_q8;
    observer->loss = loss;
    uint32_t latest = capture_ready_sequence();
    observer->next_sequence = latest ? capture_sequence_next(latest) : 1;
    observer->mute_epoch = audio_mute_snapshot().epoch;
    pcm_dsp_init(&observer->dsp, gain_q8 ? gain_q8 : PCM_DSP_DEFAULT_GAIN_Q8);
    mic_channel_apply(&observer->dsp);
}

void audio_observer_set_gain_q8(audio_observer_t *observer, uint32_t gain_q8)
{
    if (!observer) return;
    observer->gain_q8 = gain_q8;
    audio_observer_reset_16k(observer);
}

audio_observer_t *audio_observer_create_16k(void)
{
    if (!s_capture_ring || !s_capture_ring_lock) return NULL;
    audio_observer_t *observer = heap_caps_calloc(1, sizeof(*observer),
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (observer) audio_observer_reset_16k(observer);
    return observer;
}

void audio_observer_destroy_16k(audio_observer_t *observer)
{
    if (!observer) return;
    memset(observer, 0, sizeof(*observer));
    heap_caps_free(observer);
}

bool audio_observer_epoch_current(uint32_t epoch)
{
    audio_mute_snapshot_t mute = audio_mute_snapshot();
    return !mute.muted && mute.epoch == epoch;
}

esp_err_t audio_observer_read_16k(audio_observer_t *observer, audio_observer_frame_t *frame)
{
    if (!frame) return ESP_ERR_INVALID_ARG;
    memset(frame, 0, sizeof(*frame));
    if (!observer || !s_capture_ring || !s_capture_ring_lock) return ESP_ERR_INVALID_STATE;
    if (!audio_observer_epoch_current(observer->mute_epoch)) {
        observer->loss = "mute";
        audio_observer_reset_16k(observer);
        return ESP_ERR_INVALID_STATE;
    }
    uint32_t latest = capture_ready_sequence();
    int32_t distance = (int32_t)(latest - observer->next_sequence);
    if (!latest || distance < (int32_t)(CAPTURE_BLOCKS_PER_20MS - 1U))
        return ESP_ERR_NOT_FOUND;
    if (distance >= (int32_t)CAPTURE_RING_BLOCKS) {
        observer->loss = "overrun";
        audio_observer_reset_16k(observer);
        return ESP_ERR_INVALID_STATE;
    }
    const uint32_t start = observer->next_sequence;
    frame->first_sequence = start;
    for (size_t i = 0; i < CAPTURE_BLOCKS_PER_20MS; ++i) {
        const copy_result_t got = capture_try_copy_block(observer->next_sequence, &observer->block);
        if (got == COPY_BUSY) {
            /* The broker holds the ring (it can be preempted by Wi-Fi or
             * lwIP under network load): the blocks are still in the ring,
             * read the same frame again next time. Not a context loss. */
            observer->next_sequence = start;
            memset(observer->stereo, 0, sizeof(observer->stereo));
            memset(&observer->block, 0, sizeof(observer->block));
            memset(frame, 0, sizeof(*frame));
            return ESP_ERR_NOT_FOUND;
        }
        if (got != COPY_OK || observer->block.flags || observer->block.frames != CAPTURE_BLOCK_FRAMES) {
            observer->loss = got != COPY_OK ? "overwritten" : "capture-gap";
            audio_observer_reset_16k(observer);
            memset(frame, 0, sizeof(*frame));
            return ESP_ERR_INVALID_STATE;
        }
        memcpy(observer->stereo + i * CAPTURE_BLOCK_FRAMES * 2U,
            observer->block.stereo, sizeof(observer->block.stereo));
        frame->last_sequence = observer->next_sequence;
        observer->next_sequence = capture_sequence_next(observer->next_sequence);
    }
    size_t samples = 0;
    bool valid = pcm_dsp_process_20ms(&observer->dsp, observer->stereo,
        PCM_DSP_INPUT_FRAMES_20MS, frame->samples, AUDIO_CAPTURE_16K_FRAME_SAMPLES,
        &samples, NULL) && samples == AUDIO_CAPTURE_16K_FRAME_SAMPLES &&
        audio_observer_epoch_current(observer->mute_epoch);
    memset(observer->stereo, 0, sizeof(observer->stereo));
    memset(&observer->block, 0, sizeof(observer->block));
    if (!valid) {
        observer->loss = "dsp";
        audio_observer_reset_16k(observer);
        memset(frame, 0, sizeof(*frame));
        return ESP_ERR_INVALID_STATE;
    }
    frame->mute_epoch = observer->mute_epoch;
    return ESP_OK;
}

const char *audio_observer_loss_reason(const audio_observer_t *observer)
{
    return observer && observer->loss ? observer->loss : "-";
}

static void uac_set_mute_cb(uint32_t mute, void *ctx)
{
    audio_set_mute_state(&s_muted, mute != 0);
}

static void uac_set_volume_cb(uint32_t volume, void *ctx)
{
    if (volume > 100) {
        volume = 100;
    }
    s_volume = volume;
}

esp_err_t audio_init(void)
{
    /* NVS is initialised by net_wifi_init(), which runs before audio_init().
     * Restore before any UI frame reads audio_volume()/audio_local_mute(). */
    audio_persist_restore();
    const esp_timer_create_args_t persist_args = {
        .callback = audio_persist_flush,
        .name = "audio_persist",
    };
    if (esp_timer_create(&persist_args, &s_persist_timer) != ESP_OK) {
        s_persist_timer = NULL;
        ESP_LOGW(TAG, "persist timer unavailable: volume/mute will not survive reboot");
    }
    gpio_reset_pin(AUDIO_PA_CTRL);
    gpio_set_direction(AUDIO_PA_CTRL, GPIO_MODE_OUTPUT);
    gpio_set_level(AUDIO_PA_CTRL, 0);

    /* V2 audio shares one I2S bus between ES7210 ADC and ES8311 DAC. */
    i2s_chan_config_t i2s_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
#if CONFIG_KBX_AUDIO_CAPTURE_TDM4
    /* Four TDM slots double the RX DMA buffers (internal RAM): 6 x 5 ms
     * still covers a 30 ms reader stall and leaves the wake word its heap. */
    i2s_cfg.dma_desc_num = 6;
#else
    i2s_cfg.dma_desc_num = 8;
#endif
    i2s_cfg.dma_frame_num = 240;
    /* Fill TX DMA with silence on underrun instead of looping the last
     * descriptor — otherwise the last audio chunk stutters audibly for
     * ~600 ms after every playback stop. */
    i2s_cfg.auto_clear = true;
    ESP_ERROR_CHECK(i2s_new_channel(&i2s_cfg, &s_tx, &s_rx));
    i2s_std_config_t std = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(UAC_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = AUDIO_I2S_MCLK,
            .bclk = AUDIO_I2S_BCLK,
            .ws   = AUDIO_I2S_LRCK,
            .dout = AUDIO_I2S_DOUT,
            .din  = AUDIO_I2S_DIN,
        },
    };
#if CONFIG_KBX_AUDIO_CAPTURE_TDM4
    /* RLCD 4.2: the ES7210 sends four 16-bit TDM slots (MIC1, MIC2, MIC3 =
     * the speaker loopback, MIC4). TX and RX share BCLK/WS, so both run
     * 64 BCLK per frame with a 50 % WS: the ES8311 sees standard stereo with
     * 32-bit slots and reads the first 16 bits of each. */
    std.slot_cfg.slot_bit_width = I2S_SLOT_BIT_WIDTH_32BIT;
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(s_tx, &std));
    i2s_tdm_config_t tdm = {
        .clk_cfg = I2S_TDM_CLK_DEFAULT_CONFIG(UAC_SAMPLE_RATE),
        .slot_cfg = I2S_TDM_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO,
                        I2S_TDM_SLOT0 | I2S_TDM_SLOT1 | I2S_TDM_SLOT2 | I2S_TDM_SLOT3),
        .gpio_cfg = {
            .mclk = AUDIO_I2S_MCLK,
            .bclk = AUDIO_I2S_BCLK,
            .ws   = AUDIO_I2S_LRCK,
            .dout = I2S_GPIO_UNUSED,
            .din  = AUDIO_I2S_DIN,
        },
    };
    tdm.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;
    tdm.slot_cfg.ws_width = 32;   /* half of the 64-bit frame, like the TX side */
    ESP_ERROR_CHECK(i2s_channel_init_tdm_mode(s_rx, &tdm));
    es7210_min_use_tdm(true);
#else
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(s_tx, &std));
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(s_rx, &std));
#endif
    ESP_ERROR_CHECK(i2s_channel_enable(s_tx));
    ESP_ERROR_CHECK(i2s_channel_enable(s_rx));

    i2c_master_bus_handle_t sys_bus = NULL;
    s_es7210_status = system_i2c_get(&sys_bus);
    if (s_es7210_status == ESP_OK) {
        s_es8311_status = es8311_min_init(sys_bus, UAC_SAMPLE_RATE, SPK_CODEC_VOLUME, &s_es8311_addr);
        s_es7210_status = es7210_min_init(sys_bus, UAC_SAMPLE_RATE, &s_es7210_addr);
    }
    if (s_es8311_status == ESP_OK) {
        audio_set_pa(false);
    } else {
        ESP_LOGW(TAG, "ES8311 init failed: %s (speaker disabled)", esp_err_to_name(s_es8311_status));
    }
    if (s_es7210_status != ESP_OK) {
        ESP_LOGW(TAG, "ES7210 init failed: %s (USB stays up for diagnostics)", esp_err_to_name(s_es7210_status));
    }

    if (!s_i2s_lock) {
        s_i2s_lock = xSemaphoreCreateMutex();
    }
    /* PSRAM, like the agent ring: 12 KB of internal RAM is the wake word's
     * margin next to the pre-dialed socket (dev10). Task-context only. */
    s_spk_rb = xRingbufferCreateWithCaps(SPK_RINGBUF_BYTES, RINGBUF_TYPE_NOSPLIT,
                                         MALLOC_CAP_SPIRAM);
    if (!s_spk_rb) {
        return ESP_ERR_NO_MEM;
    }
    cue_load_settings();
    cue_start_task();
    mic_slot_load();
    if (xTaskCreate(spk_writer_task, "kbx_spk_writer", 4096, NULL, 6, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    /* Agent downlink ring in PSRAM + feeder (M1.4). Non-fatal: without
     * the ring the box stays USB-only as before. */
    s_agent_lock = xSemaphoreCreateMutex();
    if (s_agent_lock) {
        s_agent_rb = xRingbufferCreateWithCaps(AGENT_RING_BYTES, RINGBUF_TYPE_BYTEBUF,
                                               MALLOC_CAP_SPIRAM);
    }
    if (s_agent_rb) {
        if (xTaskCreate(agent_spk_task, "kbx_va_spk", 4096, NULL, 9, NULL) != pdPASS) {
            ESP_LOGW(TAG, "agent-spk task failed - downlink playback off");
            vRingbufferDeleteWithCaps(s_agent_rb);
            s_agent_rb = NULL;
        } else {
            ESP_LOGI(TAG, "agent play src=%u Hz upsample=%dx (KBX spkrate 16|24)",
                     (unsigned)s_agent_src_hz, s_agent_src_hz == 16000 ? 3 : 2);
        }
    } else {
        ESP_LOGW(TAG, "agent ring alloc (PSRAM) failed - downlink playback off");
    }
#if CONFIG_KBX_AUDIO_AEC
    /* Shared echo canceller, created on this (main) stack: ESP-SR's setup
     * needs more room than most tasks have. */
    if (s_aec_enabled && !s_broker_aec) {
        s_broker_aec = aec_stage_create();
        if (s_broker_aec) aec_stage_reset(s_broker_aec, PCM_DSP_CHANNEL_LEFT);
        else ESP_LOGW(TAG, "aec: stage unavailable - half duplex");
    }
#endif
    esp_err_t capture_err = capture_broker_start();
    if (capture_err != ESP_OK) {
        ESP_LOGE(TAG, "capture broker start failed: %s", esp_err_to_name(capture_err));
        return capture_err;
    }

    /* --- USB UAC device --- */
    uac_device_config_t cfg = {
        .skip_tinyusb_init = false,
        .output_cb = uac_output_cb,
        .input_cb = uac_input_cb,
        .set_mute_cb = uac_set_mute_cb,
        .set_volume_cb = uac_set_volume_cb,
        .cb_ctx = NULL,
    };
    ESP_ERROR_CHECK(uac_device_init(&cfg));
    ESP_LOGI(TAG, "audio up: UAC %d Hz, single capture broker, V2 shared I2S, ES7210=%s@0x%02x ES8311=%s@0x%02x",
             UAC_SAMPLE_RATE,
             esp_err_to_name(s_es7210_status),
             s_es7210_addr,
             esp_err_to_name(s_es8311_status),
             s_es8311_addr);
    return ESP_OK;
}

bool audio_aec_active(void)
{
    return s_aec_enabled && s_broker_aec && aec_stage_usable(mic_channel_current());
}

void audio_aec_set_enabled(bool on)
{
    s_aec_enabled = on;
    ESP_LOGI(TAG, "aec %s (applies from the next capture turn / wake reset)", on ? "on" : "off");
}

void audio_aec_log(void)
{
    ESP_LOGI(TAG, "aec: enabled=%d active=%d mic=%s stage=%s", (int)s_aec_enabled,
             (int)audio_aec_active(), mic_slot_name(mic_channel_current()), s_broker_aec ? "yes" : "no");
    aec_stage_log();
}

void audio_duck(uint32_t ms)
{
    s_duck_until_us = esp_timer_get_time() + (int64_t)ms * 1000;
}
