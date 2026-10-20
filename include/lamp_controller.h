#pragma once

#include "lamp.h"
#include "action.h"

class ProvisioningStore;

/* Associates existing LAMP storage and restores settings without touching GPIO. */
void restoreLampSettings(Lamp& lamp, ProvisioningStore& store);

enum class LampOutputTransitionResult : uint8_t {
    COMPLETED,
    STARTED,
    FAILED,
    UNCERTAIN,
    BUSY
};

enum class LampOutputTransitionPollResult : uint8_t {
    NONE,
    EXECUTED,
    FAILED,
    UNCERTAIN
};

enum class LampOutputOffResult : uint8_t {
    APPLIED,
    FAILED
};

LampOutputTransitionResult beginLampOutputTransition(
    Lamp& lamp,
    ActionType commandType,
    int32_t value,
    uint32_t sourceId,
    uint32_t messageId,
    bool coreCommand,
    bool dropAck
);

LampOutputTransitionPollResult pollLampOutputTransition(
    Lamp& lamp,
    uint32_t now
);

/* Stops a running fade, invalidates its generation, and verifies output LOW. */
LampOutputOffResult forceLampOutputOff(Lamp& lamp, uint32_t now,
                                       LampOutputInterruption& interruption);


bool setLampPower(
    Lamp& lamp,
    bool state
);


bool setLampBrightness(
    Lamp& lamp,
    int32_t brightness
);


void setLampAutomatic(
    Lamp& lamp,
    bool enabled
);


void printLampState(
    const Lamp& lamp
);
