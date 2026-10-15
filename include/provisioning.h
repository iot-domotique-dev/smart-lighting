#pragma once

#include "communication.h"
#include "device_registry.h"
#include "provisioning_store.h"

enum class ProvisioningPacketType : uint8_t {
    DEVICE_ANNOUNCE,
    PAIR_REQUEST,
    PAIR_ACCEPT,
    PAIR_CONFIRM,
    PAIR_REJECT,
    UNPAIR_REQUEST,
    UNPAIR_CONFIRM,
    UNPAIR_COMMIT,
    UNPAIR_REJECT
};

struct ProvisioningPacket {
    ProvisioningPacketType type;
    char hardwareId[LAMP_HARDWARE_ID_LENGTH];
    DeviceRole role;
    char firmwareVersion[LAMP_FIRMWARE_VERSION_LENGTH];
    uint32_t capabilities;
    uint32_t deviceId;
    uint32_t parentMainId;
    char name[LAMP_NAME_LENGTH];
    PairingState pairingState;
    uint32_t transactionId;
    /* Transport envelope copied from Message; not serialized a second time. */
    uint32_t sourceId;
    uint32_t destinationId;
};

enum class DiscoveryResult : uint8_t {
    INVALID_ANNOUNCEMENT,
    DISCOVERED_UNPAIRED,
    DISCOVERED_OTHER_MAIN,
    DUPLICATE_ANNOUNCEMENT,
    RECOGNIZED_PAIRED,
    RESTORED_PAIRED,
    IDENTITY_CONFLICT,
    CANDIDATE_CAPACITY,
    REMOVAL_PENDING,
    REMOVAL_RECONCILED
};

constexpr uint32_t PROVISIONING_TRANSACTION_TIMEOUT_MS = 30000;

class ProvisioningLamp {
private:
    char hardwareId[LAMP_HARDWARE_ID_LENGTH];
    char firmwareVersion[LAMP_FIRMWARE_VERSION_LENGTH];
    uint32_t capabilities;
    ProvisioningStore& store;
    ProvisioningRecord configuration;
    LampRemovalReceipt removalReceipt;
    PairingState pairingState;
    uint32_t pendingMainId;
    uint32_t pendingStartedAt;
    bool hasPendingRequest;
    bool hasRemovalReceipt;

    bool finishPreparedRemoval();

public:
    ProvisioningLamp(
        const char* hardwareId,
        const char* firmwareVersion,
        uint32_t capabilities,
        ProvisioningStore& store
    );

    ProvisioningPacket announce() const;
    bool handlePairRequest(
        const ProvisioningPacket& request,
        ProvisioningPacket& rejection
    );
    bool handlePairAccept(
        const ProvisioningPacket& acceptance,
        ProvisioningPacket& confirmation
    );
    bool handleUnpairRequest(const ProvisioningPacket& request,
                             ProvisioningPacket& confirmation);
    bool handleUnpairCommit(const ProvisioningPacket& commit);
    bool retryPreparedRemoval(ProvisioningPacket& confirmation);
    void expirePendingRequest(uint32_t now);
    PairingState state() const;
};

class MainProvisioning {
private:
    static const uint8_t MAX_DISCOVERED_LAMPS = 10;
    uint32_t mainId;
    LampRegistry& registry;
    ProvisioningReservationStore* reservationStore;
    MainProvisioningReservationState reservationState;
    bool reservationStoreReady;
    ProvisioningPacket discovered[MAX_DISCOVERED_LAMPS];
    uint32_t discoveredAt[MAX_DISCOVERED_LAMPS];
    uint8_t discoveredCount;
    bool commissioningEnabled;
    bool hasPendingPairing;
    char pendingHardwareId[LAMP_HARDWARE_ID_LENGTH];
    char pendingName[LAMP_NAME_LENGTH];
    uint32_t pendingDeviceId;
    uint32_t pendingStartedAt;

    int8_t findDiscovered(const char* hardwareId) const;
    int8_t findReservationByHardwareId(const char* hardwareId) const;
    int8_t findReservationByDeviceId(uint32_t deviceId) const;
    uint32_t findAvailableDeviceId() const;
    bool saveReservationState(const MainProvisioningReservationState& state);
    bool recordPairedIdentity(const ProvisioningPacket& announcement);
    void clearPendingPairing(bool requireFreshAnnouncement);

public:
    MainProvisioning(
        uint32_t mainId,
        LampRegistry& registry,
        ProvisioningReservationStore* reservationStore = nullptr
    );

    void startCommissioning();
    void stopCommissioning();
    void expirePendingPairing(uint32_t now);
    void abortPendingPairing();
    bool commissioningIsEnabled() const;
    uint8_t detectedCount() const;
    bool hasUnpairedCandidate(const char* hardwareId) const;
    bool canPairCandidate(const char* hardwareId) const;
    bool pairingCapacityReached(const char* hardwareId) const;
    void expireUnpairedCandidates(uint32_t now);
    void clearUnpairedCandidates();
    DiscoveryResult discover(const ProvisioningPacket& announcement);
    bool createPairRequest(
        const char* hardwareId,
        ProvisioningPacket& request
    );
    bool createPairAcceptance(
        const char* hardwareId,
        const char* requestedName,
        ProvisioningPacket& acceptance
    );
    bool confirmPairing(const ProvisioningPacket& confirmation);

    bool beginRemoval(uint32_t operationId, uint32_t deviceId,
                     uint32_t coreDeviceId);
    bool buildRemovalPacket(ProvisioningPacket& packet) const;
    bool reconcileRemovalPacket(const ProvisioningPacket& packet,
                                ProvisioningPacket& response,
                                bool& removalCompleted);
    bool getRemovalStatus(uint32_t operationId,
                          MainProvisioningReservationState& state) const;
    bool isRemovalTarget(uint32_t deviceId) const;
    bool isRemovedIdentity(uint32_t deviceId, const char* hardwareId) const;
    bool hasActiveRemoval() const;
    uint32_t removalOperationId() const;
    uint32_t removalDeviceId() const;
    uint8_t removalState() const;
};

/* Codec keeps provisioning packets on the existing Communication transport. */
bool encodeProvisioningPacket(
    const ProvisioningPacket& packet,
    uint32_t messageId,
    uint32_t sourceId,
    uint32_t destinationId,
    Message& message
);

bool decodeProvisioningPacket(
    const Message& message,
    ProvisioningPacket& packet
);
