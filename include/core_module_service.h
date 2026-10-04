#pragma once

#include "device_registry.h"

constexpr uint32_t CORE_LOGICAL_ID = 1;

enum class CoreModuleUpdateResult : uint8_t {
    INVALID_ANNOUNCEMENT,
    PARENT_UNKNOWN,
    REGISTERED,
    UPDATED,
    IDENTITY_CONFLICT,
    REGISTRY_FULL
};

struct CoreModuleAnnouncement {
    uint32_t localId;
    uint32_t parentId;
    const char* name;
    DeviceRole role;
    uint32_t capabilities;
};

/* Deterministic within one CORE inventory, not globally unique across homes. */
uint32_t makeCoreModuleId(uint32_t parentId, uint32_t localId);

/* Apply one decoded module announcement to the CORE inventory. */
CoreModuleUpdateResult ingestCoreModuleAnnouncement(
    DeviceRegistry& registry,
    const CoreModuleAnnouncement& announcement,
    uint32_t receivedAt
);

#if defined(SMART_LIGHTING_CORE_WIFI)
extern DeviceRegistry coreDeviceRegistry;
#endif
