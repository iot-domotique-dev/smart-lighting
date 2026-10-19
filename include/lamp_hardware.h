#pragma once

#include <stdint.h>

void initializeLampHardware();
bool writeLampHardwarePower(bool enabled);
bool writeLampHardwareBrightness(uint8_t percent);
bool lampHardwareSupportsBrightness();

#if !defined(ESP_PLATFORM)
/* Native-only output simulation used to exercise PWM and driver failures. */
void configureLampHardwareSimulation(bool brightnessSupported,
                                     uint8_t allowedMaximumPercent = 100);
void failNextLampHardwareWrite();
uint8_t simulatedLampHardwareOutput();
#endif
