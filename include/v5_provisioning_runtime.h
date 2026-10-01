#pragma once

#include "communication.h"
#include "device_registry.h"
#include "message_tracker.h"
#include "provisioning.h"

class V5ProvisioningRuntime {
private:
    Communication& communication;
    LampRegistry& lamps;
    MessageTracker& tracker;
    DeviceRole role;
    ProvisioningStore* lampStore;
    ProvisioningLamp* lampPairing;
    MainProvisioning* mainPairing;
    char hardwareId[LAMP_HARDWARE_ID_LENGTH];
    uint32_t mainLogicalId;
    uint32_t nextAnnounceAt;

    static bool handleMessage(
        Communication& communication,
        const Message& message,
        void* context
    );
    bool dispatch(const Message& message);
    bool sendPacket(const ProvisioningPacket& packet, uint32_t sourceId,
                    uint32_t destinationId);
    void restoreLocalLamp();

public:
    V5ProvisioningRuntime(
        Communication& communication,
        LampRegistry& lamps,
        MessageTracker& tracker
    );

    bool begin(DeviceRole role, ProvisioningStore* lampStore = nullptr);
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
