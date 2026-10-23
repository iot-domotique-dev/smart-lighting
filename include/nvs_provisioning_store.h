#pragma once

#include "provisioning_store.h"
#include "group_manager.h"

/* ESP-IDF NVS implementation of the V4 lamp pairing store. */
class NvsProvisioningStore : public ProvisioningStore {
public:
    bool load(ProvisioningRecord& record) const override;
    bool save(const ProvisioningRecord& record) override;
    bool clear() override;
    bool loadRemovalReceipt(LampRemovalReceipt& receipt) const override;
    bool saveRemovalReceipt(const LampRemovalReceipt& receipt) override;
    bool clearRemovalReceipt() override;
    LampBrightnessLoadResult loadLastNonzeroBrightness(
        uint8_t& brightness
    ) const override;
    bool saveLastNonzeroBrightness(uint8_t brightness) override;
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

/* Additive MAIN-only store for compact, versioned group snapshots. */
class NvsMainGroupStore : public GroupStore {
public:
    GroupStoreLoadResult load(
        uint32_t expectedMainId,
        GroupRegistry& registry
    ) const override;
    bool save(const GroupRegistry& registry) override;
};
