/* Host preview: libc gaps against ESP-IDF's newlib (glibc < 2.38 has no
 * strlcpy, Windows lacks setenv/localtime_r). Force-included. */
#pragma once
#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static inline size_t kbx_strlcpy(char *d, const char *s, size_t n)
{
    size_t l = strlen(s);
    if (n) { size_t c = l < n - 1 ? l : n - 1; memcpy(d, s, c); d[c] = '\0'; }
    return l;
}
#define strlcpy kbx_strlcpy

static inline char *kbx_strcasestr(const char *h, const char *n)
{
    size_t l = strlen(n);
    for (; *h; h++) {
        size_t i = 0;
        while (i < l && h[i] && tolower((unsigned char)h[i]) == tolower((unsigned char)n[i])) i++;
        if (i == l) return (char *)h;
    }
    return NULL;
}
#define strcasestr kbx_strcasestr

#if defined(_WIN32)
#define setenv(k, v, o) _putenv_s((k), (v))
#define tzset _tzset
#define localtime_r(t, tm) (localtime_s((tm), (t)) ? NULL : (tm))
#endif
