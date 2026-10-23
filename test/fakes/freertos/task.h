#pragma once

#include "freertos/FreeRTOS.h"

void smartLightingTestTaskDelay(TickType_t ticks);

inline void vTaskDelay(TickType_t ticks) { smartLightingTestTaskDelay(ticks); }
