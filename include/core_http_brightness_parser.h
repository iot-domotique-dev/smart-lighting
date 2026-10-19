#pragma once

#include <stddef.h>
#include <stdint.h>

/* Strict JSON body parser shared by CORE-WIFI and native contract tests. */
bool parseCoreBrightnessRequestBody(const char* body, size_t length,
                                    int32_t& percent);
