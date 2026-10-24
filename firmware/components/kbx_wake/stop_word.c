/*
 * kombify box - spoken "Stop" while a reply plays (the reply must be
 * stoppable by voice, not only by a button).
 *
 * microWakeWord's "stop" model (Kevin Ahrendt, Apache-2.0; the model Home
 * Assistant Voice PE uses to stop replies and timers) runs on its own 16 kHz
 * observer only while the agent's reply plays, and only where echo
 * cancellation removes the reply from the microphone (audio_aec_active).
 * A hit cancels the reply and the request; no new turn starts ("Hi ESP"
 * during the reply is the barge-in that listens again, wakeword.c).
 */
#include <math.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"

#include "audio.h"
#include "net_ws.h"
#include "stop_word.h"
#include "wakeword_mww.h"

static const char *TAG = "kbx_stop";

/* 0.50 (the model's own cutoff) fired on the reply's echo residue and cut
 * replies short; a hit also needs voice evidence below. */
#define STOP_CUTOFF        0.70f
#define STOP_VOICE_US      (2000LL * 1000)   /* voice over the residue this recently */
#define STOP_REFRACTORY_US (1500LL * 1000)
/* Ducking: a frame well over the reply's average echo residue is most
 * likely a voice in the room; the reply drops 12 dB for DUCK_MS so the word
 * comes through. KBX duck <dB> tunes the margin, 0 turns it off. */
#define DUCK_MS            1300U
static volatile float s_duck_margin_db = 15.0f;

extern const uint8_t kbx_mww_stop_start[], kbx_mww_stop_end[];

static uint32_t s_hits, s_frames, s_ducks, s_rejected;
static int64_t s_voice_us;   /* last frame clearly over the echo residue */
static bool s_ready;

static bool reply_playing(void)
{
    return audio_aec_active() && (audio_is_speaking() || !audio_play_agent_idle());
}

static void stop_task(void *arg)
{
    (void)arg;
    const mww_model_t md = {
        .model = kbx_mww_stop_start,
        .model_size = (size_t)(kbx_mww_stop_end - kbx_mww_stop_start),
        .arena_size = 21000,
        .window = 5,
    };
    mww_t *m = NULL;
    audio_observer_t *obs = NULL;
    while (!m || !obs) {
        if (!m && mww_create(&md, STOP_CUTOFF, &m) != ESP_OK) m = NULL;
        if (!obs) obs = audio_observer_create_16k();
        if (!m || !obs) vTaskDelay(pdMS_TO_TICKS(5000));
    }
    s_ready = true;
    ESP_LOGI(TAG, "stop word ready (arena %u B used of %u)", (unsigned)mww_arena_used(m),
             (unsigned)mww_arena_size(m));

    bool was_active = false;
    int64_t quiet_until = 0;
    float floor_db = 0.0f;
    static audio_observer_frame_t frame;
    for (;;) {
        const bool active = reply_playing();
        if (!active) {
            was_active = false;
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }
        if (!was_active) {
            /* fresh start for each reply: no audio from before it */
            audio_observer_reset_16k(obs);
            mww_reset(m);
            was_active = true;
            floor_db = 0.0f;
        }
        const esp_err_t err = audio_observer_read_16k(obs, &frame);
        if (err == ESP_ERR_NOT_FOUND) {
            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }
        if (err != ESP_OK) continue;
        s_frames++;
        /* average echo residue: ~1 s exponential mean of the frame level */
        uint64_t e = 0;
        const int n = (int)(sizeof(frame.samples) / sizeof(frame.samples[0]));
        for (int i = 0; i < n; i++) e += (uint64_t)((int32_t)frame.samples[i] * frame.samples[i]);
        const float db = e ? 10.0f * log10f((float)e / (float)n / (32768.0f * 32768.0f)) : -100.0f;
        floor_db = floor_db == 0.0f ? db : floor_db + 0.05f * (db - floor_db);
        const float margin = s_duck_margin_db;
        if (margin > 0.0f && db > floor_db + margin) {
            audio_duck(DUCK_MS);
            s_ducks++;
            s_voice_us = esp_timer_get_time();
        }
        bool hit = false;
        if (mww_feed(m, frame.samples, (int)(sizeof(frame.samples) / sizeof(frame.samples[0])), &hit) < 0) {
            mww_reset(m);
            continue;
        }
        if (!hit || esp_timer_get_time() < quiet_until) continue;
        quiet_until = esp_timer_get_time() + STOP_REFRACTORY_US;
        if (esp_timer_get_time() - s_voice_us > STOP_VOICE_US) {
            /* the model heard "stop" but nobody spoke over the reply */
            s_rejected++;
            ESP_LOGI(TAG, "\"Stop\" ignored (score %.2f, no voice over the reply)", (double)mww_hit_mean(m));
            mww_reset(m);
            continue;
        }
        s_hits++;
        ESP_LOGI(TAG, "\"Stop\" heard (score %.2f) -> reply stopped", (double)mww_hit_mean(m));
        net_ws_talk_cancel();
        mww_reset(m);
    }
}

esp_err_t stop_word_init(void)
{
    if (xTaskCreatePinnedToCoreWithCaps(stop_task, "kbx_stop", 12288, NULL, 4, NULL, 1,
                                        MALLOC_CAP_SPIRAM) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void stop_word_log(void)
{
    ESP_LOGI(TAG, "stop word: ready=%d active=%d hits=%u rejected=%u frames=%u duck_frames=%u", (int)s_ready,
             (int)reply_playing(), (unsigned)s_hits, (unsigned)s_rejected, (unsigned)s_frames, (unsigned)s_ducks);
}

void stop_word_set_duck_margin(float db)
{
    s_duck_margin_db = db < 0.0f ? 0.0f : db;
    ESP_LOGI(TAG, "duck margin %.1f dB%s", (double)s_duck_margin_db, s_duck_margin_db > 0.0f ? "" : " (off)");
}
