#pragma once

#include "core_command_protocol.h"
#include "message_tracker.h"

class CoreCommandService {
    DeviceRegistry& registry;
    MessageTracker& tracker;
    Communication& link;
public:
    CoreCommandService(DeviceRegistry& devices, MessageTracker& messages,
                       Communication& commandLink);
    CoreCommandError submitPower(uint32_t destinationId, bool power,
                                uint32_t& messageId, uint32_t requestedId = 0);
    bool handleReply(const Message& message);
    void poll();
};

class CoreCommandBridge {
    Communication& zigbee;
    Communication& coreLink;
    uint32_t mainLocalId = 0;
    uint32_t mainCoreId = 0;
public:
    CoreCommandBridge(Communication& radio, Communication& uartLink);
    void setMain(uint32_t localId, uint32_t globalId);
    bool handleFromCore(const Message& message);
    bool handleFromZigbee(const Message& message);
};

class MainCommandRelay {
    Communication& zigbee;
    LampRegistry& lamps;
    uint32_t mainLocalId = 0;
    uint32_t mainCoreId = 0;
public:
    MainCommandRelay(Communication& radio, LampRegistry& devices);
    void setMain(uint32_t localId, uint32_t globalId);
    bool handleMessage(const Message& message);
};
