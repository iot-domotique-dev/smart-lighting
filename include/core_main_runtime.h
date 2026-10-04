#pragma once

#include "communication.h"

/* Announces the MAIN's local identity and accepts the CORE-assigned ID. */
class CoreMainRuntime {
private:
    Communication& zigbee;
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
    explicit CoreMainRuntime(Communication& zigbeeCommunication);

    bool begin(uint32_t mainLocalId);
    void poll();
    uint32_t coreId() const;
};
