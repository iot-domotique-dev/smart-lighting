#include <string.h>

#include "provisioning_store.h"

void initializeMainProvisioningReservationState(
    uint32_t mainId,
    MainProvisioningReservationState& state
) {
    memset(&state, 0, sizeof(state));
    state.version = MAIN_PROVISIONING_STATE_VERSION;
    state.mainId = mainId;
    state.highWaterDeviceId = MAX_LAMPS;
}

bool migrateMainProvisioningReservationStateV1(
    const MainProvisioningReservationStateV1& legacy,
    uint32_t expectedMainId,
    MainProvisioningReservationState& migrated
) {
    if (legacy.version != 1 || legacy.reserved != 0 ||
        legacy.mainId == 0 || legacy.mainId != expectedMainId ||
        legacy.highWaterDeviceId < MAX_LAMPS || legacy.count > MAX_LAMPS) {
        return false;
    }
    initializeMainProvisioningReservationState(expectedMainId, migrated);
    migrated.count = legacy.count;
    migrated.highWaterDeviceId = legacy.highWaterDeviceId;
    for (uint8_t index = 0; index < legacy.count; ++index) {
        migrated.identities[index] = legacy.identities[index];
    }
    return isValidMainProvisioningReservationState(migrated, expectedMainId);
}

bool isValidMainProvisioningReservationState(
    const MainProvisioningReservationState& state,
    uint32_t expectedMainId
) {
    if (state.version != MAIN_PROVISIONING_STATE_VERSION ||
        state.mainId == 0 || state.mainId != expectedMainId ||
        state.highWaterDeviceId < MAX_LAMPS || state.count > MAX_LAMPS ||
        (state.retiredLegacyIds & ~MAIN_LEGACY_ID_TOMBSTONE_MASK) != 0 ||
        state.removalReserved != 0 ||
        state.removalState > MAIN_REMOVAL_FAILED) {
        return false;
    }

    if (state.removalState == MAIN_REMOVAL_NONE) {
        if (state.removalOperationId != 0 || state.removalDeviceId != 0 ||
            state.removalParentMainId != 0 || state.removalCoreDeviceId != 0 ||
            state.removalHardwareId[0] != '\0') return false;
    } else if (state.removalOperationId == 0 || state.removalDeviceId == 0 ||
               state.removalParentMainId != expectedMainId ||
               state.removalCoreDeviceId == 0 ||
               state.removalHardwareId[0] == '\0' ||
               memchr(state.removalHardwareId, '\0',
                      sizeof(state.removalHardwareId)) == nullptr) {
        return false;
    }

    for (uint8_t i = 0; i < state.count; ++i) {
        const ProvisioningIdentityReservation& current = state.identities[i];
        if (current.deviceId == 0 ||
            current.deviceId > state.highWaterDeviceId ||
            current.hardwareId[0] == '\0' ||
            memchr(current.hardwareId, '\0', sizeof(current.hardwareId)) == nullptr) {
            return false;
        }
        for (uint8_t j = 0; j < i; ++j) {
            if (state.identities[j].deviceId == current.deviceId ||
                strcmp(state.identities[j].hardwareId, current.hardwareId) == 0) {
                return false;
            }
        }
    }
    return true;
}

MemoryProvisioningReservationStore::MemoryProvisioningReservationStore()
    : hasState(false), failWrites(false) {
    memset(&state, 0, sizeof(state));
}

bool MemoryProvisioningReservationStore::load(
    uint32_t mainId,
    MainProvisioningReservationState& output
) const {
    if (mainId == 0) {
        return false;
    }
    if (!hasState) {
        initializeMainProvisioningReservationState(mainId, output);
        return true;
    }
    output = state;
    return isValidMainProvisioningReservationState(output, mainId);
}

bool MemoryProvisioningReservationStore::save(
    const MainProvisioningReservationState& input
) {
    if (failWrites ||
        !isValidMainProvisioningReservationState(input, input.mainId)) {
        return false;
    }
    state = input;
    hasState = true;
    return true;
}

void MemoryProvisioningReservationStore::setFailWrites(bool fail) {
    failWrites = fail;
}

