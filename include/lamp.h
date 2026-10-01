#pragma once

#include <Arduino.h>
#include "device.h"


struct LampState {

    bool power;

    uint8_t brightness;

    bool automatic;

};

enum class PairingState : uint8_t {
    UNPAIRED,
    PAIRING,
    PAIRED
};

constexpr uint8_t LAMP_HARDWARE_ID_LENGTH = 32;
constexpr uint8_t LAMP_NAME_LENGTH = 32;
constexpr uint8_t LAMP_FIRMWARE_VERSION_LENGTH = 16;

/* Provisioning identity is separate from Device::id and Device::role. */
struct LampIdentity {
    char hardwareId[LAMP_HARDWARE_ID_LENGTH];
    char name[LAMP_NAME_LENGTH];
    uint32_t parentMainId;
    PairingState pairingState;
};


struct Lamp {

    Device device;

    LampState state;

    LampIdentity identity;

};
