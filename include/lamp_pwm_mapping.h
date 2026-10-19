#pragma once

#include <stdint.h>

constexpr uint32_t lampPwmAllowedCeilingDuty(
    uint32_t maximumDuty, uint8_t allowedMaximumPercent) {
    return (maximumDuty * allowedMaximumPercent + 50U) / 100U;
}

constexpr uint32_t lampPwmScaleToCeiling(uint32_t ceilingDuty,
                                         uint8_t requestedPercent) {
    return (ceilingDuty * requestedPercent + 50U) / 100U;
}

constexpr int lampPwmOffLevel(bool activeLow) {
    return activeLow ? 1 : 0;
}

constexpr int lampPwmPowerLevel(bool activeLow, bool enabled) {
    return activeLow ? (enabled ? 0 : 1) : (enabled ? 1 : 0);
}

constexpr bool lampPwmOutputInvert(bool activeLow) {
    return activeLow;
}

constexpr uint32_t lampPwmDutyFromPercent(uint32_t maximumDuty,
                                          uint8_t allowedMaximumPercent,
                                          uint8_t requestedPercent) {
    return requestedPercent == 0
        ? 0U
        : (lampPwmScaleToCeiling(
               lampPwmAllowedCeilingDuty(maximumDuty,
                                         allowedMaximumPercent),
               requestedPercent) == 0U
               ? 1U
               : lampPwmScaleToCeiling(
                     lampPwmAllowedCeilingDuty(maximumDuty,
                                               allowedMaximumPercent),
                     requestedPercent));
}
