#include <Arduino.h>
#include <string.h>

#include "device_registry.h"

namespace {
void copyDeviceName(
    char* destination,
    const char* source
) {
    uint8_t index = 0;
    if (source != nullptr) {
        while (index + 1 < MAX_DEVICE_NAME_LENGTH && source[index] != '\0') {
            destination[index] = source[index];
            ++index;
        }
    }
    destination[index] = '\0';
}
}  // namespace


void initDeviceRegistry(DeviceRegistry& registry) {
    registry.count = 0;
    for (uint8_t i = 0; i < MAX_DEVICES; ++i) {
        registry.devices[i] = {};
        registry.names[i][0] = '\0';
    }
}


bool registerDevice(
    DeviceRegistry& registry,
    const Device& device
) {
    if (registry.count >= MAX_DEVICES ||
        findDeviceById(registry, device.id) != nullptr) {
        return false;
    }

    char copiedName[MAX_DEVICE_NAME_LENGTH] = {};
    copyDeviceName(copiedName, device.name);

    const uint8_t index = registry.count;
    registry.devices[index] = device;
    copyDeviceName(registry.names[index], copiedName);
    registry.devices[index].name = registry.names[index];
    ++registry.count;
    return true;
}


bool unregisterDevice(
    DeviceRegistry& registry,
    uint32_t deviceId
) {
    uint8_t index = 0;
    while (index < registry.count && registry.devices[index].id != deviceId) {
        ++index;
    }
    if (index == registry.count) {
        return false;
    }

    for (uint8_t i = index; i + 1 < registry.count; ++i) {
        registry.devices[i] = registry.devices[i + 1];
        copyDeviceName(registry.names[i], registry.names[i + 1]);
        registry.devices[i].name = registry.names[i];
    }

    --registry.count;
    registry.devices[registry.count] = {};
    registry.names[registry.count][0] = '\0';
    return true;
}


Device* findDeviceById(
    DeviceRegistry& registry,
    uint32_t deviceId
) {
    for (uint8_t i = 0; i < registry.count; ++i) {
        if (registry.devices[i].id == deviceId) {
            return &registry.devices[i];
        }
    }
    return nullptr;
}


const Device* findDeviceById(
    const DeviceRegistry& registry,
    uint32_t deviceId
) {
    for (uint8_t i = 0; i < registry.count; ++i) {
        if (registry.devices[i].id == deviceId) {
            return &registry.devices[i];
        }
    }
    return nullptr;
}


bool deviceExists(
    const DeviceRegistry& registry,
    uint32_t deviceId
) {
    return findDeviceById(registry, deviceId) != nullptr;
}


const Device* getAllDevices(
    const DeviceRegistry& registry,
    uint8_t& count
) {
    count = registry.count;
    return registry.devices;
}


uint8_t findDevicesByRole(
    DeviceRegistry& registry,
    DeviceRole role,
    Device** matches,
    uint8_t capacity
) {
    uint8_t total = 0;
    for (uint8_t i = 0; i < registry.count; ++i) {
        if (registry.devices[i].role != role) {
            continue;
        }
        if (matches != nullptr && total < capacity) {
            matches[total] = &registry.devices[i];
        }
        ++total;
    }
    return total;
}


bool updateDevice(
    DeviceRegistry& registry,
    const Device& device
) {
    Device* existing = findDeviceById(registry, device.id);
    if (existing == nullptr) {
        return false;
    }

    char copiedName[MAX_DEVICE_NAME_LENGTH] = {};
    copyDeviceName(copiedName, device.name);
    const uint8_t index = static_cast<uint8_t>(existing - registry.devices);
    registry.devices[index] = device;
    copyDeviceName(registry.names[index], copiedName);
    registry.devices[index].name = registry.names[index];
    return true;
}


bool setDeviceStatus(
    DeviceRegistry& registry,
    uint32_t deviceId,
    DeviceStatus status,
    uint32_t lastSeen
) {
    Device* device = findDeviceById(registry, deviceId);
    if (device == nullptr) {
        return false;
    }
    device->status = status;
    device->lastSeen = lastSeen;
    if (status == DeviceStatus::OFFLINE) {
        markLastConfirmedPowerStale(*device);
    }
    return true;
}


