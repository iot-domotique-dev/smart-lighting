#pragma once

#include <Arduino.h>
#include "roles.h"


enum class DeviceStatus {

    OFFLINE,

    ONLINE

};

/* Capabilities are descriptive flags; defining one does not implement it. */
enum DeviceCapability : uint32_t {
    DEVICE_CAP_POWER = 1UL << 0,
    DEVICE_CAP_BRIGHTNESS = 1UL << 1,
    DEVICE_CAP_AUTOMATIC = 1UL << 2,
    DEVICE_CAP_LIGHTING = 1UL << 3,
    DEVICE_CAP_GROUPS = 1UL << 4,
    DEVICE_CAP_SCENES = 1UL << 5,
    DEVICE_CAP_AUTOMATION = 1UL << 6,
    DEVICE_CAP_ALARM = 1UL << 7,
    DEVICE_CAP_PRESENCE = 1UL << 8,
    DEVICE_CAP_DOOR_SENSOR = 1UL << 9,
    DEVICE_CAP_SECURITY_MODE = 1UL << 10
};

enum class LastConfirmedPower : uint8_t {
    UNKNOWN,
    OFF,
    ON
};

enum class LastConfirmedPowerStatus : uint8_t {
    UNKNOWN,
    CONFIRMED,
    STALE
};


struct Device {

    uint32_t id;

    const char* name;

    DeviceRole role;

    DeviceStatus status;

    uint32_t lastSeen;

    uint32_t capabilities;

    uint32_t parentId;

    /* ID in the owner's namespace; Device::id is the CORE-wide ID. */
    uint32_t localId;

    /* Historical software result of SET_POWER, never electrical readback. */
    LastConfirmedPower lastConfirmedPower;
    LastConfirmedPowerStatus lastConfirmedPowerStatus;
    /* Sticky until reboot: an expired command may still execute late. */
    bool powerExecutionUnknown;

};

inline void markLastConfirmedPowerStale(Device& device) {
    if (device.role == DeviceRole::LAMP &&
        device.lastConfirmedPower != LastConfirmedPower::UNKNOWN &&
        device.lastConfirmedPowerStatus == LastConfirmedPowerStatus::CONFIRMED) {
        device.lastConfirmedPowerStatus = LastConfirmedPowerStatus::STALE;
    }
}

inline bool hasDeviceCapability(
    const Device& device,
    DeviceCapability capability
) {
    return (device.capabilities & static_cast<uint32_t>(capability)) != 0;
}
