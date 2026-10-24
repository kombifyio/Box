/*
 * kombify box - acoustic echo cancellation for one 16 kHz capture consumer.
 *
 * The codec loopback is the reference: one ES7210 slot carries what the
 * speaker plays (1.85C V2: the non-microphone slot; RLCD 4.2: MIC3 in TDM
 * slot 1). Both slots are decimated 48 -> 16 kHz at unity gain, ESP-SR's
 * AEC ("MR") removes the echo, and the consumer's gain is applied after it
 * so the echo stays linear for the filter. The AEC chunk rarely equals the
 * 20 ms frame, so a FIFO keeps a constant latency of one chunk.
 */
#include <math.h>
#include <string.h>

#include "esp_afe_aec.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

#include "aec_stage.h"
#include "board.h"

/* Board make-up gain after the AEC (RLCD 4.2: its microphones run 7.5 dB
 * lower to stay linear under playback). */
#ifndef KBX_AEC_POST_GAIN_Q8
#define KBX_AEC_POST_GAIN_Q8 256
#endif

static const char *TAG = "kbx_aec";

#define AEC_FRAME   PCM_DSP_OUTPUT_SAMPLES_20MS
#define AEC_FILTER  4                          /* esp32s3 recommendation */

struct aec_stage {
    afe_aec_handle_t *aec;
    int chunk;
    pcm_dsp_t mic, ref;
    pcm_dsp_channel_t mic_channel;
    int16_t *in;        /* interleaved mic/ref, capacity 2 * (chunk + frame) */
    size_t in_frames;
    int16_t *out;       /* cleaned mono, capacity chunk + 2 * frame */
    size_t out_samples;
    int16_t *aec_out;   /* 16-byte aligned, one chunk */
    int16_t mic16[AEC_FRAME], ref16[AEC_FRAME];
};

/* AFE_TYPE_SR is the linear AEC for recognition; AFE_TYPE_VC adds
 * nonlinear suppression, which removed more echo but bent the voice so far
 * that the wake word stopped firing (measured on the device). SR by default;
 * KBX aec type sr|vc applies to stages created afterwards. */
static afe_type_t s_type = AFE_TYPE_SR;

void aec_stage_set_type_vc(bool vc)
{
    s_type = vc ? AFE_TYPE_VC : AFE_TYPE_SR;
    ESP_LOGI(TAG, "aec type %s for new stages", vc ? "vc" : "sr");
}

/* KBX aec?: energies of the last processed second (any stage). */
static uint64_t s_e_mic, s_e_ref, s_e_out;
static uint32_t s_e_frames;
static float s_db_mic = -120.0f, s_db_ref = -120.0f, s_db_out = -120.0f;
static int s_internal_cost;   /* internal RAM taken by the last stage setup */

static float db_of(uint64_t e, uint32_t n)
{
    if (!e || !n) return -120.0f;
    return 10.0f * log10f((float)e / (float)n / (32768.0f * 32768.0f));
}

void aec_stage_log(void)
{
    ESP_LOGI(TAG, "aec last 1 s (16 kHz, before gain): mic %.1f dBFS, ref %.1f dBFS, out %.1f dBFS; setup took %d B internal",
             (double)s_db_mic, (double)s_db_ref, (double)s_db_out, s_internal_cost);
}

void aec_stage_destroy(aec_stage_t *s)
{
    if (!s) return;
    if (s->aec) afe_aec_destroy(s->aec);
    heap_caps_free(s->in);
    heap_caps_free(s->out);
    heap_caps_free(s->aec_out);
    heap_caps_free(s);
}

