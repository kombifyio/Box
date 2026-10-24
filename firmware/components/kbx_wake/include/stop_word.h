#pragma once
#include "esp_err.h"

/* "Stop" while a reply plays (stop_word.c); needs echo cancellation. */
esp_err_t stop_word_init(void);

/* KBX stop? */
void stop_word_log(void);

/* KBX duck <dB>: voice margin over the echo residue for ducking, 0 = off. */
void stop_word_set_duck_margin(float db);
