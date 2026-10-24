/*
 * kombify box C6 - half-duplex audio on the ES8311 codec.
 *
 * Microphone and speaker share one I2S port, so the clock follows the open
 * direction: 16 kHz capture for the uplink, the reply rate (24 kHz) for
 * playback. Only one direction is open at a time. The power amplifier
 * (TCA9554 pin 7) is off except while a reply plays.
 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#define C6_MIC_RATE 16000
#define C6_REPLY_RATE 24000

esp_err_t c6_audio_init(void);

esp_err_t c6_audio_mic_open(void);
/* Blocking read of exactly `samples` mono S16 samples. */
esp_err_t c6_audio_mic_read(int16_t *pcm, size_t samples);
void c6_audio_mic_close(void);

esp_err_t c6_audio_spk_open(uint32_t rate);
esp_err_t c6_audio_spk_write(const int16_t *pcm, size_t samples);
void c6_audio_spk_close(void);

/* Speaker volume 0..100, persisted in NVS. */
void c6_audio_set_volume(int volume);
int c6_audio_volume(void);

/* Self-test (KBX loop?): soft 1 kHz tone on the speaker while the microphone
 * records; returns the mic RMS in silence and during the tone. */
esp_err_t c6_audio_loopback_test(int *quiet_rms, int *tone_rms);

/* Peak level of the last processed block, 0..100. */
uint8_t c6_audio_level(void);
