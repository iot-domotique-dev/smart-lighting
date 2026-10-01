#pragma once

#include "communication.h"
#include "device_registry.h"
#include "provisioning_store.h"

enum class ProvisioningPacketType : uint8_t {
    DEVICE_ANNOUNCE,
    PAIR_REQUEST,
    PAIR_ACCEPT,
    PAIR_CONFIRM,
    PAIR_REJECT
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
};

enum class DiscoveryResult : uint8_t {
    INVALID_ANNOUNCEMENT,
    DISCOVERED_UNPAIRED,
    DISCOVERED_OTHER_MAIN,
    DUPLICATE_ANNOUNCEMENT,
    RECOGNIZED_PAIRED,
    RESTORED_PAIRED,
    IDENTITY_CONFLICT
};

class ProvisioningLamp {
private:
    char hardwareId[LAMP_HARDWARE_ID_LENGTH];
    char firmwareVersion[LAMP_FIRMWARE_VERSION_LENGTH];
    uint32_t capabilities;
    ProvisioningStore& store;
    ProvisioningRecord configuration;
    PairingState pairingState;
    uint32_t pendingMainId;
    bool hasPendingRequest;

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
    PairingState state() const;
};

class MainProvisioning {
private:
    static const uint8_t MAX_DISCOVERED_LAMPS = 10;
    uint32_t mainId;
    LampRegistry& registry;
    ProvisioningPacket discovered[MAX_DISCOVERED_LAMPS];
    uint8_t discoveredCount;
    bool commissioningEnabled;
    bool hasPendingPairing;
    char pendingHardwareId[LAMP_HARDWARE_ID_LENGTH];
    uint32_t pendingDeviceId;

    int8_t findDiscovered(const char* hardwareId) const;
    uint32_t findAvailableDeviceId() const;
    void clearPendingPairing();

public:
    MainProvisioning(uint32_t mainId, LampRegistry& registry);

    void startCommissioning();
    void stopCommissioning();
    bool commissioningIsEnabled() const;
    uint8_t detectedCount() const;
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
