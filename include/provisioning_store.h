#pragma once

#include "device_registry.h"

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

/*
 * MAIN keeps its own append-only allocation watermark. IDs 1..MAX_LAMPS are
 * reserved for pairings created by firmware versions that only kept the
 * registry in RAM. The small identity table records the latest ID assigned to
 * each known hardware identity; IDs below the watermark are never reused.
 */
constexpr uint8_t MAIN_PROVISIONING_STATE_VERSION = 1;

struct ProvisioningIdentityReservation {
    uint32_t deviceId;
    char hardwareId[LAMP_HARDWARE_ID_LENGTH];
};

struct MainProvisioningReservationState {
    uint8_t version;
    uint8_t count;
    uint16_t reserved;
    uint32_t mainId;
    uint32_t highWaterDeviceId;
    ProvisioningIdentityReservation identities[MAX_LAMPS];
};

void initializeMainProvisioningReservationState(
    uint32_t mainId,
    MainProvisioningReservationState& state
);
bool isValidMainProvisioningReservationState(
    const MainProvisioningReservationState& state,
    uint32_t expectedMainId
);

class ProvisioningReservationStore {
public:
    virtual ~ProvisioningReservationStore() = default;
    /* A missing record is a valid first boot and returns the legacy watermark. */
    virtual bool load(
        uint32_t mainId,
        MainProvisioningReservationState& state
    ) const = 0;
    virtual bool save(const MainProvisioningReservationState& state) = 0;
};

/* Test double for the MAIN's separate persistent reservation store. */
class MemoryProvisioningReservationStore : public ProvisioningReservationStore {
private:
    MainProvisioningReservationState state;
    bool hasState;
    bool failWrites;

public:
    MemoryProvisioningReservationStore();
    bool load(
        uint32_t mainId,
        MainProvisioningReservationState& state
    ) const override;
    bool save(const MainProvisioningReservationState& state) override;
    void setFailWrites(bool fail);
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
