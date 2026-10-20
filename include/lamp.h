#pragma once

#include <Arduino.h>
#include "device.h"

class ProvisioningStore;


struct LampState {

    bool power;

    uint8_t brightness;

    bool automatic;

    /* Persisted last positive level, or 100% when absent/invalid. */
    uint8_t lastNonzeroBrightness = 100;

};

enum class PairingState : uint8_t {
    UNPAIRED,
    PAIRING,
    PAIRED,
    UNPAIRING
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
    bool removalPending;
};

/* Runtime-only identity for the one output transition currently run by LAMP. */
struct LampOutputTransition {
    bool active = false;
    bool faulted = false;
    bool coreCommand = false;
    bool dropAck = false;
    bool targetPower = false;
    uint8_t targetBrightness = 0;
    uint8_t previousBrightness = 0;
    uint32_t startedAt = 0;
    uint32_t timeoutMs = 0;
    uint32_t completionSequence = 0;
    uint32_t generation = 0;
    uint32_t sourceId = 0;
    uint32_t messageId = 0;
};

struct LampOutputInterruption {
    LampOutputTransition transition = {};
    bool hadTransition = false;
    bool transitionCompleted = false;
    bool transitionStopped = false;
};


struct Lamp {

    Device device;

    LampState state;

    LampIdentity identity;

    /* Non-owning handle to this physical LAMP's existing provisioning store. */
    ProvisioningStore* settingsStore = nullptr;

    LampOutputTransition outputTransition;

    /* Local generation invalidates any completion event from an older fade. */
    uint32_t outputGeneration = 0;

};
