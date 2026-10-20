#pragma once

#include "device_registry.h"

struct ProvisioningRecord {
    uint32_t deviceId;
    char hardwareId[LAMP_HARDWARE_ID_LENGTH];
    char name[LAMP_NAME_LENGTH];
    uint32_t parentMainId;
    PairingState pairingState;
};

enum class LampRemovalReceiptStage : uint8_t { NONE, PREPARED, CLEARED };

struct LampRemovalReceipt {
    uint8_t version;
    LampRemovalReceiptStage stage;
    uint16_t reserved;
    uint32_t operationId;
    uint32_t deviceId;
    uint32_t parentMainId;
    char hardwareId[LAMP_HARDWARE_ID_LENGTH];
};

constexpr uint8_t LAMP_REMOVAL_RECEIPT_VERSION = 1;

enum class LampBrightnessLoadResult : uint8_t {
    FOUND,
    NOT_FOUND,
    INVALID,
    ERROR
};

class ProvisioningStore {
public:
    virtual ~ProvisioningStore() = default;
    virtual bool load(ProvisioningRecord& record) const = 0;
    virtual bool save(const ProvisioningRecord& record) = 0;
    virtual bool clear() = 0;
    virtual bool loadRemovalReceipt(LampRemovalReceipt& receipt) const = 0;
    virtual bool saveRemovalReceipt(const LampRemovalReceipt& receipt) = 0;
    virtual bool clearRemovalReceipt() = 0;
    virtual LampBrightnessLoadResult loadLastNonzeroBrightness(
        uint8_t& brightness
    ) const = 0;
    virtual bool saveLastNonzeroBrightness(uint8_t brightness) = 0;
};

/*
 * MAIN keeps its own append-only allocation watermark. IDs 1..MAX_LAMPS are
 * reserved for pairings created by firmware versions that only kept the
 * registry in RAM. The small identity table records the latest ID assigned to
 * each known hardware identity; IDs below the watermark are never reused.
 */
constexpr uint8_t MAIN_PROVISIONING_STATE_VERSION = 2;
constexpr uint16_t MAIN_LEGACY_ID_TOMBSTONE_MASK = (1U << MAX_LAMPS) - 1U;
constexpr uint8_t MAIN_REMOVAL_NONE = 0;
constexpr uint8_t MAIN_REMOVAL_REQUESTED = 1;
constexpr uint8_t MAIN_REMOVAL_SUCCEEDED = 2;
constexpr uint8_t MAIN_REMOVAL_FAILED = 3;

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
    uint16_t retiredLegacyIds;
    uint8_t removalState;
    uint8_t removalReserved;
    uint32_t removalOperationId;
    uint32_t removalDeviceId;
    uint32_t removalParentMainId;
    uint32_t removalCoreDeviceId;
    char removalHardwareId[LAMP_HARDWARE_ID_LENGTH];
};

/* Exact V7.6 raw-blob layout. NVS migration validates this before conversion. */
struct MainProvisioningReservationStateV1 {
    uint8_t version;
    uint8_t count;
    uint16_t reserved;
    uint32_t mainId;
    uint32_t highWaterDeviceId;
    ProvisioningIdentityReservation identities[MAX_LAMPS];
};

static_assert(sizeof(MainProvisioningReservationStateV1) ==
                  sizeof(uint8_t) * 2 + sizeof(uint16_t) +
                  sizeof(uint32_t) * 2 +
                  sizeof(ProvisioningIdentityReservation) * MAX_LAMPS,
              "V7.6 NVS reservation layout changed");

bool migrateMainProvisioningReservationStateV1(
    const MainProvisioningReservationStateV1& legacy,
    uint32_t expectedMainId,
    MainProvisioningReservationState& migrated
);

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
    LampRemovalReceipt removalReceipt;
    uint8_t lastNonzeroBrightness;
    bool hasRecord;
    bool hasRemovalReceipt;
    bool hasLastNonzeroBrightness;
    bool failWrites;
    bool failBrightnessReads;
    uint32_t brightnessWriteAttemptCount;

public:
    MemoryProvisioningStore();
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
    void setFailWrites(bool fail);
    void setFailBrightnessReads(bool fail);
    void setStoredBrightnessForTest(uint8_t brightness);
    uint32_t getBrightnessWriteAttemptCount() const;
};
