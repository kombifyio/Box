/*
 * kombify box - microWakeWord engine (TensorFlow Lite Micro streaming model
 * on the microWakeWord audio frontend), as ESPHome's micro_wake_word runs it
 * on the ESP32-S3. One instance, one model; wakeword.c owns the start gates,
 * heap floors and crash guard and treats this as a second engine kind next
 * to ESP-SR WakeNet9.
 *
 * Memory: the instance, the tensor arena, the resource-variable arena and the
 * frontend state are allocated in PSRAM. The model stays in the app image.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct mww mww_t;

typedef struct {
    const uint8_t *model;     /* .tflite flatbuffer, 16-byte aligned */
    size_t model_size;
    size_t arena_size;        /* manifest tensor_arena_size; probed upward if short */
    int window;               /* manifest sliding_window_size */
} mww_model_t;

/* Builds the frontend and the interpreter; *out is NULL on failure.
 * ESP_ERR_NO_MEM: allocation failed (retry later); others: model problem. */
esp_err_t mww_create(const mww_model_t *model, float cutoff, mww_t **out);
void mww_destroy(mww_t *m);
/* Clears the frontend, the model's streaming state and the sliding window;
 * no detection for the next 1 s of audio (100 feature slices). */
void mww_reset(mww_t *m);
/* 0.5 ... 0.999; quantized to the model's uint8 output like ESPHome. */
void mww_set_cutoff(mww_t *m, float cutoff);
/* Feeds 16 kHz mono PCM. Returns the number of model invocations (one per
 * 30 ms); *hit is set when the sliding-window mean passed the cutoff, after
 * which the window is cleared (one utterance, one hit). <0 on an invoke error. */
int mww_feed(mww_t *m, const int16_t *pcm, int samples, bool *hit);
/* Last invocation's probability and the window mean at the last hit, 0..1. */
float mww_last_probability(const mww_t *m);
float mww_hit_mean(const mww_t *m);
size_t mww_arena_size(const mww_t *m);
size_t mww_arena_used(const mww_t *m);

#ifdef __cplusplus
}
#endif
