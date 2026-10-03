#pragma once

#include "device_registry.h"

struct EventBus;


constexpr uint32_t DEVICE_TIMEOUT = 10000;


void updateDeviceSeen(
    Device& device,
    EventBus* eventBus = nullptr
);


void updateDeviceStatus(
    LampRegistry& registry,
    EventBus* eventBus = nullptr
);

void updateDeviceStatus(
    DeviceRegistry& registry,
    EventBus* eventBus = nullptr
);
