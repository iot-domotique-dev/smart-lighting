#pragma once

#include <stddef.h>

#include "freertos/FreeRTOS.h"

struct SmartLightingFakeSemaphore { bool taken; };
using SemaphoreHandle_t = SmartLightingFakeSemaphore*;

namespace smart_freertos_test {
inline SmartLightingFakeSemaphore* pool() {
    static SmartLightingFakeSemaphore values[256] = {};
    return values;
}
inline size_t& nextSemaphore() {
    static size_t index = 0;
    return index;
}
inline void resetSemaphores() {
    nextSemaphore() = 0;
    for (size_t index = 0; index < 256; ++index) pool()[index].taken = false;
}
}  // namespace smart_freertos_test

inline SemaphoreHandle_t xSemaphoreCreateMutex() {
    size_t& next = smart_freertos_test::nextSemaphore();
    if (next >= 256) return nullptr;
    SmartLightingFakeSemaphore* result = &smart_freertos_test::pool()[next++];
    result->taken = false;
    return result;
}
inline BaseType_t xSemaphoreTake(SemaphoreHandle_t semaphore, TickType_t) {
    if (semaphore == nullptr || semaphore->taken) return pdFALSE;
    semaphore->taken = true;
    return pdTRUE;
}
inline BaseType_t xSemaphoreGive(SemaphoreHandle_t semaphore) {
    if (semaphore == nullptr || !semaphore->taken) return pdFALSE;
    semaphore->taken = false;
    return pdTRUE;
}
