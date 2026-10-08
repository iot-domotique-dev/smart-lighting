#pragma once

#include "action.h"
#include "device_registry.h"
#include "message.h"

enum class CoreCommandError : int32_t {
    NONE = 0,
    UNKNOWN_DESTINATION = 10,
    OFFLINE = 11,
    INVALID_COMMAND = 12,
    INVALID_ROUTE = 13,
    TRANSPORT_FAILURE = 14,
    DEDUP_FULL = 15,
    TRACKER_FULL = 16,
    DUPLICATE_ID = 17
};

bool isCoreCommandMessage(const Message& message);
bool isCoreCommandReply(const Message& message);
bool validCorePowerCommand(const Message& message);
Message makeCoreCommandAck(const Message& command, uint32_t sourceId,
                           ExecutionStatus execution,
                           CoreCommandError error = CoreCommandError::NONE);
CoreCommandError resolveCoreLampCommand(LampRegistry& lamps,
                                       const Message& command,
                                       uint32_t& targetId,
                                       uint32_t& replyHop);
const char* coreCommandErrorName(CoreCommandError error);

// Explicit serial diagnostic: suppress one final LAMP ACK, never execution.
void requestDropNextCoreExecutionAck();
bool consumeDropNextCoreExecutionAck();
