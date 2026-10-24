#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Wipe sensitive bytes through volatile stores so optimization cannot elide it. */
void secure_zero(void *buffer, size_t length);

#ifdef __cplusplus
}
#endif
