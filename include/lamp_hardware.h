#pragma once

#include <stdint.h>

enum class LampHardwareFadeStartResult : uint8_t {
    STARTED,
    UNSUPPORTED,
    FAILED
};

void initializeLampHardware();
bool writeLampHardwarePower(bool enabled, uint8_t lastNonzeroBrightness);
bool writeLampHardwareBrightness(uint8_t percent);
bool lampHardwareSupportsBrightness();
bool lampHardwareSupportsFade();
LampHardwareFadeStartResult startLampHardwareFade(uint8_t percent,
                                                  uint16_t durationMs,
                                                  uint32_t generation);
uint32_t lampHardwareFadeCompletionSequence();
bool lampHardwareFadeCompletionMatches(uint32_t& observedSequence,
                                       uint8_t percent,
                                       uint32_t generation);
bool lampHardwareFadeAtTarget(uint8_t percent);
bool stopLampHardwareFade();

#if !defined(ESP_PLATFORM)
/* Native-only output simulation used to exercise PWM and driver failures. */
void configureLampHardwareSimulation(bool brightnessSupported,
                                     uint8_t allowedMaximumPercent = 100,
                                     bool fadeSupported = true);
void failNextLampHardwareWrite();
void failNextLampHardwareFadeStart();
void failNextLampHardwareFadeStop();
void simulateLampHardwareFadeCompletion();
void simulateLampHardwareReachTargetWithoutCallback();
void simulateLampHardwareFadeCallback(uint8_t reportedPercent);
void simulateLampHardwareFadeCallbackForGeneration(uint8_t reportedPercent,
                                                   uint32_t generation);
bool simulatedLampHardwareFadeActive();
uint16_t simulatedLampHardwareFadeDuration();
uint8_t simulatedLampHardwareOutput();
#endif
