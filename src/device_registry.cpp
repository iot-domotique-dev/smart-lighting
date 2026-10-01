#include <Arduino.h>
#include <string.h>

#include "device_registry.h"


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
