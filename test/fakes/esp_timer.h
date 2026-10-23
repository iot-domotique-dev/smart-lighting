#pragma once

#include <stdint.h>

uint32_t millis();

inline int64_t esp_timer_get_time() {
    return static_cast<int64_t>(millis()) * 1000;
}
