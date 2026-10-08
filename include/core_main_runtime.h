#pragma once

#include "communication.h"
#include "core_command_service.h"

/* Announces the MAIN's local identity and accepts the CORE-assigned ID. */
class CoreMainRuntime {
private:
    Communication& zigbee;
    MainCommandRelay commandRelay;
    uint32_t localId;
    uint32_t assignedCoreId;
    uint32_t nextAnnouncementAt;

    static bool handleZigbeeMessage(
        Communication& communication,
        const Message& message,
        void* context
    );
    bool dispatch(const Message& message);

public:
    CoreMainRuntime(Communication& zigbeeCommunication, LampRegistry& lamps);

    bool begin(uint32_t mainLocalId);
    void poll();
    uint32_t coreId() const;
};
