/* Host preview stub: fake monotonic clock driven by the harness. */
#pragma once
#include <stdint.h>
#include "esp_err.h"
int64_t esp_timer_get_time(void);
typedef struct esp_timer *esp_timer_handle_t;
typedef struct { void (*callback)(void *); void *arg; const char *name; } esp_timer_create_args_t;
static inline esp_err_t esp_timer_create(const esp_timer_create_args_t *a, esp_timer_handle_t *h) { (void)a; *h = 0; return ESP_FAIL; }
static inline esp_err_t esp_timer_start_periodic(esp_timer_handle_t h, uint64_t p) { (void)h; (void)p; return ESP_OK; }
