#pragma once

#include "communication.h"
#include "device_registry.h"
#include "message_tracker.h"
#include "provisioning.h"
#include "core_pairing_protocol.h"

class V5ProvisioningRuntime {
private:
    Communication& communication;
    LampRegistry& lamps;
    MessageTracker& tracker;
    DeviceRole role;
    ProvisioningStore* lampStore;
    ProvisioningReservationStore* reservationStore;
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
    char activePairingHardwareId[LAMP_HARDWARE_ID_LENGTH];

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
    bool sendPacket(const ProvisioningPacket& packet, uint32_t sourceId,
                    uint32_t destinationId);
    void restoreLocalLamp();

public:
    V5ProvisioningRuntime(
        Communication& communication,
        LampRegistry& lamps,
        MessageTracker& tracker
    );

    bool begin(
        DeviceRole role,
        ProvisioningStore* lampStore = nullptr,
        ProvisioningReservationStore* reservationStore = nullptr
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
