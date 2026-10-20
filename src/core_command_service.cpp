#include "core_command_service.h"

#include "core_module_service.h"
#include "message_id_generator.h"

namespace {
bool isTrackedOutputCommand(const Message& message) {
    return isCoreCommandMessage(message) &&
        (message.commandType == static_cast<int32_t>(ActionType::SET_LAMP_POWER) ||
         message.commandType == static_cast<int32_t>(ActionType::SET_LAMP_BRIGHTNESS));
}

bool sameLampIdentity(const Device& device, const Message& command) {
    return device.role == DeviceRole::LAMP &&
        device.id == command.destinationId &&
        device.parentId == command.parentMainId &&
        device.localId == command.provisioningDeviceId;
}

bool outputExecutionUnknown(const Device& lamp) {
    return lamp.powerExecutionUnknown || lamp.brightnessExecutionUnknown;
}

bool deviceOnline(const Device& device) {
    return device.status == DeviceStatus::ONLINE;
}

void markOutputExecutionUnknown(Device& lamp) {
    lamp.powerExecutionUnknown = true;
    lamp.brightnessExecutionUnknown = true;
    markLastConfirmedOutputStale(lamp);
}

bool hasPendingOutput(const MessageTracker& tracker, uint32_t destinationId) {
    for (uint8_t index = 0; index < tracker.count; ++index) {
        const PendingMessage& pending = tracker.messages[index];
        if (pending.waitingForAck && isTrackedOutputCommand(pending.message) &&
            pending.message.destinationId == destinationId) {
            return true;
        }
    }
    return false;
}

uint8_t countPendingOutput(const MessageTracker& tracker,
                           uint32_t destinationId,
                           const PendingMessage** onlyPending = nullptr) {
    uint8_t count = 0;
    if (onlyPending != nullptr) *onlyPending = nullptr;
    for (uint8_t index = 0; index < tracker.count; ++index) {
        const PendingMessage& pending = tracker.messages[index];
        if (pending.waitingForAck && isTrackedOutputCommand(pending.message) &&
            pending.message.destinationId == destinationId) {
            ++count;
            if (onlyPending != nullptr) *onlyPending = &pending;
        }
    }
    return count;
}

uint8_t countUnresolvedOutput(const MessageTracker& tracker,
                              uint32_t destinationId,
                              const PendingMessage** onlyPending = nullptr) {
    uint8_t count = 0;
    if (onlyPending != nullptr) *onlyPending = nullptr;
    for (uint8_t index = 0; index < tracker.count; ++index) {
        const PendingMessage& pending = tracker.messages[index];
        if ((pending.waitingForAck || pending.timedOut) &&
            isTrackedOutputCommand(pending.message) &&
            pending.message.destinationId == destinationId) {
            ++count;
            if (onlyPending != nullptr) *onlyPending = &pending;
        }
    }
    return count;
}

bool isPowerOff(const PendingMessage& pending) {
    return pending.message.commandType ==
               static_cast<int32_t>(ActionType::SET_LAMP_POWER) &&
           pending.message.value == 0;
}

void completeSupersededCommand(MessageTracker& tracker,
                               uint32_t supersededId,
                               uint32_t destinationId) {
    if (supersededId == 0) return;
    PendingMessage* superseded = findPendingMessage(tracker, supersededId);
    if (superseded == nullptr ||
        superseded->message.destinationId != destinationId ||
        !isTrackedOutputCommand(superseded->message) ||
        superseded->completed) {
        return;
    }
    superseded->waitingForAck = false;
    superseded->completed = true;
    superseded->timedOut = false;
    superseded->message.executionStatus = ExecutionStatus::FAILED;
    superseded->message.value2 = static_cast<int32_t>(ExecutionStatus::FAILED);
    superseded->completedAt = millis();
}

bool applyConfirmedOutput(Device& lamp, const Message& command) {
    const bool isBrightness = command.commandType ==
        static_cast<int32_t>(ActionType::SET_LAMP_BRIGHTNESS);
    if (isBrightness) {
        lamp.lastConfirmedBrightness = static_cast<uint8_t>(command.value);
        lamp.lastConfirmedBrightnessStatus = deviceOnline(lamp)
            ? LastConfirmedBrightnessStatus::CONFIRMED
            : LastConfirmedBrightnessStatus::STALE;
        if (command.value > 0) {
            lamp.lastConfirmedNonzeroBrightness =
                static_cast<uint8_t>(command.value);
            lamp.lastConfirmedNonzeroBrightnessKnown = true;
        }
        lamp.lastConfirmedPower = command.value == 0
            ? LastConfirmedPower::OFF : LastConfirmedPower::ON;
    } else {
        lamp.lastConfirmedPower = command.value == 0
            ? LastConfirmedPower::OFF : LastConfirmedPower::ON;
        if (hasDeviceCapability(lamp, DEVICE_CAP_BRIGHTNESS)) {
            if (command.value == 0) {
                lamp.lastConfirmedBrightness = 0;
                lamp.lastConfirmedBrightnessStatus = deviceOnline(lamp)
                    ? LastConfirmedBrightnessStatus::CONFIRMED
                    : LastConfirmedBrightnessStatus::STALE;
            } else if (lamp.lastConfirmedNonzeroBrightnessKnown) {
                lamp.lastConfirmedBrightness = lamp.lastConfirmedNonzeroBrightness;
                lamp.lastConfirmedBrightnessStatus = deviceOnline(lamp)
                    ? LastConfirmedBrightnessStatus::CONFIRMED
                    : LastConfirmedBrightnessStatus::STALE;
            } else {
                lamp.lastConfirmedBrightness = 0;
                lamp.lastConfirmedBrightnessStatus =
                    LastConfirmedBrightnessStatus::UNKNOWN;
            }
        }
    }
    lamp.lastConfirmedPowerStatus = deviceOnline(lamp)
        ? LastConfirmedPowerStatus::CONFIRMED
        : LastConfirmedPowerStatus::STALE;
    return true;
}
}  // namespace

