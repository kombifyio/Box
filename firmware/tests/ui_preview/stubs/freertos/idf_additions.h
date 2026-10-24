#pragma once
#include "FreeRTOS.h"
static inline BaseType_t xTaskCreatePinnedToCoreWithCaps(void (*fn)(void *), const char *n, uint32_t st,
    void *arg, int prio, TaskHandle_t *h, int core, unsigned caps)
{ (void)n; (void)st; (void)prio; (void)h; (void)core; (void)caps; (void)fn; (void)arg; return 0; }
static inline void vTaskDeleteWithCaps(TaskHandle_t t) { (void)t; }
