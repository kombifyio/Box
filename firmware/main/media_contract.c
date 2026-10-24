#include "media_contract.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

static bool parse_ipv4_octet(const char **cursor, unsigned *value)
{
    const char *p = *cursor;
    if (!isdigit((unsigned char)*p)) {
        return false;
    }
    if (*p == '0' && isdigit((unsigned char)p[1])) {
        return false;
    }
    unsigned parsed = 0;
    unsigned digits = 0;
    while (isdigit((unsigned char)*p)) {
        parsed = parsed * 10U + (unsigned)(*p - '0');
        if (parsed > 255U || ++digits > 3U) {
            return false;
        }
        p++;
    }
    *cursor = p;
    *value = parsed;
    return true;
}

bool media_contract_endpoint_valid(const char *endpoint)
{
    static const char prefix[] = "https://";
    if (!endpoint || strncmp(endpoint, prefix, sizeof(prefix) - 1U) != 0) {
        return false;
    }
    const char *p = endpoint + sizeof(prefix) - 1U;
    unsigned octets[4] = {0};
    for (size_t index = 0; index < 4U; ++index) {
        if (!parse_ipv4_octet(&p, &octets[index])) {
            return false;
        }
        if (index < 3U) {
            if (*p++ != '.') {
                return false;
            }
        }
    }
    const bool private_address = octets[0] == 10U ||
                                 (octets[0] == 172U && octets[1] >= 16U && octets[1] <= 31U) ||
                                 (octets[0] == 192U && octets[1] == 168U);
    if (!private_address || *p++ != ':') {
        return false;
    }
    if (!isdigit((unsigned char)*p)) {
        return false;
    }
    unsigned port = 0;
    unsigned port_digits = 0;
    while (isdigit((unsigned char)*p)) {
        port = port * 10U + (unsigned)(*p - '0');
        if (port > 65535U || ++port_digits > 5U) {
            return false;
        }
        p++;
    }
    return port > 0U && strcmp(p, MEDIA_TURN_PATH) == 0;
}

bool media_contract_id_valid(const char *value)
{
    if (!value) {
        return false;
    }
    const size_t length = strlen(value);
    if (length == 0U || length > MEDIA_ID_MAX_BYTES) {
        return false;
    }
    for (size_t index = 0; index < length; ++index) {
        const unsigned char character = (unsigned char)value[index];
        if (!isalnum(character) && character != '.' && character != '_' &&
            character != '-' && character != ':') {
            return false;
        }
    }
    return true;
}

bool media_contract_token_valid(const char *value)
{
    if (!value) {
        return false;
    }
    const size_t length = strlen(value);
    if (length < MEDIA_TOKEN_MIN_BYTES || length > MEDIA_TOKEN_MAX_BYTES) {
        return false;
    }
    for (size_t index = 0; index < length; ++index) {
        const unsigned char character = (unsigned char)value[index];
        if (!isalnum(character) && character != '-' && character != '.' &&
            character != '_' && character != '~' && character != '+' &&
            character != '/' && character != '=') {
            return false;
        }
    }
    return true;
}

bool media_contract_pairing_values_valid(const char *endpoint,
                                         const char *token,
                                         const char *device_id,
                                         const char *pairing_id)
{
    return media_contract_endpoint_valid(endpoint) &&
           media_contract_token_valid(token) &&
           media_contract_id_valid(device_id) &&
           media_contract_id_valid(pairing_id);
}

bool media_contract_sha256_valid(const char *value)
{
    if (!value || strlen(value) != MEDIA_SHA256_HEX_BYTES) {
        return false;
    }
    for (size_t index = 0; index < MEDIA_SHA256_HEX_BYTES; ++index) {
        const char character = value[index];
        if (!((character >= '0' && character <= '9') ||
              (character >= 'a' && character <= 'f'))) {
            return false;
        }
    }
    return true;
}

