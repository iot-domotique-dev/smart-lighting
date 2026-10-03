#include <Arduino.h>

#include "device_manager.h"
#include "event_bus.h"

static uint32_t nextDeviceEventId = 1;

static void publishDeviceStatusEvent(
    EventBus* eventBus,
    EventType type,
    const Device& device,
    uint32_t timestamp
) {
    if (eventBus == nullptr) {
        return;
    }

    Event event = {
        nextDeviceEventId++,
        type,
        device.id,
        device.status == DeviceStatus::ONLINE ? 1 : 0,
        0,
        timestamp
    };

    publishEvent(*eventBus, event);
}

static EventType statusEventType(
    const Device& device,
    bool online
) {
    if (device.role == DeviceRole::LAMP) {
        return online ? EventType::LAMP_ONLINE : EventType::LAMP_OFFLINE;
    }
    return online ? EventType::DEVICE_ONLINE : EventType::DEVICE_OFFLINE;
}

static void expireDeviceIfNeeded(
    Device& device,
    uint32_t now,
    EventBus* eventBus
) {
    if (device.status != DeviceStatus::ONLINE ||
        now - device.lastSeen <= DEVICE_TIMEOUT) {
        return;
    }

    device.status = DeviceStatus::OFFLINE;
    publishDeviceStatusEvent(
        eventBus,
        statusEventType(device, false),
        device,
        now
    );

    Serial.print("Device OFFLINE : ");
    Serial.println(device.name);
}

void updateDeviceSeen(
    Device& device,
    EventBus* eventBus
) {
    const bool wasOffline = device.status == DeviceStatus::OFFLINE;
    const uint32_t now = millis();

    device.lastSeen = now;

    device.status =
        DeviceStatus::ONLINE;

    if (wasOffline) {
        publishDeviceStatusEvent(
            eventBus,
            statusEventType(device, true),
            device,
            now
        );
    }
}


void updateDeviceStatus(
    LampRegistry& registry,
    EventBus* eventBus
) {

    const uint32_t now = millis();
    for (uint8_t i = 0; i < registry.count; ++i) {
        expireDeviceIfNeeded(registry.lamps[i].device, now, eventBus);
    }
}


void updateDeviceStatus(
    DeviceRegistry& registry,
    EventBus* eventBus
) {
    const uint32_t now = millis();
    for (uint8_t i = 0; i < registry.count; ++i) {
        expireDeviceIfNeeded(registry.devices[i], now, eventBus);
    }
}
