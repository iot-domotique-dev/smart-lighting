#include <string.h>

#include "core_pairing_protocol.h"
#include "core_pairing_state.h"

#include "core_module_service.h"
#include "device_registry.h"

namespace {
bool boundedText(const char* text, size_t capacity, bool allowEmpty = false) {
    if (text == nullptr || (!allowEmpty && text[0] == '\0')) return false;
    size_t length = 0;
    while (length < capacity && text[length] != '\0') ++length;
    return length < capacity && (allowEmpty || length != 0);
}

bool copyText(char* destination, size_t capacity, const char* source) {
    if (!boundedText(source, capacity)) return false;
    const size_t length = strlen(source);
    memcpy(destination, source, length + 1);
    return true;
}

bool isResultCode(uint32_t value) {
    return value > static_cast<uint32_t>(CorePairingResultCode::NONE) &&
           value <= static_cast<uint32_t>(CorePairingResultCode::ROUTE_UNAVAILABLE);
}

void initializeMainToCore(uint32_t mainLocalId, Message& message,
                           int32_t commandType, uint32_t correlationId) {
    message = {};
    message.id = correlationId;
    message.sourceId = mainLocalId;
    message.destinationId = CORE_LOGICAL_ID;
    message.type = MessageType::STATE;
    message.commandType = commandType;
    message.parentMainId = correlationId;
    message.deviceRole = static_cast<uint8_t>(DeviceRole::MAIN);
    message.status = MessageStatus::SENT;
    message.executionStatus = ExecutionStatus::NOT_EXECUTED;
}
}  // namespace

bool makeCorePairingSessionCommand(uint32_t mainLocalId, uint32_t sessionId,
                                   bool open, uint16_t durationSeconds,
                                   Message& message) {
    if (mainLocalId == 0 || sessionId == 0 ||
        (open && (durationSeconds < 15 || durationSeconds > 180)) ||
        (!open && durationSeconds != 0)) return false;
    message = {};
    message.id = sessionId;
    message.sourceId = CORE_LOGICAL_ID;
    message.destinationId = mainLocalId;
    message.type = MessageType::STATE;
    message.commandType = CORE_PAIRING_CONTROL_MESSAGE;
    message.value = static_cast<int32_t>(open
        ? CorePairingCommand::OPEN_SESSION : CorePairingCommand::CLOSE_SESSION);
    message.value2 = durationSeconds;
    return true;
}

bool readCorePairingSessionCommand(const Message& message,
                                   uint32_t expectedMainLocalId,
                                   CorePairingCommand& command,
                                   uint32_t& sessionId,
                                   uint16_t& durationSeconds) {
    if (expectedMainLocalId == 0 || message.type != MessageType::STATE ||
        message.commandType != CORE_PAIRING_CONTROL_MESSAGE ||
        message.sourceId != CORE_LOGICAL_ID ||
        message.destinationId != expectedMainLocalId || message.id == 0 ||
        (message.value != static_cast<int32_t>(CorePairingCommand::OPEN_SESSION) &&
         message.value != static_cast<int32_t>(CorePairingCommand::CLOSE_SESSION))) {
        return false;
    }
    const bool open = message.value ==
        static_cast<int32_t>(CorePairingCommand::OPEN_SESSION);
    if ((open && (message.value2 < 15 || message.value2 > 180)) ||
        (!open && message.value2 != 0)) return false;
    command = open ? CorePairingCommand::OPEN_SESSION : CorePairingCommand::CLOSE_SESSION;
    sessionId = message.id;
    durationSeconds = static_cast<uint16_t>(message.value2);
    return true;
}