CoreCommandService::CoreCommandService(DeviceRegistry& devices,
    MessageTracker& messages, Communication& commandLink)
    : registry(devices), tracker(messages), link(commandLink) {}

CoreCommandError CoreCommandService::submitPower(uint32_t destinationId,
    bool power, uint32_t& messageId, uint32_t requestedId) {
    return submitOutputCommand(destinationId,
        static_cast<int32_t>(ActionType::SET_LAMP_POWER), power ? 1 : 0,
        messageId, requestedId);
}

CoreCommandError CoreCommandService::submitBrightness(uint32_t destinationId,
    int32_t percent, uint32_t& messageId, uint32_t requestedId) {
    return submitOutputCommand(destinationId,
        static_cast<int32_t>(ActionType::SET_LAMP_BRIGHTNESS), percent,
        messageId, requestedId);
}

CoreCommandError CoreCommandService::submitOutputCommand(uint32_t destinationId,
    int32_t commandType, int32_t value, uint32_t& messageId,
    uint32_t requestedId) {
    messageId = 0;
    if ((commandType == static_cast<int32_t>(ActionType::SET_LAMP_POWER) &&
         value != 0 && value != 1) ||
        (commandType == static_cast<int32_t>(ActionType::SET_LAMP_BRIGHTNESS) &&
         (value < 0 || value > 100))) {
        return CoreCommandError::INVALID_COMMAND;
    }
    const Device* lamp = findDeviceById(registry, destinationId);
    if (lamp == nullptr || lamp->role != DeviceRole::LAMP)
        return CoreCommandError::UNKNOWN_DESTINATION;
    const Device* main = findDeviceById(registry, lamp->parentId);
    if (main == nullptr || main->role != DeviceRole::MAIN ||
        main->parentId != CORE_LOGICAL_ID ||
        main->id != makeCoreModuleId(CORE_LOGICAL_ID, main->localId) ||
        lamp->id != makeCoreModuleId(main->id, lamp->localId))
        return CoreCommandError::INVALID_ROUTE;
    if (commandType == static_cast<int32_t>(ActionType::SET_LAMP_BRIGHTNESS) &&
        !hasDeviceCapability(*lamp, DEVICE_CAP_BRIGHTNESS)) {
        return CoreCommandError::UNSUPPORTED_CAPABILITY;
    }
    if (lamp->status != DeviceStatus::ONLINE || main->status != DeviceStatus::ONLINE)
        return CoreCommandError::OFFLINE;
    if (link.transport == nullptr || !link.transport->isReady())
        return CoreCommandError::TRANSPORT_FAILURE;
    Message command = {};
    command.id = requestedId != 0 ? requestedId : generateMessageId();
    command.sourceId = CORE_LOGICAL_ID;
    command.destinationId = destinationId;
    command.type = MessageType::COMMAND;
    command.commandType = commandType;
    command.value = value;
    command.timestamp = millis();
    command.parentMainId = main->id;
    command.provisioningDeviceId = lamp->localId;
    if (findPendingMessage(tracker, command.id) != nullptr)
        return CoreCommandError::DUPLICATE_ID;

    const bool priorityOff = commandType ==
        static_cast<int32_t>(ActionType::SET_LAMP_POWER) && value == 0;
    const uint8_t pendingCount = countPendingOutput(tracker, destinationId);
    const PendingMessage* unresolvedOutput = nullptr;
    const uint8_t unresolvedCount = countUnresolvedOutput(
        tracker, destinationId, &unresolvedOutput);
    PendingMessage* superseded = unresolvedOutput == nullptr ? nullptr
        : findPendingMessage(tracker, unresolvedOutput->message.id);
    const bool canSupersede = priorityOff && unresolvedCount == 1 &&
        superseded != nullptr && !superseded->priorityOff &&
        !isPowerOff(*superseded);
    if ((!priorityOff && pendingCount != 0) ||
        (priorityOff && unresolvedCount != 0 && !canSupersede)) {
        if (priorityOff) {
            Serial.print("[CORE-COMMAND] priority OFF rejected: unresolved outputs=");
            Serial.print(static_cast<unsigned>(unresolvedCount));
            if (superseded != nullptr) {
                Serial.print(" blocker=");
                Serial.print(static_cast<unsigned long>(superseded->message.id));
                Serial.print(" waiting=");
                Serial.print(superseded->waitingForAck ? "yes" : "no");
                Serial.print(" timed_out=");
                Serial.print(superseded->timedOut ? "yes" : "no");
                Serial.print(" priority_off=");
                Serial.print(superseded->priorityOff ? "yes" : "no");
            }
            Serial.println();
        }
        return CoreCommandError::COMMAND_IN_PROGRESS;
    }

    if (!trackMessage(tracker, command)) return CoreCommandError::TRACKER_FULL;
    messageId = command.id;
    PendingMessage* submitted = findPendingMessage(tracker, command.id);
    if (priorityOff && superseded != nullptr) {
        // value2 is already carried by the existing UART and Zigbee message
        // formats. It identifies the one earlier output command that this OFF
        // fences at LAMP if the earlier packet is still in transit.
        command.value2 = static_cast<int32_t>(superseded->message.id);
        if (submitted != nullptr) submitted->message.value2 = command.value2;
    }
    if (submitted != nullptr) submitted->priorityOff = priorityOff;
    if (superseded != nullptr) superseded->supersededByPriorityOff = true;
    if (!sendMessage(link, command)) {
        if (superseded != nullptr) superseded->supersededByPriorityOff = false;
        if (submitted != nullptr) submitted->priorityOff = false;
        const Message failure = makeCoreCommandAck(command, CORE_LOGICAL_ID,
            ExecutionStatus::FAILED, CoreCommandError::TRANSPORT_FAILURE);
        (void)processAck(tracker, failure);
        return CoreCommandError::TRANSPORT_FAILURE;
    }

    if (submitted != nullptr) {
        submitted->executionOrderAmbiguous = outputExecutionUnknown(*lamp) &&
            superseded == nullptr;
    }
    return CoreCommandError::NONE;
}

