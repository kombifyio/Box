#pragma once

/* Bounded lifetime of a pre-minted ("prepared") voice session.
 *
 * A prepared session holds a Gateway budget reservation and one of the
 * account's concurrent SpeechKit session slots, but bills nothing until a
 * provider connects after the start frame (SpeechKit settles an unstarted
 * session at zero on DELETE or ticket expiry). The ticket expires after at
 * most KBX_WARM_TICKET_MAX_S. The box therefore keeps at most one prepared
 * session, only for the follow-up window after a turn or a fresh pairing,
 * and deletes it KBX_WARM_DIAL_MARGIN_S before the ticket expires so the WS
 * upgrade still has time to reach the server. Pure for the native test. */

#include <stdbool.h>
#include <stdint.h>

#define KBX_WARM_TICKET_MAX_S INT64_C(90)
#define KBX_WARM_DIAL_MARGIN_S INT64_C(20)
/* 2024-01-01: wall clock is known only after SNTP. */
#define KBX_WARM_CLOCK_VALID_EPOCH INT64_C(1704067200)

static inline bool kbx_warm_digits(const char *s, int n, int64_t *out)
{
    int64_t v = 0;
    for (int i = 0; i < n; ++i) {
        if (s[i] < '0' || s[i] > '9') return false;
        v = v * 10 + (s[i] - '0');
    }
    *out = v;
    return true;
}

/* Days since 1970-01-01 for a proleptic Gregorian date (Howard Hinnant). */
static inline int64_t kbx_warm_days_from_civil(int64_t y, int64_t m, int64_t d)
{
    y -= m <= 2 ? 1 : 0;
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    int64_t yoe = y - era * 400;
    int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

/* RFC 3339 "YYYY-MM-DDTHH:MM:SS[.frac](Z|+hh:mm|-hh:mm)" -> Unix seconds. */
static inline bool kbx_warm_rfc3339(const char *s, int64_t *out)
{
    int64_t y, mo, d, h, mi, sec, oh = 0, om = 0;
    if (!s || !kbx_warm_digits(s, 4, &y) || s[4] != '-' || !kbx_warm_digits(s + 5, 2, &mo) ||
        s[7] != '-' || !kbx_warm_digits(s + 8, 2, &d) || (s[10] != 'T' && s[10] != 't') ||
        !kbx_warm_digits(s + 11, 2, &h) || s[13] != ':' || !kbx_warm_digits(s + 14, 2, &mi) ||
        s[16] != ':' || !kbx_warm_digits(s + 17, 2, &sec)) {
        return false;
    }
    if (mo < 1 || mo > 12 || d < 1 || d > 31 || h > 23 || mi > 59 || sec > 60) return false;
    const char *p = s + 19;
    if (*p == '.') {
        ++p;
        if (*p < '0' || *p > '9') return false;
        while (*p >= '0' && *p <= '9') ++p;
    }
    int sign = 0;
    if ((*p == 'Z' || *p == 'z') && p[1] == '\0') {
        sign = 0;
    } else if ((*p == '+' || *p == '-') && kbx_warm_digits(p + 1, 2, &oh) && p[3] == ':' &&
               kbx_warm_digits(p + 4, 2, &om) && p[6] == '\0' && oh <= 23 && om <= 59) {
        sign = *p == '+' ? 1 : -1;
    } else {
        return false;
    }
    int64_t t = kbx_warm_days_from_civil(y, mo, d) * 86400 + h * 3600 + mi * 60 + sec;
    *out = t - sign * (oh * 3600 + om * 60);
    return true;
}

/* Remaining ticket lifetime in seconds, never above the server's documented
 * maximum. Without a valid wall clock or a parsable expiry the documented
 * maximum is assumed; a past expiry yields 0. */
static inline int64_t kbx_warm_ticket_ttl_s(const char *expires_at, int64_t now_epoch)
{
    int64_t expiry;
    if (now_epoch < KBX_WARM_CLOCK_VALID_EPOCH || !kbx_warm_rfc3339(expires_at, &expiry))
        return KBX_WARM_TICKET_MAX_S;
    int64_t ttl = expiry - now_epoch;
    if (ttl < 0) return 0;
    return ttl > KBX_WARM_TICKET_MAX_S ? KBX_WARM_TICKET_MAX_S : ttl;
}

/* Monotonic deadline after which a prepared session must be deleted instead
 * of dialed; 0 = not worth keeping at all. */
static inline int64_t kbx_warm_usable_until_us(int64_t minted_us, int64_t ttl_s)
{
    if (ttl_s <= KBX_WARM_DIAL_MARGIN_S) return 0;
    return minted_us + (ttl_s - KBX_WARM_DIAL_MARGIN_S) * INT64_C(1000000);
}

static inline bool kbx_warm_usable(int64_t usable_until_us, int64_t now_us)
{
    return usable_until_us > 0 && now_us < usable_until_us;
}
