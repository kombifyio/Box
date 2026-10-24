#include "c6_audio.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "bsp/esp-bsp.h"
#include "esp_codec_dev.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"

static const char *TAG = "c6_audio";

#define MIC_GAIN_DB 30.0f
#define DEFAULT_VOLUME 60

static esp_codec_dev_handle_t s_mic;
static esp_codec_dev_handle_t s_spk;
static SemaphoreHandle_t s_lock;
static bool s_mic_open;
static bool s_spk_open;
static int s_volume = DEFAULT_VOLUME;
static volatile uint8_t s_level;

static void track_level(const int16_t *pcm, size_t samples)
{
    int peak = 0;
    for (size_t i = 0; i < samples; ++i) {
        int v = abs((int)pcm[i]);
        if (v > peak) peak = v;
    }
    /* Roughly logarithmic so quiet speech still moves the orb. */
    int level = peak >= 16384 ? 100 : peak >= 8192 ? 85 : peak >= 4096 ? 70 :
        peak >= 2048 ? 55 : peak >= 1024 ? 40 : peak >= 512 ? 25 : peak >= 256 ? 10 : 0;
    s_level = (uint8_t)level;
}

static void load_volume(void)
{
    nvs_handle_t h;
    if (nvs_open("c6_audio", NVS_READONLY, &h) != ESP_OK) return;
    int32_t v = DEFAULT_VOLUME;
    if (nvs_get_i32(h, "volume", &v) == ESP_OK && v >= 0 && v <= 100) s_volume = (int)v;
    nvs_close(h);
}

esp_err_t c6_audio_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) return ESP_ERR_NO_MEM;
    load_volume();
    s_spk = bsp_audio_codec_speaker_init();
    s_mic = bsp_audio_codec_microphone_init();
    /* bsp_audio_init switches the amplifier on; keep it silent until a reply. */
    (void)bsp_audio_poweramp_enable(false);
    if (!s_spk || !s_mic) {
        ESP_LOGE(TAG, "codec init failed (spk=%p mic=%p)", s_spk, s_mic);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "ES8311 ready, volume %d", s_volume);
    return ESP_OK;
}

esp_err_t c6_audio_mic_open(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t err = ESP_OK;
    if (s_spk_open) {
        err = ESP_ERR_INVALID_STATE;
    } else if (!s_mic_open) {
        esp_codec_dev_sample_info_t fs = {
            .bits_per_sample = 16, .channel = 1, .sample_rate = C6_MIC_RATE,
        };
        err = esp_codec_dev_open(s_mic, &fs) == ESP_CODEC_DEV_OK ? ESP_OK : ESP_FAIL;
        if (err == ESP_OK) {
            esp_codec_dev_set_in_gain(s_mic, MIC_GAIN_DB);
            s_mic_open = true;
        }
    }
    xSemaphoreGive(s_lock);
    return err;
}

esp_err_t c6_audio_mic_read(int16_t *pcm, size_t samples)
{
    if (!s_mic_open) return ESP_ERR_INVALID_STATE;
    int rc = esp_codec_dev_read(s_mic, pcm, (int)(samples * sizeof(int16_t)));
    if (rc != ESP_CODEC_DEV_OK) return ESP_FAIL;
    track_level(pcm, samples);
    return ESP_OK;
}

void c6_audio_mic_close(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_mic_open) {
        esp_codec_dev_close(s_mic);
        s_mic_open = false;
    }
    s_level = 0;
    xSemaphoreGive(s_lock);
}

esp_err_t c6_audio_spk_open(uint32_t rate)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t err = ESP_OK;
    if (s_mic_open) {
        err = ESP_ERR_INVALID_STATE;
    } else if (!s_spk_open) {
        esp_codec_dev_sample_info_t fs = {
            .bits_per_sample = 16, .channel = 1, .sample_rate = rate,
        };
        err = esp_codec_dev_open(s_spk, &fs) == ESP_CODEC_DEV_OK ? ESP_OK : ESP_FAIL;
        if (err == ESP_OK) {
            esp_codec_dev_set_out_vol(s_spk, s_volume);
            (void)bsp_audio_poweramp_enable(true);
            s_spk_open = true;
        }
    }
    xSemaphoreGive(s_lock);
    return err;
}

