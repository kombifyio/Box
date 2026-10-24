#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define MEDIA_TURN_PATH "/v1/box-media/turn"
#define MEDIA_INPUT_CONTENT_TYPE "audio/L16; rate=16000; channels=1"
#define MEDIA_OUTPUT_CONTENT_TYPE "audio/L16; rate=48000; channels=1"
#define MEDIA_INPUT_MIN_BYTES 9600U
#define MEDIA_INPUT_MAX_BYTES 480000U
#define MEDIA_OUTPUT_MAX_BYTES 2880000U
#define MEDIA_ID_MAX_BYTES 128U
#define MEDIA_TOKEN_MIN_BYTES 32U
#define MEDIA_TOKEN_MAX_BYTES 512U
#define MEDIA_SHA256_HEX_BYTES 64U
#define MEDIA_UUID_TEXT_BYTES 36U

typedef struct {
    int status_code;
    int64_t content_length;
    bool chunked;
    bool duplicate_header;
    const char *content_type;
    const char *content_encoding;
    const char *location;
    const char *transfer_encoding;
    const char *device_id;
    const char *pairing_id;
    const char *request_id;
    const char *server_instance_id;
    const char *input_sha256;
    const char *output_sha256;
    const char *replayed;
} media_response_contract_t;

bool media_contract_endpoint_valid(const char *endpoint);
bool media_contract_id_valid(const char *value);
bool media_contract_token_valid(const char *value);
bool media_contract_sha256_valid(const char *value);

bool media_contract_pairing_values_valid(const char *endpoint,
                                         const char *token,
                                         const char *device_id,
                                         const char *pairing_id);

bool media_contract_uuid_v7(uint64_t unix_ms,
                            const uint8_t random_bytes[10],
                            char output[MEDIA_UUID_TEXT_BYTES + 1U]);

void media_contract_sha256_hex(const uint8_t digest[32],
                               char output[MEDIA_SHA256_HEX_BYTES + 1U]);

void media_contract_pcm16le_to_l16(const int16_t *samples,
                                   size_t sample_count,
                                   uint8_t *output);

size_t media_contract_l16_to_stereo(const uint8_t *l16,
                                    size_t l16_bytes,
                                    int16_t *stereo,
                                    size_t stereo_frame_capacity);

bool media_contract_response_valid(const media_response_contract_t *response,
                                   const char *expected_device_id,
                                   const char *expected_pairing_id,
                                   const char *expected_request_id,
                                   const char *expected_input_sha256);