void initLampRegistry(
    LampRegistry& registry
) {

    registry.count = 0;
}


Lamp* findLamp(
    LampRegistry& registry,
    uint32_t id
) {

    for (
        uint8_t i = 0;
        i < registry.count;
        i++
    ) {

        if (
            registry.lamps[i].device.id == id
        ) {

            return &registry.lamps[i];
        }
    }

    return nullptr;
}


Lamp* findLampByDeviceId(
    LampRegistry& registry,
    uint32_t deviceId
) {
    return findLamp(registry, deviceId);
}


bool addLamp(
    LampRegistry& registry,
    const Lamp& lamp
) {

    if (
        registry.count >= MAX_LAMPS
    ) {

        return false;
    }


    if (
        findLamp(
            registry,
            lamp.device.id
        ) != nullptr
    ) {

        return false;
    }

    if (lamp.identity.hardwareId[0] != '\0' &&
        findLampByHardwareId(registry, lamp.identity.hardwareId) != nullptr) {
        return false;
    }


    registry.lamps[
        registry.count
    ] = lamp;

    if (registry.lamps[registry.count].identity.hardwareId[0] != '\0') {
        registry.lamps[registry.count].device.name =
            registry.lamps[registry.count].identity.name;
    }

    registry.count++;

    return true;
}


Lamp* findLampByHardwareId(
    LampRegistry& registry,
    const char* hardwareId
) {
    if (hardwareId == nullptr || hardwareId[0] == '\0') {
        return nullptr;
    }

    for (uint8_t i = 0; i < registry.count; ++i) {
        if (strcmp(registry.lamps[i].identity.hardwareId, hardwareId) == 0) {
            return &registry.lamps[i];
        }
    }

    return nullptr;
}


uint8_t findLampsByParentMainId(
    LampRegistry& registry,
    uint32_t parentMainId,
    Lamp** matches,
    uint8_t capacity
) {
    uint8_t total = 0;
    for (uint8_t i = 0; i < registry.count; ++i) {
        const Lamp& lamp = registry.lamps[i];
        if (lamp.identity.pairingState != PairingState::PAIRED ||
            lamp.identity.parentMainId != parentMainId) {
            continue;
        }

        if (matches != nullptr && total < capacity) {
            matches[total] = &registry.lamps[i];
        }
        ++total;
    }
    return total;
}


bool removeLamp(
    LampRegistry& registry,
    uint32_t deviceId
) {
    uint8_t index = 0;
    while (index < registry.count && registry.lamps[index].device.id != deviceId) {
        ++index;
    }
    if (index == registry.count) {
        return false;
    }

    for (uint8_t i = index; i + 1 < registry.count; ++i) {
        registry.lamps[i] = registry.lamps[i + 1];
    }
    --registry.count;

    for (uint8_t i = 0; i < registry.count; ++i) {
        if (registry.lamps[i].identity.hardwareId[0] != '\0') {
            registry.lamps[i].device.name = registry.lamps[i].identity.name;
        }
    }
    return true;
}


void printLampRegistry(
    const LampRegistry& registry
) {

    Serial.println();
    Serial.println(
        "===== REGISTRE LAMPES ====="
    );

    Serial.print(
        "Nombre de lampes : "
    );

    Serial.println(
        registry.count
    );


    for (
        uint8_t i = 0;
        i < registry.count;
        i++
    ) {

        const Lamp& lamp =
            registry.lamps[i];


        Serial.print("#");
        Serial.print(i);

        Serial.print(" | ID=");
        Serial.print(
            lamp.device.id
        );

        Serial.print(" | ");
        Serial.print(
            lamp.device.name
        );

        Serial.print(" | ");

        Serial.println(
            lamp.device.status ==
            DeviceStatus::ONLINE
                ? "ONLINE"
                : "OFFLINE"
        );
    }

    Serial.println(
        "==========================="
    );
}
