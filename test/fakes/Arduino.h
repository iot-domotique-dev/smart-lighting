#pragma once

#include <stdint.h>
#include <stddef.h>
#include <string.h>

class FakeHardwareSerial {
public:
    // Counts actual successful SET_POWER executions, including idempotent ones.
    uint32_t powerExecutionCount = 0;

    void print(const char* text) {
        if (text != nullptr && strcmp(text, "Power -> ") == 0) {
            ++powerExecutionCount;
        }
    }

    template <size_t N>
    void print(const char (&text)[N]) { print(static_cast<const char*>(text)); }

    template <typename T>
    void print(const T&) {}

    template <typename T>
    void println(const T&) {}

    void println() {}
};

extern FakeHardwareSerial Serial;

uint32_t millis();
