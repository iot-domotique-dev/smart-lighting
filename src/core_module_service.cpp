#include "core_module_service.h"

namespace {
void hashByte(uint32_t& hash, uint8_t value) {
    hash ^= value;
    hash *= 16777619UL;
}

void hashU32(uint32_t& hash, uint32_t value) {
    hashByte(hash, static_cast<uint8_t>(value));
    hashByte(hash, static_cast<uint8_t>(value >> 8));
    hashByte(hash, static_cast<uint8_t>(value >> 16));
    hashByte(hash, static_cast<uint8_t>(value >> 24));
}

bool isValidParentRole(DeviceRole deviceRole, DeviceRole parentRole) {
    if (deviceRole == DeviceRole::MAIN) {
        return parentRole == DeviceRole::CORE;
    }
    if (parentRole != DeviceRole::MAIN) {
        return false;
    }
    switch (deviceRole) {
        case DeviceRole::LAMP:
        case DeviceRole::RELAY:
        case DeviceRole::SENSOR:
        case DeviceRole::CAMERA:
            return true;
        case DeviceRole::CORE:
        case DeviceRole::MAIN:
            return false;
    }
    return false;
}
}  // namespace

uint32_t makeCoreModuleId(uint32_t parentId, uint32_t localId) {
    if (parentId == 0 || localId == 0) {
        return 0;
    }

    uint32_t hash = 2166136261UL;
    hashByte(hash, 1);  // ID scheme version.
    hashU32(hash, parentId);
    hashU32(hash, localId);
    return 0x80000000UL | (hash & 0x7FFFFFFFUL);
}

CoreModuleUpdateResult ingestCoreModuleAnnouncement(
    DeviceRegistry& registry,
    const CoreModuleAnnouncement& announcement,
    uint32_t receivedAt
) {
    if (announcement.localId == 0 || announcement.name == nullptr ||
        announcement.name[0] == '\0' ||
        announcement.role == DeviceRole::CORE || announcement.parentId == 0) {
        return CoreModuleUpdateResult::INVALID_ANNOUNCEMENT;
    }

    const Device* parent = findDeviceById(registry, announcement.parentId);
    if (parent == nullptr) {
        return CoreModuleUpdateResult::PARENT_UNKNOWN;
    }

    const uint32_t globalId = makeCoreModuleId(announcement.parentId,
                                               announcement.localId);
    Device* existing = findDeviceById(registry, globalId);
    if (existing != nullptr) {
        if (existing->role != announcement.role ||
            existing->parentId != announcement.parentId ||
            existing->localId != announcement.localId) {
            return CoreModuleUpdateResult::IDENTITY_CONFLICT;
        }

        Device updated = {};
        updated.id = globalId;
        updated.name = announcement.name;
        updated.role = announcement.role;
        updated.status = DeviceStatus::ONLINE;
        updated.lastSeen = receivedAt;
        updated.capabilities = announcement.capabilities;
        updated.parentId = announcement.parentId;
        updated.localId = announcement.localId;
        updated.lastConfirmedPower = existing->lastConfirmedPower;
        updated.lastConfirmedPowerStatus = existing->lastConfirmedPowerStatus;
        updated.powerExecutionUnknown = existing->powerExecutionUnknown;
        if (!updateDevice(registry, updated)) {
            return CoreModuleUpdateResult::IDENTITY_CONFLICT;
        }
        return CoreModuleUpdateResult::UPDATED;
    }

    if (!isValidParentRole(announcement.role, parent->role)) {
        return CoreModuleUpdateResult::INVALID_ANNOUNCEMENT;
    }

    Device discovered = {};
    discovered.id = globalId;
    discovered.name = announcement.name;
    discovered.role = announcement.role;
    discovered.status = DeviceStatus::ONLINE;
    discovered.lastSeen = receivedAt;
    discovered.capabilities = announcement.capabilities;
    discovered.parentId = announcement.parentId;
    discovered.localId = announcement.localId;
    if (!registerDevice(registry, discovered)) {
        return deviceExists(registry, globalId)
            ? CoreModuleUpdateResult::IDENTITY_CONFLICT
            : CoreModuleUpdateResult::REGISTRY_FULL;
    }
    return CoreModuleUpdateResult::REGISTERED;
}