esp_err_t c6_audio_spk_write(const int16_t *pcm, size_t samples)
{
    if (!s_spk_open) return ESP_ERR_INVALID_STATE;
    track_level(pcm, samples);
    int rc = esp_codec_dev_write(s_spk, (void *)pcm, (int)(samples * sizeof(int16_t)));
    return rc == ESP_CODEC_DEV_OK ? ESP_OK : ESP_FAIL;
}

void c6_audio_spk_close(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_spk_open) {
        (void)bsp_audio_poweramp_enable(false);
        esp_codec_dev_close(s_spk);
        s_spk_open = false;
    }
    s_level = 0;
    xSemaphoreGive(s_lock);
}

static int block_rms(const int16_t *pcm, size_t n)
{
    double sum = 0;
    for (size_t i = 0; i < n; ++i) sum += (double)pcm[i] * pcm[i];
    return (int)sqrt(sum / (double)n);
}

esp_err_t c6_audio_loopback_test(int *quiet_rms, int *tone_rms)
{
    /* Both directions at 16 kHz on the shared clock: 0.3 s silence, then a
     * soft 1 kHz tone for 0.4 s while the microphone records. */
    enum { N = 320, QUIET = 15, TONE = 20 };
    static int16_t out[N], in[N];
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool busy = s_mic_open || s_spk_open;
    xSemaphoreGive(s_lock);
    if (busy) return ESP_ERR_INVALID_STATE;
    esp_codec_dev_sample_info_t fs = { .bits_per_sample = 16, .channel = 1, .sample_rate = C6_MIC_RATE };
    if (esp_codec_dev_open(s_spk, &fs) != ESP_CODEC_DEV_OK) return ESP_FAIL;
    if (esp_codec_dev_open(s_mic, &fs) != ESP_CODEC_DEV_OK) {
        esp_codec_dev_close(s_spk);
        return ESP_FAIL;
    }
    esp_codec_dev_set_in_gain(s_mic, MIC_GAIN_DB);
    esp_codec_dev_set_out_vol(s_spk, 30);
    (void)bsp_audio_poweramp_enable(true);
    long quiet = 0, tone = 0;
    int phase = 0;
    for (int b = 0; b < QUIET + TONE; ++b) {
        for (int i = 0; i < N; ++i) {
            out[i] = b < QUIET ? 0 : (int16_t)(6000.0 * sin(2.0 * M_PI * 1000.0 * phase / C6_MIC_RATE));
            phase++;
        }
        esp_codec_dev_write(s_spk, out, sizeof(out));
        esp_codec_dev_read(s_mic, in, sizeof(in));
        int rms = block_rms(in, N);
        if (b >= 3 && b < QUIET) quiet += rms;
        if (b >= QUIET + 3) tone += rms;
    }
    memset(out, 0, sizeof(out));
    esp_codec_dev_write(s_spk, out, sizeof(out));
    (void)bsp_audio_poweramp_enable(false);
    esp_codec_dev_close(s_mic);
    esp_codec_dev_close(s_spk);
    *quiet_rms = (int)(quiet / (QUIET - 3));
    *tone_rms = (int)(tone / (TONE - 3));
    return ESP_OK;
}

void c6_audio_set_volume(int volume)
{
    if (volume < 0) volume = 0;
    if (volume > 100) volume = 100;
    s_volume = volume;
    if (s_spk_open) esp_codec_dev_set_out_vol(s_spk, volume);
    nvs_handle_t h;
    if (nvs_open("c6_audio", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_i32(h, "volume", volume);
        nvs_commit(h);
        nvs_close(h);
    }
}

int c6_audio_volume(void)
{
    return s_volume;
}

uint8_t c6_audio_level(void)
{
    return s_level;
}
