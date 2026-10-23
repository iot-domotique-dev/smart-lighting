#pragma once

#include <stdint.h>

using TickType_t = uint32_t;
using BaseType_t = int;

constexpr BaseType_t pdTRUE = 1;
constexpr BaseType_t pdFALSE = 0;
constexpr TickType_t portMAX_DELAY = 0xffffffffUL;

#define pdMS_TO_TICKS(ms) (static_cast<TickType_t>(ms))
