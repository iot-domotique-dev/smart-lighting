#pragma once

#include "provisioning_store.h"

/* ESP-IDF NVS implementation of the V4 lamp pairing store. */
class NvsProvisioningStore : public ProvisioningStore {
public:
    bool load(ProvisioningRecord& record) const override;
    bool save(const ProvisioningRecord& record) override;
    bool clear() override;
};