bool makeCorePairingOperationCommand(uint32_t mainLocalId, uint32_t sessionId,
                                     uint32_t operationId, const char* hardwareId,
                                     const char* name, Message& message) {
    if (mainLocalId == 0 || sessionId == 0 || operationId == 0 ||
        !boundedText(hardwareId, sizeof(message.hardwareId)) ||
        !boundedText(name, sizeof(message.name)) ||
        !isValidCorePairingHardwareId(hardwareId) ||
        !isValidCorePairingName(name)) return false;
    message = {};
    message.id = operationId;
    message.sourceId = CORE_LOGICAL_ID;
    message.destinationId = mainLocalId;
    message.type = MessageType::STATE;
    message.commandType = CORE_PAIRING_CONTROL_MESSAGE;
    message.value = static_cast<int32_t>(CorePairingCommand::PAIR_CANDIDATE);
    message.parentMainId = sessionId;
    return copyText(message.hardwareId, sizeof(message.hardwareId), hardwareId) &&
           copyText(message.name, sizeof(message.name), name);
}

bool readCorePairingOperationCommand(const Message& message,
                                     uint32_t expectedMainLocalId,
                                     uint32_t& sessionId,
                                     uint32_t& operationId) {
    if (expectedMainLocalId == 0 || message.type != MessageType::STATE ||
        message.commandType != CORE_PAIRING_CONTROL_MESSAGE ||
        message.sourceId != CORE_LOGICAL_ID ||
        message.destinationId != expectedMainLocalId || message.id == 0 ||
        message.value != static_cast<int32_t>(CorePairingCommand::PAIR_CANDIDATE) ||
        message.parentMainId == 0 ||
        !boundedText(message.hardwareId, sizeof(message.hardwareId)) ||
        !boundedText(message.name, sizeof(message.name)) ||
        !isValidCorePairingHardwareId(message.hardwareId) ||
        !isValidCorePairingName(message.name)) return false;
    sessionId = message.parentMainId;
    operationId = message.id;
    return true;
}

bool makeCorePairingCandidateMessage(uint32_t mainLocalId, uint32_t sessionId,
                                     const ProvisioningPacket& candidate,
                                     Message& message) {
    if (mainLocalId == 0 || sessionId == 0 ||
        candidate.type != ProvisioningPacketType::DEVICE_ANNOUNCE ||
        candidate.role != DeviceRole::LAMP ||
        candidate.pairingState != PairingState::UNPAIRED ||
        !boundedText(candidate.hardwareId, sizeof(candidate.hardwareId)) ||
        !isValidCorePairingHardwareId(candidate.hardwareId)) return false;
    initializeMainToCore(mainLocalId, message, CORE_PAIRING_CANDIDATE_MESSAGE,
                         sessionId);
    message.capabilities = candidate.capabilities;
    message.deviceRole = static_cast<uint8_t>(DeviceRole::LAMP);
    message.pairingState = static_cast<uint8_t>(PairingState::UNPAIRED);
    return copyText(message.hardwareId, sizeof(message.hardwareId),
                    candidate.hardwareId);
}

bool readCorePairingCandidateMessage(const Message& message,
                                     uint32_t& mainLocalId,
                                     uint32_t& sessionId) {
    if (message.type != MessageType::STATE ||
        message.commandType != CORE_PAIRING_CANDIDATE_MESSAGE ||
        message.sourceId == 0 || message.destinationId != CORE_LOGICAL_ID ||
        message.id == 0 || message.parentMainId != message.id ||
        message.deviceRole != static_cast<uint8_t>(DeviceRole::LAMP) ||
        message.pairingState != static_cast<uint8_t>(PairingState::UNPAIRED) ||
        !boundedText(message.hardwareId, sizeof(message.hardwareId)) ||
        !isValidCorePairingHardwareId(message.hardwareId)) return false;
    mainLocalId = message.sourceId;
    sessionId = message.id;
    return true;
}

bool makeCorePairingSessionResultMessage(uint32_t mainLocalId,
                                         uint32_t sessionId,
                                         CorePairingResultCode result,
                                         Message& message) {
    if (mainLocalId == 0 || sessionId == 0 ||
        result == CorePairingResultCode::NONE ||
        result > CorePairingResultCode::ROUTE_UNAVAILABLE) return false;
    initializeMainToCore(mainLocalId, message,
                         CORE_PAIRING_SESSION_RESULT_MESSAGE, sessionId);
    message.value = static_cast<int32_t>(result);
    return true;
}

