#pragma once

#include <stddef.h>

/* Stable ESP32-C6 IEEE 802.15.4 identity, formatted as C6-<16 hex digits>. */
bool readDeviceHardwareId(char* output, size_t outputCapacity);