MemoryProvisioningStore::MemoryProvisioningStore()
    : lastNonzeroBrightness(0), hasRecord(false), hasRemovalReceipt(false),
      hasLastNonzeroBrightness(false), failWrites(false),
      failBrightnessReads(false), brightnessWriteAttemptCount(0) {
    memset(&record, 0, sizeof(record));
    memset(&removalReceipt, 0, sizeof(removalReceipt));
}

bool MemoryProvisioningStore::load(ProvisioningRecord& output) const {
    if (!hasRecord) {
        return false;
    }
    output = record;
    return true;
}

bool MemoryProvisioningStore::save(const ProvisioningRecord& input) {
    if (failWrites || input.hardwareId[0] == '\0' || input.deviceId == 0 ||
        input.parentMainId == 0 || input.pairingState != PairingState::PAIRED) {
        return false;
    }
    record = input;
    record.hardwareId[sizeof(record.hardwareId) - 1] = '\0';
    record.name[sizeof(record.name) - 1] = '\0';
    hasRecord = true;
    return true;
}

bool MemoryProvisioningStore::clear() {
    if (failWrites) return false;
    memset(&record, 0, sizeof(record));
    hasRecord = false;
    return true;
}

bool MemoryProvisioningStore::loadRemovalReceipt(
    LampRemovalReceipt& output
) const {
    output = {};
    if (!hasRemovalReceipt) return false;
    output = removalReceipt;
    return output.version == LAMP_REMOVAL_RECEIPT_VERSION &&
           output.operationId != 0 && output.deviceId != 0 &&
           output.parentMainId != 0 && output.hardwareId[0] != '\0' &&
           memchr(output.hardwareId, '\0', sizeof(output.hardwareId)) != nullptr &&
           (output.stage == LampRemovalReceiptStage::PREPARED ||
            output.stage == LampRemovalReceiptStage::CLEARED);
}

bool MemoryProvisioningStore::saveRemovalReceipt(
    const LampRemovalReceipt& input
) {
    if (failWrites || input.version != LAMP_REMOVAL_RECEIPT_VERSION ||
        input.operationId == 0 || input.deviceId == 0 ||
        input.parentMainId == 0 || input.hardwareId[0] == '\0' ||
        memchr(input.hardwareId, '\0', sizeof(input.hardwareId)) == nullptr ||
        (input.stage != LampRemovalReceiptStage::PREPARED &&
         input.stage != LampRemovalReceiptStage::CLEARED)) return false;
    removalReceipt = input;
    hasRemovalReceipt = true;
    return true;
}

bool MemoryProvisioningStore::clearRemovalReceipt() {
    if (failWrites) return false;
    removalReceipt = {};
    hasRemovalReceipt = false;
    return true;
}

LampBrightnessLoadResult MemoryProvisioningStore::loadLastNonzeroBrightness(
    uint8_t& brightness
) const {
    brightness = 0;
    if (failBrightnessReads) return LampBrightnessLoadResult::ERROR;
    if (!hasLastNonzeroBrightness) return LampBrightnessLoadResult::NOT_FOUND;
    brightness = lastNonzeroBrightness;
    return brightness >= 1 && brightness <= 100
        ? LampBrightnessLoadResult::FOUND
        : LampBrightnessLoadResult::INVALID;
}

bool MemoryProvisioningStore::saveLastNonzeroBrightness(uint8_t brightness) {
    ++brightnessWriteAttemptCount;
    if (failWrites || brightness < 1 || brightness > 100) return false;
    lastNonzeroBrightness = brightness;
    hasLastNonzeroBrightness = true;
    return true;
}

void MemoryProvisioningStore::setFailWrites(bool fail) {
    failWrites = fail;
}

void MemoryProvisioningStore::setFailBrightnessReads(bool fail) {
    failBrightnessReads = fail;
}

void MemoryProvisioningStore::setStoredBrightnessForTest(uint8_t brightness) {
    lastNonzeroBrightness = brightness;
    hasLastNonzeroBrightness = true;
}

uint32_t MemoryProvisioningStore::getBrightnessWriteAttemptCount() const {
    return brightnessWriteAttemptCount;
}