bool readCorePairingSessionResultMessage(const Message& message,
                                         uint32_t expectedMainLocalId,
                                         uint32_t& sessionId,
                                         CorePairingResultCode& result) {
    if (expectedMainLocalId == 0 || message.type != MessageType::STATE ||
        message.commandType != CORE_PAIRING_SESSION_RESULT_MESSAGE ||
        message.sourceId != expectedMainLocalId ||
        message.destinationId != CORE_LOGICAL_ID || message.id == 0 ||
        message.parentMainId != message.id ||
        !isResultCode(static_cast<uint32_t>(message.value))) return false;
    sessionId = message.id;
    result = static_cast<CorePairingResultCode>(message.value);
    return true;
}

bool makeCorePairingOperationResultMessage(uint32_t mainLocalId,
                                           uint32_t sessionId,
                                           uint32_t operationId,
                                           CorePairingResultCode result,
                                           uint32_t deviceId,
                                           const char* hardwareId,
                                           Message& message) {
    if (mainLocalId == 0 || sessionId == 0 || operationId == 0 ||
        result == CorePairingResultCode::NONE ||
        result > CorePairingResultCode::ROUTE_UNAVAILABLE ||
        (hardwareId != nullptr && hardwareId[0] != '\0' &&
         !boundedText(hardwareId, sizeof(message.hardwareId)))) return false;
    initializeMainToCore(mainLocalId, message,
                         CORE_PAIRING_OPERATION_RESULT_MESSAGE, operationId);
    message.parentMainId = sessionId;
    message.value = static_cast<int32_t>(result);
    message.value2 = static_cast<int32_t>(deviceId);
    if (hardwareId != nullptr && hardwareId[0] != '\0' &&
        !copyText(message.hardwareId, sizeof(message.hardwareId), hardwareId)) {
        return false;
    }
    return true;
}

bool readCorePairingOperationResultMessage(const Message& message,
                                           uint32_t expectedMainLocalId,
                                           uint32_t& sessionId,
                                           uint32_t& operationId,
                                           CorePairingResultCode& result,
                                           uint32_t& deviceId) {
    if (expectedMainLocalId == 0 || message.type != MessageType::STATE ||
        message.commandType != CORE_PAIRING_OPERATION_RESULT_MESSAGE ||
        message.sourceId != expectedMainLocalId ||
        message.destinationId != CORE_LOGICAL_ID || message.id == 0 ||
        message.parentMainId == 0 ||
        !isResultCode(static_cast<uint32_t>(message.value)) ||
        message.value2 < 0) return false;
    sessionId = message.parentMainId;
    operationId = message.id;
    result = static_cast<CorePairingResultCode>(message.value);
    deviceId = static_cast<uint32_t>(message.value2);
    return true;
}

bool makeCorePairingCandidateLimitMessage(uint32_t mainLocalId,
                                          uint32_t sessionId,
                                          Message& message) {
    if (mainLocalId == 0 || sessionId == 0) return false;
    initializeMainToCore(mainLocalId, message,
                         CORE_PAIRING_CANDIDATE_LIMIT_MESSAGE, sessionId);
    return true;
}

bool readCorePairingCandidateLimitMessage(const Message& message,
                                          uint32_t expectedMainLocalId,
                                          uint32_t& sessionId) {
    if (expectedMainLocalId == 0 || message.type != MessageType::STATE ||
        message.commandType != CORE_PAIRING_CANDIDATE_LIMIT_MESSAGE ||
        message.sourceId != expectedMainLocalId ||
        message.destinationId != CORE_LOGICAL_ID || message.id == 0 ||
        message.parentMainId != message.id) return false;
    sessionId = message.id;
    return true;
}
