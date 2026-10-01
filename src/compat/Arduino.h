#pragma once

/* Small compatibility surface for the existing Arduino-style application.
 * The C6 firmware itself is built on ESP-IDF so ESP-Zigbee SDK 2.x can be
 * linked without pulling in a mismatched Arduino/IDF framework pair.
 */
#include <inttypes.h>
#include <stdio.h>
#include <type_traits>

#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

class IdFSerialCompat {
public:
    void begin(unsigned long) {}

    size_t print(const char* value) {
        return value == nullptr ? 0 : static_cast<size_t>(printf("%s", value));
    }
    size_t print(char value) { return static_cast<size_t>(printf("%c", value)); }

    template <typename T,
              typename std::enable_if<std::is_integral<T>::value, int>::type = 0>
    size_t print(T value) {
        if constexpr (std::is_same<T, bool>::value) {
            return static_cast<size_t>(printf("%s", value ? "1" : "0"));
        } else if constexpr (std::is_signed<T>::value) {
            return static_cast<size_t>(printf("%lld", static_cast<long long>(value)));
        } else {
            return static_cast<size_t>(printf("%llu", static_cast<unsigned long long>(value)));
        }
    }

    size_t println() { return static_cast<size_t>(printf("\r\n")); }
    size_t println(const char* value) {
        const size_t written = print(value);
        return written + println();
    }
    size_t println(char value) {
        const size_t written = print(value);
        return written + println();
    }

    template <typename T,
              typename std::enable_if<std::is_integral<T>::value, int>::type = 0>
    size_t println(T value) {
        const size_t written = print(value);
        return written + println();
    }
};

inline IdFSerialCompat Serial;

inline uint32_t millis() {
    return static_cast<uint32_t>(esp_timer_get_time() / 1000ULL);
}

inline void delay(uint32_t milliseconds) {
    vTaskDelay(pdMS_TO_TICKS(milliseconds));
}
