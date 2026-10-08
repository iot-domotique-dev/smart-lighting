#include "core_command_protocol.h"

#include "core_module_service.h"
#include "message_id_generator.h"
#include <atomic>

namespace { std::atomic<bool> dropNextExecutionAck{false}; }

void requestDropNextCoreExecutionAck() { dropNextExecutionAck.store(true); }
bool consumeDropNextCoreExecutionAck() { return dropNextExecutionAck.exchange(false); }

bool isCoreCommandMessage(const Message& message) {
    return message.type == MessageType::COMMAND &&
           message.sourceId == CORE_LOGICAL_ID && message.parentMainId != 0;
}

bool isCoreCommandReply(const Message& message) {
    return message.type == MessageType::ACK &&
           message.destinationId == CORE_LOGICAL_ID && message.parentMainId != 0;
}

bool validCorePowerCommand(const Message& message) {
    return isCoreCommandMessage(message) && message.id != 0 &&
           message.destinationId != 0 && message.provisioningDeviceId != 0 &&
           message.commandType == static_cast<int32_t>(ActionType::SET_LAMP_POWER) &&
           (message.value == 0 || message.value == 1);
}

Message makeCoreCommandAck(const Message& command, uint32_t sourceId,
                           ExecutionStatus execution, CoreCommandError error) {
    Message ack = {};
    ack.id = generateMessageId();
    ack.sourceId = sourceId;
    ack.destinationId = command.sourceId;
    ack.timestamp = millis();
    ack.type = MessageType::ACK;
    ack.commandType = command.commandType;
    ack.value = error == CoreCommandError::NONE
        ? static_cast<int32_t>(execution) : static_cast<int32_t>(error);
    ack.value2 = static_cast<int32_t>(command.id);
    ack.status = MessageStatus::PENDING;
    ack.executionStatus = execution;
    ack.parentMainId = command.parentMainId;
    ack.provisioningDeviceId = command.provisioningDeviceId;
    return ack;
}

CoreCommandError resolveCoreLampCommand(LampRegistry& lamps,
                                       const Message& command,
                                       uint32_t& targetId, uint32_t& replyHop) {
    targetId = 0;
    replyHop = 0;
    for (uint8_t i = 0; i < lamps.count; ++i) {
        if (lamps.lamps[i].identity.pairingState == PairingState::PAIRED) {
            replyHop = lamps.lamps[i].identity.parentMainId;
            break;
        }
    }
    if (!validCorePowerCommand(command)) return CoreCommandError::INVALID_COMMAND;
    Lamp* lamp = findLamp(lamps, command.provisioningDeviceId);
    if (lamp == nullptr) return CoreCommandError::UNKNOWN_DESTINATION;
    const uint32_t mainId = makeCoreModuleId(CORE_LOGICAL_ID,
                                            lamp->identity.parentMainId);
    if (lamp->identity.pairingState != PairingState::PAIRED ||
        lamp->identity.parentMainId == 0 || command.parentMainId != mainId ||
        command.destinationId != makeCoreModuleId(mainId, lamp->device.id)) {
        return CoreCommandError::INVALID_ROUTE;
    }
    replyHop = lamp->identity.parentMainId;
    targetId = lamp->device.id;
    if (lamp->device.status != DeviceStatus::ONLINE) return CoreCommandError::OFFLINE;
    return CoreCommandError::NONE;
}

const char* coreCommandErrorName(CoreCommandError error) {
    switch (error) {
        case CoreCommandError::NONE: return "ok";
        case CoreCommandError::UNKNOWN_DESTINATION: return "unknown_destination";
        case CoreCommandError::OFFLINE: return "offline";
        case CoreCommandError::INVALID_COMMAND: return "invalid_command";
        case CoreCommandError::INVALID_ROUTE: return "invalid_route";
        case CoreCommandError::TRANSPORT_FAILURE: return "transport_failure";
        case CoreCommandError::DEDUP_FULL: return "dedup_full";
        case CoreCommandError::TRACKER_FULL: return "tracker_full";
        case CoreCommandError::DUPLICATE_ID: return "duplicate_id";
    }
    return "execution_failed";
}
