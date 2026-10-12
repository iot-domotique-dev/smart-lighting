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

bool isValidMainProvisioningReservationState(
    const MainProvisioningReservationState& state,
    uint32_t expectedMainId
) {
    if (state.version != MAIN_PROVISIONING_STATE_VERSION ||
        state.mainId == 0 || state.mainId != expectedMainId ||
        state.highWaterDeviceId < MAX_LAMPS || state.count > MAX_LAMPS) {
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
    : hasRecord(false) {
    memset(&record, 0, sizeof(record));
}

bool MemoryProvisioningStore::load(ProvisioningRecord& output) const {
    if (!hasRecord) {
        return false;
    }
    output = record;
    return true;
}

bool MemoryProvisioningStore::save(const ProvisioningRecord& input) {
    if (input.hardwareId[0] == '\0' || input.deviceId == 0 ||
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
    memset(&record, 0, sizeof(record));
    hasRecord = false;
    return true;
}
