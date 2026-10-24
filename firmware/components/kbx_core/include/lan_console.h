#pragma once
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

/* The KBX command table belongs to the USB console; the LAN console runs a
 * line through it and returns the log output that line produced. */
typedef struct {
    bool (*capture_begin)(char *buf, size_t cap);
    void (*exec_line)(char *line);
    size_t (*capture_end)(void);
} lan_console_commands_t;

/* LAN console (lan_console.c): signed remote KBX commands and firmware push
 * over Wi-Fi. Starts only when a key is stored. */
esp_err_t lan_console_init(const lan_console_commands_t *commands);

/* CDC "KBX remote ..." arguments: "key <64 hex>", "off", "?". USB only. */
bool lan_console_cdc_command(const char *args);
