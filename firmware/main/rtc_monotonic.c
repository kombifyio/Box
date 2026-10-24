#include "rtc_monotonic.h"

#include "rtc_contract.h"

bool rtc_monotonic_select(uint64_t candidate_unix,
                          uint64_t current_unix,
                          bool current_trusted,
                          uint64_t persistent_high_water,
                          uint64_t *selected_unix)
{
    if (!selected_unix || candidate_unix < RTC_CONTRACT_MIN_UNIX_SECONDS ||
        candidate_unix > RTC_CONTRACT_MAX_UNIX_SECONDS ||
        persistent_high_water > RTC_CONTRACT_MAX_UNIX_SECONDS ||
        (current_trusted && current_unix > RTC_CONTRACT_MAX_UNIX_SECONDS)) {
        return false;
    }
    if (persistent_high_water != 0U &&
        candidate_unix + RTC_CONTRACT_RESYNC_TOLERANCE_SECONDS <
            persistent_high_water) {
        return false;
    }
    if (current_trusted &&
        candidate_unix + RTC_CONTRACT_RESYNC_TOLERANCE_SECONDS < current_unix) {
        return false;
    }

    uint64_t selected = candidate_unix;
    if (persistent_high_water > selected) {
        selected = persistent_high_water;
    }
    if (current_trusted && current_unix > selected) {
        selected = current_unix;
    }
    *selected_unix = selected;
    return true;
}
