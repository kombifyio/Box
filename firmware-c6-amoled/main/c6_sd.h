/*
 * kombify box C6 - optional microSD capture of wake candidates and a
 * rotating persistent log. Off by default, switched with `KBX sd on|off`
 * (persisted), local only: nothing on the card is ever uploaded.
 *
 * The card sits on GPIO 10/11/18/6, but the AMOLED's QSPI bus owns the C6's
 * only general-purpose SPI peripheral, so the card is driven by a bit-banged
 * SPI-mode protocol and mounted through FatFs. Every card access runs in one
 * low-priority task; the voice and log paths only post messages and never
 * wait for the card.
 *
 *   /sdcard/kbx/wake/wNNNNN.wav   first 2 s sent for a wake candidate (16 kHz mono)
 *   /sdcard/kbx/wake.csv          seq,uptime_s,epoch,phrase,score,verdict,verify_ms,file
 *   /sdcard/kbx/log/log.0..3.txt  the console log, newest in log.0 (256 KB each)
 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

/* Loads the persisted switch; starts the card task when it is on. */
void c6_sd_init(void);
bool c6_sd_enabled(void);
/* Persists the switch. On: starts the task and mounts the card at the next
 * candidate or log line; off: closes the files and unmounts. */
esp_err_t c6_sd_set_enabled(bool on);

/* A wake turn starts / sends a 20 ms frame of the uplink / ends. verdict is
 * "accepted", "rejected", "local" (no server check) or "failed". All three
 * return at once; frames beyond the first 2 s are ignored. */
void c6_sd_wake_begin(const char *phrase, float score);
void c6_sd_wake_frame(const int16_t *pcm, size_t samples);
void c6_sd_wake_end(const char *verdict, int verify_ms);

/* `KBX sd?` and `KBX sd ls`. */
void c6_sd_log_status(void);
void c6_sd_log_list(void);
