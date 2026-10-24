#include "rtc_contract.h"

#include <stddef.h>

static bool bcd_decode(uint8_t value, unsigned maximum, unsigned *output)
{
    const unsigned high = value >> 4U;
    const unsigned low = value & 0x0FU;
    const unsigned decoded = high * 10U + low;
    if (high > 9U || low > 9U || decoded > maximum) {
        return false;
    }
    *output = decoded;
    return true;
}

static uint8_t bcd_encode(unsigned value)
{
    return (uint8_t)(((value / 10U) << 4U) | (value % 10U));
}

static bool leap_year(unsigned year)
{
    return (year % 4U == 0U && year % 100U != 0U) || year % 400U == 0U;
}

static unsigned days_in_month(unsigned year, unsigned month)
{
    static const uint8_t days[] = {31U, 28U, 31U, 30U, 31U, 30U,
                                   31U, 31U, 30U, 31U, 30U, 31U};
    if (month == 0U || month > 12U) {
        return 0U;
    }
    return month == 2U && leap_year(year) ? 29U : days[month - 1U];
}

static int64_t days_from_civil(int year, unsigned month, unsigned day)
{
    year -= month <= 2U;
    const int era = (year >= 0 ? year : year - 399) / 400;
    const unsigned year_of_era = (unsigned)(year - era * 400);
    const unsigned adjusted_month = month > 2U ? month - 3U : month + 9U;
    const unsigned day_of_year = (153U * adjusted_month + 2U) / 5U + day - 1U;
    const unsigned day_of_era = year_of_era * 365U + year_of_era / 4U - year_of_era / 100U + day_of_year;
    return (int64_t)era * 146097LL + (int64_t)day_of_era - 719468LL;
}

static void civil_from_days(int64_t days, unsigned *year, unsigned *month, unsigned *day)
{
    days += 719468LL;
    const int64_t era = (days >= 0 ? days : days - 146096LL) / 146097LL;
    const unsigned day_of_era = (unsigned)(days - era * 146097LL);
    const unsigned year_of_era = (day_of_era - day_of_era / 1460U + day_of_era / 36524U - day_of_era / 146096U) / 365U;
    int calculated_year = (int)year_of_era + (int)(era * 400LL);
    const unsigned day_of_year = day_of_era - (365U * year_of_era + year_of_era / 4U - year_of_era / 100U);
    const unsigned month_prime = (5U * day_of_year + 2U) / 153U;
    *day = day_of_year - (153U * month_prime + 2U) / 5U + 1U;
    *month = month_prime < 10U ? month_prime + 3U : month_prime - 9U;
    calculated_year += *month <= 2U;
    *year = (unsigned)calculated_year;
}

bool rtc_contract_decode(const uint8_t registers[RTC_CONTRACT_REGISTER_BYTES],
                         uint64_t *unix_seconds)
{
    if (!registers || !unix_seconds || (registers[0] & 0x80U) != 0U ||
        (registers[1] & 0x80U) != 0U || (registers[2] & 0xC0U) != 0U ||
        (registers[3] & 0xC0U) != 0U || (registers[4] & 0xF8U) != 0U ||
        (registers[5] & 0xE0U) != 0U) {
        return false;
    }
    unsigned second = 0, minute = 0, hour = 0, day = 0, weekday = 0, month = 0, year_offset = 0;
    if (!bcd_decode(registers[0], 59U, &second) ||
        !bcd_decode(registers[1], 59U, &minute) ||
        !bcd_decode(registers[2], 23U, &hour) ||
        !bcd_decode(registers[3], 31U, &day) ||
        !bcd_decode(registers[4], 6U, &weekday) ||
        !bcd_decode(registers[5], 12U, &month) ||
        !bcd_decode(registers[6], 99U, &year_offset)) {
        return false;
    }
    (void)weekday;
    const unsigned year = 2000U + year_offset;
    if (day == 0U || month == 0U || day > days_in_month(year, month)) {
        return false;
    }
    const int64_t days = days_from_civil((int)year, month, day);
    if (days < 0) {
        return false;
    }
    const uint64_t result = (uint64_t)days * 86400ULL +
                            (uint64_t)hour * 3600ULL +
                            (uint64_t)minute * 60ULL + second;
    if (result < RTC_CONTRACT_MIN_UNIX_SECONDS || result > RTC_CONTRACT_MAX_UNIX_SECONDS) {
        return false;
    }
    *unix_seconds = result;
    return true;
}

bool rtc_contract_encode(uint64_t unix_seconds,
                         uint8_t registers[RTC_CONTRACT_REGISTER_BYTES])
{
    if (!registers || unix_seconds < RTC_CONTRACT_MIN_UNIX_SECONDS ||
        unix_seconds > RTC_CONTRACT_MAX_UNIX_SECONDS) {
        return false;
    }
    const uint64_t days = unix_seconds / 86400ULL;
    uint64_t remainder = unix_seconds % 86400ULL;
    unsigned year = 0, month = 0, day = 0;
    civil_from_days((int64_t)days, &year, &month, &day);
    if (year < 2000U || year > 2099U) {
        return false;
    }
    const unsigned hour = (unsigned)(remainder / 3600ULL);
    remainder %= 3600ULL;
    const unsigned minute = (unsigned)(remainder / 60ULL);
    const unsigned second = (unsigned)(remainder % 60ULL);
    const unsigned weekday = (unsigned)((days + 4ULL) % 7ULL);
    registers[0] = bcd_encode(second);
    registers[1] = bcd_encode(minute);
    registers[2] = bcd_encode(hour);
    registers[3] = bcd_encode(day);
    registers[4] = bcd_encode(weekday);
    registers[5] = bcd_encode(month);
    registers[6] = bcd_encode(year - 2000U);
    return true;
}

bool rtc_contract_read_pair_valid(uint64_t first_unix_seconds,
                                  uint64_t second_unix_seconds)
{
    return first_unix_seconds >= RTC_CONTRACT_MIN_UNIX_SECONDS &&
           second_unix_seconds >= first_unix_seconds &&
           second_unix_seconds - first_unix_seconds <= 2ULL;
}

bool rtc_contract_resync_valid(uint64_t rtc_unix_seconds,
                               uint64_t current_unix_seconds,
                               uint64_t last_synced_unix_seconds)
{
    return rtc_unix_seconds >= RTC_CONTRACT_MIN_UNIX_SECONDS &&
           rtc_unix_seconds <= RTC_CONTRACT_MAX_UNIX_SECONDS &&
           current_unix_seconds <= RTC_CONTRACT_MAX_UNIX_SECONDS &&
           last_synced_unix_seconds <= RTC_CONTRACT_MAX_UNIX_SECONDS &&
           rtc_unix_seconds + RTC_CONTRACT_RESYNC_TOLERANCE_SECONDS >=
               current_unix_seconds &&
           rtc_unix_seconds + RTC_CONTRACT_RESYNC_TOLERANCE_SECONDS >=
               last_synced_unix_seconds;
}
