#include <string.h>

#include "provisioning_store.h"

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
