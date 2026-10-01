#pragma once

#include "lamp.h"


constexpr uint8_t MAX_LAMPS = 10;


struct LampRegistry {

    Lamp lamps[MAX_LAMPS];

    uint8_t count;

};


void initLampRegistry(
    LampRegistry& registry
);


Lamp* findLamp(
    LampRegistry& registry,
    uint32_t id
);

Lamp* findLampByDeviceId(
    LampRegistry& registry,
    uint32_t deviceId
);


bool addLamp(
    LampRegistry& registry,
    const Lamp& lamp
);

Lamp* findLampByHardwareId(
    LampRegistry& registry,
    const char* hardwareId
);

uint8_t findLampsByParentMainId(
    LampRegistry& registry,
    uint32_t parentMainId,
    Lamp** matches,
    uint8_t capacity
);

bool removeLamp(
    LampRegistry& registry,
    uint32_t deviceId
);


void printLampRegistry(
    const LampRegistry& registry
);
