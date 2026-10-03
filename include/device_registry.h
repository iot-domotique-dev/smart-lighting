#pragma once

#include "lamp.h"


constexpr uint8_t MAX_DEVICES = 32;
constexpr uint8_t MAX_DEVICE_NAME_LENGTH = 32;


/* Generic CORE-side inventory. LampRegistry below remains the specialized
 * state store used by the existing lighting command and Zigbee paths. */
struct DeviceRegistry {
    Device devices[MAX_DEVICES];
    char names[MAX_DEVICES][MAX_DEVICE_NAME_LENGTH];
    uint8_t count;
};

void initDeviceRegistry(DeviceRegistry& registry);

bool registerDevice(DeviceRegistry& registry, const Device& device);
bool unregisterDevice(DeviceRegistry& registry, uint32_t deviceId);

Device* findDeviceById(DeviceRegistry& registry, uint32_t deviceId);
const Device* findDeviceById(
    const DeviceRegistry& registry,
    uint32_t deviceId
);
bool deviceExists(const DeviceRegistry& registry, uint32_t deviceId);

const Device* getAllDevices(
    const DeviceRegistry& registry,
    uint8_t& count
);

uint8_t findDevicesByRole(
    DeviceRegistry& registry,
    DeviceRole role,
    Device** matches,
    uint8_t capacity
);

bool updateDevice(DeviceRegistry& registry, const Device& device);
bool setDeviceStatus(
    DeviceRegistry& registry,
    uint32_t deviceId,
    DeviceStatus status,
    uint32_t lastSeen
);


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
