#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Select a non-decreasing trusted time or reject an RTC rollback. */
bool rtc_monotonic_select(uint64_t candidate_unix,
                          uint64_t current_unix,
                          bool current_trusted,
                          uint64_t persistent_high_water,
                          uint64_t *selected_unix);
