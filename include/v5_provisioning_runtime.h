#pragma once

#include "communication.h"
#include "device_registry.h"
#include "message_tracker.h"
#include "message_deduplicator.h"
#include "provisioning.h"
#include "core_pairing_protocol.h"
#include "group_manager.h"

constexpr uint8_t MAIN_GROUP_POWER_OPERATION_SLOTS = 32;

struct MainGroupPowerMember {
    uint32_t localId;
    uint32_t destinationId;
    uint32_t childCommandId;
    uint32_t sentAt;
    uint32_t firstSentAt;
    GroupProtocolResult result;
    uint8_t retries;
    bool waiting;
    bool supersededByPriorityOff;
};

struct MainGroupPowerOperation {
    uint32_t id;
    uint32_t startedAt;
    uint32_t completedAt;
    GroupProtocolPayload request;
    MainGroupPowerMember members[MAX_GROUP_LAMPS];
    uint8_t memberCount;
    GroupProtocolResult result;
    bool valid;
    bool active;
};

struct MainIndividualOutputGuard {
    uint32_t messageId;
    uint32_t localId;
    uint32_t destinationId;
    uint32_t startedAt;
    uint32_t fencesMessageId;
    int32_t commandType;
    int32_t value;
    bool active;
    bool uncertain;
    bool supersededByPriorityOff;
};

class V5ProvisioningRuntime {
private:
    Communication& communication;
    LampRegistry& lamps;
    MessageTracker& tracker;
    MessageDeduplicator& deduplicator;
    DeviceRole role;
    ProvisioningStore* lampStore;
    ProvisioningReservationStore* reservationStore;
    GroupStore* groupStore;
    GroupRegistry* groupRegistry;
    ProvisioningLamp* lampPairing;
    MainProvisioning* mainPairing;
    char hardwareId[LAMP_HARDWARE_ID_LENGTH];
    uint32_t mainLogicalId;
    uint32_t nextAnnounceAt;
    uint32_t activePairingSessionId;
    uint32_t lastClosedPairingSessionId;
    uint32_t pairingSessionStartedAt;
    uint16_t pairingSessionDurationSeconds;
    bool pairingSessionExpired;
    uint32_t nextPairingCloseAttemptAt;
    uint32_t activePairingOperationId;
    uint32_t activePairingOperationSessionId;
    uint32_t pairingOperationStartedAt;
    uint32_t nextRemovalRetryAt;
    uint32_t lastRemovalContactAt;
    char activePairingHardwareId[LAMP_HARDWARE_ID_LENGTH];
    GroupRegistry groupReadSnapshot;
    GroupProtocolPayload activeGroupRequest;
    uint8_t groupReadIndex;
    uint8_t groupReadMemberOffset;
    uint8_t groupReadPhase;
    bool groupReadActive;
    bool groupReadIsList;
    MainGroupPowerOperation groupPowerOperations[
        MAIN_GROUP_POWER_OPERATION_SLOTS];
    MainIndividualOutputGuard individualOutputGuards[MAX_PENDING_MESSAGES];
    uint8_t nextGroupPowerOperationSlot;

    static bool handleMessage(
        Communication& communication,
        const Message& message,
        void* context
    );
    bool dispatch(const Message& message);
    bool handleCorePairingControl(const Message& message);
    void sendPairingSessionResult(uint32_t sessionId,
                                  CorePairingResultCode result);
    void sendPairingOperationResult(uint32_t sessionId, uint32_t operationId,
                                   CorePairingResultCode result,
                                   uint32_t deviceId,
                                   const char* hardwareId);
    void clearPairingOperation();
    bool handleCoreRemovalControl(const Message& message);
    bool handleCoreGroupControl(const Message& message);
    bool handleGroupPowerRequest(const Message& message);
    bool handleGroupPowerAck(const Message& message);
    bool handleGroupOutputCommand(const Message& message);
    void pollGroupPowerOperations(uint32_t now);
    void sendGroupPowerMemberResult(MainGroupPowerOperation& operation,
                                    uint8_t memberIndex);
    void finishGroupPowerIfReady(MainGroupPowerOperation& operation,
                                 uint32_t now);
    bool hasActiveGroupPowerOperation() const;
    void pollGroupRead();
    bool sendGroupResponse(const GroupProtocolPayload& group);
    void sendRemovalResult(uint32_t operationId,
                           CorePairingResultCode result,
                           const MainProvisioningReservationState& state);
    void announceAcceptedLamp(const ProvisioningPacket& lamp);
    void processRemovalEvidence(const ProvisioningPacket& packet);
    bool sendPacket(const ProvisioningPacket& packet, uint32_t sourceId,
                    uint32_t destinationId);
    void restoreLocalLamp();

public:
    V5ProvisioningRuntime(
        Communication& communication,
        LampRegistry& lamps,
        MessageTracker& tracker,
        MessageDeduplicator& deduplicator
    );

    bool begin(
        DeviceRole role,
        ProvisioningStore* lampStore = nullptr,
        ProvisioningReservationStore* reservationStore = nullptr,
        GroupStore* groupStore = nullptr,
        GroupRegistry* groupRegistry = nullptr
    );
    void poll();

    /* Explicit commissioning entry point. Discovery never calls this itself. */
    bool commissionLamp(const char* hardwareId, const char* requestedName);
    bool setPower(uint32_t deviceId, bool enabled);

    uint32_t mainId() const;
    const char* localHardwareId() const;
};

extern V5ProvisioningRuntime v5ProvisioningRuntime;

/* Callable by a local commissioning controller after device discovery. */
bool commissionLampV5(const char* hardwareId, const char* requestedName);
bool setLampPowerV5(uint32_t deviceId, bool enabled);
