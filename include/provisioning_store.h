#pragma once

#include "lamp.h"

struct ProvisioningRecord {
    uint32_t deviceId;
    char hardwareId[LAMP_HARDWARE_ID_LENGTH];
    char name[LAMP_NAME_LENGTH];
    uint32_t parentMainId;
    PairingState pairingState;
};

class ProvisioningStore {
public:
    virtual ~ProvisioningStore() = default;
    virtual bool load(ProvisioningRecord& record) const = 0;
    virtual bool save(const ProvisioningRecord& record) = 0;
    virtual bool clear() = 0;
};

/* Test double for native builds; one instance represents one LAMP's NVS. */
class MemoryProvisioningStore : public ProvisioningStore {
private:
    ProvisioningRecord record;
    bool hasRecord;

public:
    MemoryProvisioningStore();
    bool load(ProvisioningRecord& record) const override;
    bool save(const ProvisioningRecord& record) override;
    bool clear() override;
};