aec_stage_t *aec_stage_create(void)
{
    const size_t int_before = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    aec_stage_t *s = heap_caps_calloc(1, sizeof(*s), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s) return NULL;
    s->aec = afe_aec_create("MR", AEC_FILTER, s_type, AFE_MODE_LOW_COST);
    if (!s->aec) {
        ESP_LOGE(TAG, "afe_aec_create failed");
        aec_stage_destroy(s);
        return NULL;
    }
    s->chunk = afe_aec_get_chunksize(s->aec);
    const size_t chunk = (size_t)s->chunk;
    s->in = heap_caps_calloc(2U * (chunk + AEC_FRAME), sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s->out = heap_caps_calloc(chunk + 2U * AEC_FRAME, sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s->aec_out = heap_caps_aligned_calloc(16, chunk, sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s->in || !s->out || !s->aec_out || s->chunk <= 0) {
        aec_stage_destroy(s);
        return NULL;
    }
    aec_stage_reset(s, PCM_DSP_CHANNEL_LEFT);
    ESP_LOGI(TAG, "AEC up: chunk %d samples, filter %d, type %s, internal RAM %d B", s->chunk, AEC_FILTER,
             s_type == AFE_TYPE_VC ? "vc" : "sr",
             s_internal_cost = (int)int_before - (int)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    return s;
}

void aec_stage_reset(aec_stage_t *s, pcm_dsp_channel_t mic)
{
    if (!s) return;
    s->mic_channel = mic;
    pcm_dsp_init(&s->mic, 256U);
    pcm_dsp_init(&s->ref, 256U);
    pcm_dsp_set_channel(&s->mic, mic);
    pcm_dsp_set_channel(&s->ref, mic == PCM_DSP_CHANNEL_LEFT ? PCM_DSP_CHANNEL_RIGHT : PCM_DSP_CHANNEL_LEFT);
    s->in_frames = 0;
    /* Prefill one chunk of silence: constant latency, never an underrun. */
    memset(s->out, 0, (size_t)s->chunk * sizeof(int16_t));
    s->out_samples = (size_t)s->chunk;
}

bool aec_stage_usable(pcm_dsp_channel_t mic)
{
    return mic == PCM_DSP_CHANNEL_LEFT || mic == PCM_DSP_CHANNEL_RIGHT;
}

bool aec_stage_process_20ms(aec_stage_t *s, const int16_t *stereo, int16_t *mono, uint32_t gain_q8)
{
    size_t n = 0;
    if (!pcm_dsp_process_20ms(&s->mic, stereo, PCM_DSP_INPUT_FRAMES_20MS, s->mic16, AEC_FRAME, &n, NULL) ||
        n != AEC_FRAME ||
        !pcm_dsp_process_20ms(&s->ref, stereo, PCM_DSP_INPUT_FRAMES_20MS, s->ref16, AEC_FRAME, &n, NULL) ||
        n != AEC_FRAME) {
        return false;
    }
    int16_t *in = s->in + 2U * s->in_frames;
    for (size_t i = 0; i < AEC_FRAME; ++i) {
        in[2U * i] = s->mic16[i];
        in[2U * i + 1U] = s->ref16[i];
    }
    s->in_frames += AEC_FRAME;

    const size_t chunk = (size_t)s->chunk;
    size_t used = 0;
    while (s->in_frames - used >= chunk) {
        (void)afe_aec_process(s->aec, s->in + 2U * used, s->aec_out);
        memcpy(s->out + s->out_samples, s->aec_out, chunk * sizeof(int16_t));
        s->out_samples += chunk;
        used += chunk;
    }
    if (used) {
        memmove(s->in, s->in + 2U * used, (s->in_frames - used) * 2U * sizeof(int16_t));
        s->in_frames -= used;
    }
    if (s->out_samples < AEC_FRAME) return false;   /* cannot happen after the prefill */

    for (size_t i = 0; i < AEC_FRAME; ++i) {
        s_e_mic += (uint64_t)((int32_t)s->mic16[i] * s->mic16[i]);
        s_e_ref += (uint64_t)((int32_t)s->ref16[i] * s->ref16[i]);
        s_e_out += (uint64_t)((int32_t)s->out[i] * s->out[i]);
    }
    if (++s_e_frames >= 50U) {
        s_db_mic = db_of(s_e_mic, 50U * AEC_FRAME);
        s_db_ref = db_of(s_e_ref, 50U * AEC_FRAME);
        s_db_out = db_of(s_e_out, 50U * AEC_FRAME);
        s_e_mic = s_e_ref = s_e_out = 0;
        s_e_frames = 0;
    }
    for (size_t i = 0; i < AEC_FRAME; ++i) {
        int32_t v = (int32_t)(((int64_t)s->out[i] * gain_q8 * KBX_AEC_POST_GAIN_Q8) / 65536);
        mono[i] = (int16_t)(v > INT16_MAX ? INT16_MAX : v < INT16_MIN ? INT16_MIN : v);
    }
    memmove(s->out, s->out + AEC_FRAME, (s->out_samples - AEC_FRAME) * sizeof(int16_t));
    s->out_samples -= AEC_FRAME;
    return true;
}
