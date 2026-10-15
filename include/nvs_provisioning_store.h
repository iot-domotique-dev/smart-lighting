#pragma once

#include "provisioning_store.h"

/* ESP-IDF NVS implementation of the V4 lamp pairing store. */
class NvsProvisioningStore : public ProvisioningStore {
public:
    bool load(ProvisioningRecord& record) const override;
    bool save(const ProvisioningRecord& record) override;
    bool clear() override;
    bool loadRemovalReceipt(LampRemovalReceipt& receipt) const override;
    bool saveRemovalReceipt(const LampRemovalReceipt& receipt) override;
    bool clearRemovalReceipt() override;
};

/* Separate additive NVS namespace used only by MAIN ID allocation. */
class NvsProvisioningReservationStore : public ProvisioningReservationStore {
public:
    bool load(
        uint32_t mainId,
        MainProvisioningReservationState& state
    ) const override;
    bool save(const MainProvisioningReservationState& state) override;
};