bool CoreCommandService::handleReply(const Message& message) {
    if (!isCoreCommandReply(message)) return false;
    PendingMessage* pending = findPendingMessage(tracker,
        static_cast<uint32_t>(message.value2));
    if (pending != nullptr && pending->waitingForAck &&
        millis() - pending->firstSentAt >=
            MESSAGE_TIMEOUT * (static_cast<uint32_t>(MAX_MESSAGE_RETRIES) + 1)) {
        // A queued ACK must not win over the absolute deadline merely because
        // UART reception is polled before the timeout task in this iteration.
        poll();
        return false;
    }

    const Message original = pending != nullptr ? pending->message : Message{};
    const bool executionOrderAmbiguous = pending != nullptr &&
        pending->executionOrderAmbiguous;
    const bool priorityOff = pending != nullptr && pending->priorityOff;
    const bool supersededByPriorityOff = pending != nullptr &&
        pending->supersededByPriorityOff;
    if (!processAck(tracker, message)) return false;

    if (!isTrackedOutputCommand(original)) {
        return true;
    }

    Device* lamp = findDeviceById(registry, original.destinationId);
    if (lamp == nullptr || !sameLampIdentity(*lamp, original)) return true;
    if (supersededByPriorityOff &&
        message.executionStatus == ExecutionStatus::EXECUTED) {
        markLastConfirmedOutputStale(*lamp);
        return true;
    }
    if (priorityOff && message.executionStatus == ExecutionStatus::FAILED) {
        const uint32_t supersededId = static_cast<uint32_t>(original.value2);
        PendingMessage* superseded = findPendingMessage(tracker, supersededId);
        if (superseded != nullptr) superseded->supersededByPriorityOff = false;
        markOutputExecutionUnknown(*lamp);
        return true;
    }
    if (message.executionStatus != ExecutionStatus::EXECUTED) return true;
    if (priorityOff && original.value2 != 0) {
        lamp->powerExecutionUnknown = false;
        lamp->brightnessExecutionUnknown = false;
        completeSupersededCommand(tracker,
            static_cast<uint32_t>(original.value2), original.destinationId);
    }
    if (executionOrderAmbiguous || outputExecutionUnknown(*lamp)) {
        markLastConfirmedOutputStale(*lamp);
        return true;
    }
    (void)applyConfirmedOutput(*lamp, original);
    return true;
}

