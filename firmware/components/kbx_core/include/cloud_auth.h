#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#define CLOUD_AUTH_TOKEN_MAX 8192

typedef struct {
    bool paired;
    bool pairing;
    char user_code[32];
    char verification_uri[160];
    char verification_uri_complete[256];
    int expires_in;
    char error_code[64];
} cloud_auth_status_t;

/* Init after NVS. No network runs on the caller of init/start/forget/status. */
esp_err_t cloud_auth_init(void);
esp_err_t cloud_auth_start_pairing(void);
esp_err_t cloud_auth_forget(void);
/* Blocking: call only from the network lifecycle task, never CDC or LVGL.
 * The output is cleared on every failure; caller must erase its copy after use. */
esp_err_t cloud_auth_access_token(char *out, size_t capacity);
void cloud_auth_get_status(cloud_auth_status_t *out);
/* Seconds the cached access token stays valid (0 = none). Lets the voice
 * loop refresh it in the background before a tap needs it. */
int cloud_auth_access_valid_s(void);
/* Lend a caller-owned buffer for the next renewals instead of a heap
 * allocation (boards without PSRAM, while the buffer is otherwise unused).
 * NULL takes it back; false while a renewal is still using it. */
bool cloud_auth_set_work_area(void *area, size_t size);
/* Bytes a work area must hold. */
size_t cloud_auth_work_size(void);
