#include "secure_zero.h"

void secure_zero(void *buffer, size_t length)
{
    volatile unsigned char *bytes = (volatile unsigned char *)buffer;
    while (bytes && length > 0U) {
        *bytes++ = 0U;
        --length;
    }
}