bool CoreCommandService::hasUnresolvedPowerCommand(uint32_t destinationId) const {
    const Device* lamp = findDeviceById(registry, destinationId);
    return lamp == nullptr || outputExecutionUnknown(*lamp) ||
           hasPendingOutput(tracker, destinationId);
}

bool CoreCommandService::hasUnresolvedOutputCommand(uint32_t destinationId) const {
    return hasUnresolvedPowerCommand(destinationId);
}

bool CoreCommandService::canBeginRemoval(uint32_t destinationId) const {
    const Device* lamp = findDeviceById(registry, destinationId);
    if (lamp == nullptr || lamp->role != DeviceRole::LAMP ||
        outputExecutionUnknown(*lamp)) return false;
    return countPendingOutput(tracker, destinationId) <= 1;
}

void CoreCommandService::poll() {
    (void)updateMessageTimeouts(tracker, link);
    for (uint8_t index = 0; index < tracker.count; ++index) {
        const PendingMessage& pending = tracker.messages[index];
        if (!pending.timedOut || !isTrackedOutputCommand(pending.message)) continue;
        Device* lamp = findDeviceById(registry, pending.message.destinationId);
        if (lamp == nullptr || !sameLampIdentity(*lamp, pending.message)) continue;
        markOutputExecutionUnknown(*lamp);
    }
}

CoreCommandBridge::CoreCommandBridge(Communication& radio, Communication& uartLink)
    : zigbee(radio), coreLink(uartLink) {}

void CoreCommandBridge::setMain(uint32_t localId, uint32_t globalId) {
    mainLocalId = localId;
    mainCoreId = globalId;
}

bool CoreCommandBridge::handleFromCore(const Message& message) {
    if (!isCoreCommandMessage(message)) return false;
    CoreCommandError error = CoreCommandError::NONE;
    if (!validCoreOutputCommand(message)) error = CoreCommandError::INVALID_COMMAND;
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
    if (!validCoreOutputCommand(message)) error = CoreCommandError::INVALID_COMMAND;
    else if (mainCoreId == 0 || message.parentMainId != mainCoreId)
        error = CoreCommandError::INVALID_ROUTE;
    else if (lamp == nullptr) error = CoreCommandError::UNKNOWN_DESTINATION;
    else if (lamp->identity.pairingState != PairingState::PAIRED ||
             lamp->identity.parentMainId != mainLocalId ||
             message.destinationId != makeCoreModuleId(mainCoreId, lamp->device.id))
        error = CoreCommandError::INVALID_ROUTE;
    else if (lamp->identity.removalPending)
        error = CoreCommandError::REMOVAL_IN_PROGRESS;
    else if (message.commandType ==
                 static_cast<int32_t>(ActionType::SET_LAMP_BRIGHTNESS) &&
             !hasDeviceCapability(lamp->device, DEVICE_CAP_BRIGHTNESS))
        error = CoreCommandError::UNSUPPORTED_CAPABILITY;
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
