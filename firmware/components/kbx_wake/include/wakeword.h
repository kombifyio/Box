/*
 * kombify box - local wake word: ESP-SR WakeNet9 phrases ("Hi ESP" default)
 * and microWakeWord phrases ("Kombify", "Hey Kombify", "Hey Kubi",
 * "Hey Jarvis"); one engine and one model run at a time.
 *
 * The detector is a passive reader of the existing microphone broker
 * (audio_observer_*). Audio never leaves the device until the phrase fires;
 * a hit then enters exactly the start-only path of a tap
 * (net_ws_talk_start_for_wakeword), including the pre-dialed socket.
 * Listening pauses while muted, while a turn or playback runs and while the
 * box cannot talk; nothing is ever buffered for the cloud before a hit.
 *
 * Qualification (KBX wake ...):
 *   test mode counts and logs hits without starting a turn, so false
 *   accepts per hour (ambient, TV, conversation) and the hit rate at a
 *   distance can be measured; every hit logs its score-independent input
 *   level (peak dBFS of the last 1.5 s) and the counters live in KBX wake?.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Default phrase; the selected one is wakeword_phrase_text(wakeword_get_phrase()). */
#define WAKEWORD_PHRASE "Hi ESP"

typedef struct {
    bool enabled;        /* owner setting (NVS), default off */
    bool ready;          /* model loaded, detector running */
    bool listening;      /* currently observing the microphone */
    bool test_mode;      /* hits are counted only (qualification) */
    esp_err_t error;     /* last load/run error, ESP_OK when fine */
    uint32_t hits;       /* detections since boot */
} wakeword_status_t;

esp_err_t wakeword_init(void);
/* Nonblocking; persistence and model load run on the detector task. */
esp_err_t wakeword_set_enabled(bool enabled);
void wakeword_set_test_mode(bool on);
/* 0 = the phrase default; otherwise 0.50 ... 0.999 (higher = fewer false
 * accepts). Per phrase; microWakeWord quantizes it to the model's uint8 output. */
esp_err_t wakeword_set_threshold(float threshold);
void wakeword_get_status(wakeword_status_t *status);
void wakeword_reset_counters(void);
void wakeword_log_status(void);
/* Phrase selection (Quick Settings, KBX wake phrase <id>), persisted. A
 * switch applies without a reboot: the running engine is rebuilt with the
 * new model through the normal start gates. */
int wakeword_phrase_count(void);
const char *wakeword_phrase_id(int index);
const char *wakeword_phrase_text(int index);
int wakeword_get_phrase(void);
esp_err_t wakeword_set_phrase(int index);
esp_err_t wakeword_set_phrase_id(const char *id);
void wakeword_log_phrases(void);
/* assist.voice "auto": true when the phrase answers with a male voice
 * (jarvis, hey_jarvis, mycroft, computer). */
bool wakeword_phrase_male_voice(int index);
/* KBX wake gain <1-16|0>: microWakeWord input gain (x1 default, persisted);
 * ESP-SR keeps the uplink's 14x. Applies at the next engine start. */
esp_err_t wakeword_set_mww_gain(unsigned gain);
unsigned wakeword_mww_gain(void);
/* Crash-isolation variants (dev11), persisted: istack, noext, noarm,
 * mode90, copy, nofloor, core0; "clear" resets all. */
esp_err_t wakeword_set_opt(const char *name, bool on);
void wakeword_log_opts(void);

#ifdef __cplusplus
}
#endif