bool media_contract_uuid_v7(uint64_t unix_ms,
                            const uint8_t random_bytes[10],
                            char output[MEDIA_UUID_TEXT_BYTES + 1U])
{
    if (!random_bytes || !output || unix_ms == 0U || unix_ms > 0xFFFFFFFFFFFFULL) {
        return false;
    }
    uint8_t value[16];
    value[0] = (uint8_t)(unix_ms >> 40U);
    value[1] = (uint8_t)(unix_ms >> 32U);
    value[2] = (uint8_t)(unix_ms >> 24U);
    value[3] = (uint8_t)(unix_ms >> 16U);
    value[4] = (uint8_t)(unix_ms >> 8U);
    value[5] = (uint8_t)unix_ms;
    memcpy(&value[6], random_bytes, 10U);
    value[6] = (uint8_t)((value[6] & 0x0FU) | 0x70U);
    value[8] = (uint8_t)((value[8] & 0x3FU) | 0x80U);
    const int written = snprintf(output,
                                 MEDIA_UUID_TEXT_BYTES + 1U,
                                 "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
                                 value[0], value[1], value[2], value[3],
                                 value[4], value[5], value[6], value[7],
                                 value[8], value[9], value[10], value[11],
                                 value[12], value[13], value[14], value[15]);
    return written == (int)MEDIA_UUID_TEXT_BYTES;
}

void media_contract_sha256_hex(const uint8_t digest[32],
                               char output[MEDIA_SHA256_HEX_BYTES + 1U])
{
    static const char hex[] = "0123456789abcdef";
    if (!digest || !output) {
        return;
    }
    for (size_t index = 0; index < 32U; ++index) {
        output[index * 2U] = hex[digest[index] >> 4U];
        output[index * 2U + 1U] = hex[digest[index] & 0x0FU];
    }
    output[MEDIA_SHA256_HEX_BYTES] = '\0';
}

void media_contract_pcm16le_to_l16(const int16_t *samples,
                                   size_t sample_count,
                                   uint8_t *output)
{
    if (!samples || !output) {
        return;
    }
    for (size_t index = 0; index < sample_count; ++index) {
        const uint16_t sample = (uint16_t)samples[index];
        output[index * 2U] = (uint8_t)(sample >> 8U);
        output[index * 2U + 1U] = (uint8_t)sample;
    }
}

size_t media_contract_l16_to_stereo(const uint8_t *l16,
                                    size_t l16_bytes,
                                    int16_t *stereo,
                                    size_t stereo_frame_capacity)
{
    if (!l16 || !stereo || l16_bytes == 0U || (l16_bytes & 1U) != 0U) {
        return 0U;
    }
    size_t frames = l16_bytes / 2U;
    if (frames > stereo_frame_capacity) {
        frames = stereo_frame_capacity;
    }
    for (size_t index = 0; index < frames; ++index) {
        const uint16_t wire = (uint16_t)((uint16_t)l16[index * 2U] << 8U) |
                              (uint16_t)l16[index * 2U + 1U];
        const int16_t sample = (int16_t)wire;
        stereo[index * 2U] = sample;
        stereo[index * 2U + 1U] = sample;
    }
    return frames;
}

static bool absent(const char *value)
{
    return !value || value[0] == '\0';
}

bool media_contract_response_valid(const media_response_contract_t *response,
                                   const char *expected_device_id,
                                   const char *expected_pairing_id,
                                   const char *expected_request_id,
                                   const char *expected_input_sha256)
{
    if (!response || !expected_device_id || !expected_pairing_id ||
        !expected_request_id || !expected_input_sha256) {
        return false;
    }
    const bool length_valid = response->content_length > 0 &&
                              response->content_length <= (int64_t)MEDIA_OUTPUT_MAX_BYTES &&
                              (response->content_length & 1) == 0;
    return response->status_code == 200 && length_valid && !response->chunked &&
           !response->duplicate_header &&
           response->content_type && strcmp(response->content_type, MEDIA_OUTPUT_CONTENT_TYPE) == 0 &&
           absent(response->content_encoding) && absent(response->location) &&
           absent(response->transfer_encoding) &&
           response->device_id && strcmp(response->device_id, expected_device_id) == 0 &&
           response->pairing_id && strcmp(response->pairing_id, expected_pairing_id) == 0 &&
           response->request_id && strcmp(response->request_id, expected_request_id) == 0 &&
           response->server_instance_id && media_contract_id_valid(response->server_instance_id) &&
           response->input_sha256 && strcmp(response->input_sha256, expected_input_sha256) == 0 &&
           response->output_sha256 && media_contract_sha256_valid(response->output_sha256) &&
           response->replayed && strcmp(response->replayed, "false") == 0;
}
