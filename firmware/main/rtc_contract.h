#pragma once

#include <stdbool.h>
#include <stdint.h>

#define RTC_CONTRACT_REGISTER_BYTES 7U
#define RTC_CONTRACT_MIN_UNIX_SECONDS 1767225600ULL /* 2026-01-01T00:00:00Z */
#define RTC_CONTRACT_MAX_UNIX_SECONDS 4102444799ULL /* 2099-12-31T23:59:59Z */
#define RTC_CONTRACT_RESYNC_TOLERANCE_SECONDS 2ULL

bool rtc_contract_decode(const uint8_t registers[RTC_CONTRACT_REGISTER_BYTES],
                         uint64_t *unix_seconds);

bool rtc_contract_encode(uint64_t unix_seconds,
                         uint8_t registers[RTC_CONTRACT_REGISTER_BYTES]);

bool rtc_contract_read_pair_valid(uint64_t first_unix_seconds,
                                  uint64_t second_unix_seconds);

bool rtc_contract_resync_valid(uint64_t rtc_unix_seconds,
                               uint64_t current_unix_seconds,
                               uint64_t last_synced_unix_seconds);
