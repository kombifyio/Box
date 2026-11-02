#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "pcm_dsp.h"

#ifdef __cplusplus
extern "C" {
#endif

#define AUDIO_CAPTURE_16K_FRAME_SAMPLES PCM_DSP_OUTPUT_SAMPLES_20MS
#define AUDIO_CAPTURE_16K_FRAME_BYTES PCM_DSP_OUTPUT_BYTES_20MS
#define AUDIO_CAPTURE_FRAME_DURATION_MS 20U
#define AUDIO_CAPTURE_MAX_READ_TIMEOUT_MS 100U

typedef enum {
    AUDIO_CAPTURE_FAULT_NONE = 0,
    AUDIO_CAPTURE_FAULT_MUTED,
    AUDIO_CAPTURE_FAULT_GAP,
    AUDIO_CAPTURE_FAULT_OVERFLOW,
    AUDIO_CAPTURE_FAULT_TIMEOUT,
    AUDIO_CAPTURE_FAULT_DSP,
} audio_capture_fault_t;

typedef struct {
    bool valid;
    audio_capture_fault_t fault;
    uint32_t first_sequence;
    uint32_t last_sequence;
    uint32_t gap_count;
    uint32_t overflow_count;
    pcm_dsp_telemetry_t dsp;
} audio_capture_telemetry_t;

esp_err_t audio_init(void);
bool audio_is_speaking(void);
bool audio_is_muted(void);         // host mute OR box-local mute
uint32_t audio_mic_level(void);   // 0..100
uint32_t audio_speaker_level(void); // 0..100
uint32_t audio_volume(void);      // 0..100
bool audio_speaker_ready(void);
void audio_play_ready_chime(void);
void audio_play_test_tone(void);
void audio_play_agent_test_tone(void);
void audio_log_mic_status(void);
void audio_log_play_status(void);
uint32_t audio_adjust_volume(int delta); // returns new 0..100 local volume
uint32_t audio_set_volume(uint32_t volume); // absolute 0..100 set (UI slider); returns clamped value

// Box-local mute (touch toggle): independent of host mute. When set, the mic
// samples sent to the host are zeroed (host hears silence) and the UI shows
// "mute". The box cannot push mute upstream over UAC, so this is host-invisible
// except as silence.
void audio_set_local_mute(bool muted);
void audio_toggle_local_mute(void);
bool audio_local_mute(void);

/*
 * Single-consumer, fail-closed stream of 20 ms mono S16LE frames at 16 kHz.
 * The owner must stop the stream after any read error before starting again.
 */
/*
 * Agent downlink (M1.4): raw 24 kHz S16LE mono frames from the voice-agent WS.
 * play: into the PSRAM jitter ring (non-blocking; ESP_ERR_NO_MEM when full).
 * flush: discard the ring (interrupted / barge-in / teardown). idle: ring
 * empty and feeder waiting (turn finished playing).
 */
esp_err_t audio_play_agent_24k(const int16_t *pcm, size_t samples);
void audio_play_agent_flush(void);
/* Qualified end of this output stream: play even a short queued reply and
 * treat its final drain as EOF. Flush starts a new stream and clears EOF. */
void audio_play_agent_complete(void);
void audio_play_agent_holdoff(void);
void audio_play_agent_resume(void);
bool audio_play_agent_idle(void);
uint32_t audio_play_agent_src_hz(void);
esp_err_t audio_play_agent_set_src_hz(uint32_t hz);

/* Listening earcons (dev18): non-blocking; the microphone is gated while a
 * cue plays. Setting and style persist (NVS kbx_assist). Style ids:
 * "chime" (default), "pop", "glass". */
typedef enum { AUDIO_CUE_LISTEN_START = 0, AUDIO_CUE_LISTEN_END, AUDIO_CUE_COUNT } audio_cue_t;
typedef enum {
    AUDIO_CUE_STYLE_CHIME = 0,
    AUDIO_CUE_STYLE_POP,
    AUDIO_CUE_STYLE_GLASS,
    AUDIO_CUE_STYLE_BELL,   /* ~350 ms, the clearest one */
    AUDIO_CUE_STYLE_COUNT
} audio_cue_style_t;
void audio_cue_play(audio_cue_t cue);
bool audio_cues_enabled(void);
void audio_cues_set_enabled(bool on);
audio_cue_style_t audio_cue_style(void);
const char *audio_cue_style_id(audio_cue_style_t style);
/* Sets the style and plays a start-cue preview. */
esp_err_t audio_cue_set_style_id(const char *id);

esp_err_t audio_capture_start_16k(void);
esp_err_t audio_capture_read_16k(int16_t frame[AUDIO_CAPTURE_16K_FRAME_SAMPLES],
                                uint32_t timeout_ms,
                                audio_capture_telemetry_t *telemetry);
esp_err_t audio_capture_stop_16k(void);
bool audio_capture_valid_16k(void);

/* Passive reader of the existing microphone broker. It has its own cursor
 * and DSP state and never acquires or starts the single-consumer voice turn.
 * The low-priority observer owns its PSRAM state; no microphone copy escapes
 * mute or a mute-epoch change. NOT_FOUND means wait for the next 20 ms block
 * (also while the broker holds the ring: the frame is read again later);
 * any other error requires resetting the detector's accumulated context and
 * audio_observer_loss_reason() names it (mute, overrun, overwritten,
 * capture-gap, dsp). */
typedef struct audio_observer audio_observer_t;
typedef struct {
    int16_t samples[AUDIO_CAPTURE_16K_FRAME_SAMPLES];
    uint32_t first_sequence;
    uint32_t last_sequence;
    uint32_t mute_epoch;
} audio_observer_frame_t;
audio_observer_t *audio_observer_create_16k(void);
void audio_observer_destroy_16k(audio_observer_t *observer);
void audio_observer_reset_16k(audio_observer_t *observer);
esp_err_t audio_observer_read_16k(audio_observer_t *observer, audio_observer_frame_t *frame);
const char *audio_observer_loss_reason(const audio_observer_t *observer);
/* Recheck immediately before accepting a detection, after inference. */
bool audio_observer_epoch_current(uint32_t epoch);
/* Software gain of this observer's DSP (Q8, 0 = the uplink default 14x);
 * resets the observer. The microWakeWord frontend wants the unboosted level. */
void audio_observer_set_gain_q8(audio_observer_t *observer, uint32_t gain_q8);

/* Microphone slot (dev21): KBX mic slot left|right|mix|auto (persisted;
 * auto = learned this boot, else the mix of both slots), KBX mic slot?, and
 * KBX mic probe (per-slot raw level for 3 s). */
esp_err_t audio_mic_set_slot(const char *name);
void audio_mic_log_slot(void);
void audio_mic_probe_start(void);

/* KBX tdm?: per-slot level of the TDM capture (RLCD 4.2). */
void audio_tdm_log(void);

/* Echo cancellation against the codec loopback (aec_stage.c). Active when
 * enabled and the microphone slot is known; then the microphone listens
 * through cues and playback (full duplex). KBX aec on|off|?. */
bool audio_aec_active(void);
void audio_aec_set_enabled(bool on);
void audio_aec_log(void);

/* Play the agent's reply 12 dB lower for ms (voice heard during a reply). */
void audio_duck(uint32_t ms);

#ifdef __cplusplus
}
#endif
