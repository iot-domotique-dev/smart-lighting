#include "core_command_service.h"

#include "core_module_service.h"
#include "message_id_generator.h"

CoreCommandService::CoreCommandService(DeviceRegistry& devices,
    MessageTracker& messages, Communication& commandLink)
    : registry(devices), tracker(messages), link(commandLink) {}

CoreCommandError CoreCommandService::submitPower(uint32_t destinationId,
    bool power, uint32_t& messageId, uint32_t requestedId) {
    messageId = 0;
    const Device* lamp = findDeviceById(registry, destinationId);
    if (lamp == nullptr || lamp->role != DeviceRole::LAMP)
        return CoreCommandError::UNKNOWN_DESTINATION;
    const Device* main = findDeviceById(registry, lamp->parentId);
    if (main == nullptr || main->role != DeviceRole::MAIN ||
        main->parentId != CORE_LOGICAL_ID ||
        main->id != makeCoreModuleId(CORE_LOGICAL_ID, main->localId) ||
        lamp->id != makeCoreModuleId(main->id, lamp->localId))
        return CoreCommandError::INVALID_ROUTE;
    if (lamp->status != DeviceStatus::ONLINE || main->status != DeviceStatus::ONLINE)
        return CoreCommandError::OFFLINE;
    if (link.transport == nullptr || !link.transport->isReady())
        return CoreCommandError::TRANSPORT_FAILURE;
    Message command = {};
    command.id = requestedId != 0 ? requestedId : generateMessageId();
    command.sourceId = CORE_LOGICAL_ID;
    command.destinationId = destinationId;
    command.type = MessageType::COMMAND;
    command.commandType = static_cast<int32_t>(ActionType::SET_LAMP_POWER);
    command.value = power ? 1 : 0;
    command.timestamp = millis();
    command.parentMainId = main->id;
    command.provisioningDeviceId = lamp->localId;
    if (findPendingMessage(tracker, command.id) != nullptr)
        return CoreCommandError::DUPLICATE_ID;
    if (!trackMessage(tracker, command)) return CoreCommandError::TRACKER_FULL;
    messageId = command.id;
    if (!sendMessage(link, command)) {
        const Message failure = makeCoreCommandAck(command, CORE_LOGICAL_ID,
            ExecutionStatus::FAILED, CoreCommandError::TRANSPORT_FAILURE);
        (void)processAck(tracker, failure);
        return CoreCommandError::TRANSPORT_FAILURE;
    }
    return CoreCommandError::NONE;
}

bool CoreCommandService::handleReply(const Message& message) {
    if (!isCoreCommandReply(message)) return false;
    const PendingMessage* pending = findPendingMessage(tracker,
        static_cast<uint32_t>(message.value2));
    if (pending != nullptr && pending->waitingForAck &&
        millis() - pending->firstSentAt >=
            MESSAGE_TIMEOUT * (static_cast<uint32_t>(MAX_MESSAGE_RETRIES) + 1)) {
        // A queued ACK must not win over the absolute deadline merely because
        // UART reception is polled before the timeout task in this iteration.
        (void)updateMessageTimeouts(tracker, link);
        return false;
    }
    return processAck(tracker, message);
}

void CoreCommandService::poll() { (void)updateMessageTimeouts(tracker, link); }

CoreCommandBridge::CoreCommandBridge(Communication& radio, Communication& uartLink)
    : zigbee(radio), coreLink(uartLink) {}

void CoreCommandBridge::setMain(uint32_t localId, uint32_t globalId) {
    mainLocalId = localId;
    mainCoreId = globalId;
}

bool CoreCommandBridge::handleFromCore(const Message& message) {
    if (!isCoreCommandMessage(message)) return false;
    CoreCommandError error = CoreCommandError::NONE;
    if (!validCorePowerCommand(message)) error = CoreCommandError::INVALID_COMMAND;
    else if (mainLocalId == 0 || mainCoreId == 0 || message.parentMainId != mainCoreId)
        error = CoreCommandError::INVALID_ROUTE;
    else if (!sendMessageVia(zigbee, message, mainLocalId))
        error = CoreCommandError::TRANSPORT_FAILURE;
    if (error != CoreCommandError::NONE) {
        (void)sendMessage(coreLink, makeCoreCommandAck(message, CORE_LOGICAL_ID,
            ExecutionStatus::FAILED, error));
    }
    return true;
}

bool CoreCommandBridge::handleFromZigbee(const Message& message) {
    if (!isCoreCommandReply(message)) return false;
    if (mainCoreId != 0 && message.parentMainId == mainCoreId)
        (void)sendMessage(coreLink, message);
    return true;
}

MainCommandRelay::MainCommandRelay(Communication& radio, LampRegistry& devices)
    : zigbee(radio), lamps(devices) {}

void MainCommandRelay::setMain(uint32_t localId, uint32_t globalId) {
    mainLocalId = localId;
    mainCoreId = globalId;
}

bool MainCommandRelay::handleMessage(const Message& message) {
    if (isCoreCommandReply(message)) {
        if (mainCoreId != 0 && message.parentMainId == mainCoreId &&
            message.executionStatus != ExecutionStatus::NOT_EXECUTED) {
            Lamp* lamp = findLamp(lamps, message.provisioningDeviceId);
            if (lamp != nullptr && lamp->identity.parentMainId == mainLocalId &&
                message.sourceId == makeCoreModuleId(mainCoreId, lamp->device.id))
                (void)sendMessageVia(zigbee, message, CORE_LOGICAL_ID,
                                    CommunicationRouteScope::CORE);
        }
        return true;
    }
    if (!isCoreCommandMessage(message)) return false;
    CoreCommandError error = CoreCommandError::NONE;
    Lamp* lamp = findLamp(lamps, message.provisioningDeviceId);
    if (!validCorePowerCommand(message)) error = CoreCommandError::INVALID_COMMAND;
    else if (mainCoreId == 0 || message.parentMainId != mainCoreId)
        error = CoreCommandError::INVALID_ROUTE;
    else if (lamp == nullptr) error = CoreCommandError::UNKNOWN_DESTINATION;
    else if (lamp->identity.pairingState != PairingState::PAIRED ||
             lamp->identity.parentMainId != mainLocalId ||
             message.destinationId != makeCoreModuleId(mainCoreId, lamp->device.id))
        error = CoreCommandError::INVALID_ROUTE;
    else if (lamp->device.status != DeviceStatus::ONLINE) error = CoreCommandError::OFFLINE;
    if (error == CoreCommandError::NONE) {
        // Acceptance is non-terminal. Each retry reaches the LAMP cache again.
        (void)sendMessageVia(zigbee, makeCoreCommandAck(message, mainCoreId,
            ExecutionStatus::NOT_EXECUTED), CORE_LOGICAL_ID,
            CommunicationRouteScope::CORE);
        if (!sendMessageVia(zigbee, message, lamp->device.id))
            error = CoreCommandError::TRANSPORT_FAILURE;
    }
    if (error != CoreCommandError::NONE) {
        (void)sendMessageVia(zigbee, makeCoreCommandAck(message,
            mainCoreId != 0 ? mainCoreId : message.parentMainId,
            ExecutionStatus::FAILED, error), CORE_LOGICAL_ID,
            CommunicationRouteScope::CORE);
    }
    return true;
}
