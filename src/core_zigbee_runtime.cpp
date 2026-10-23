#include <Arduino.h>
#include <string.h>

#include "core_zigbee_runtime.h"

#include "core_module_service.h"
#include "core_pairing_protocol.h"
#include "core_topology_protocol.h"
#include "message_router.h"

CoreZigbeeRuntime::CoreZigbeeRuntime(Communication& zigbeeCommunication)
    : zigbee(zigbeeCommunication), uart(), commandTransport(uart),
      commandLink{CommunicationTransportType::CORE_UART,
                  CommunicationState::INITIALIZING, CORE_LOGICAL_ID,
                  &commandTransport},
      commandBridge(zigbee, commandLink), mainLocalId(0), mainCoreId(0),
      helloSequence(1), uartReady(false) {}

bool CoreZigbeeRuntime::begin() {
    uartReady = uart.begin();
    commandLink.state = commandTransport.begin()
        ? CommunicationState::READY : CommunicationState::ERROR;
    registerApplicationMessageHandler(handleZigbeeMessage, this);
    if (uartReady) {
        CoreLinkPacket hello = {};
        hello.sequence = helloSequence++;
        hello.type = CoreLinkMessageType::HELLO;
        hello.coreId = CORE_LOGICAL_ID;
        (void)uart.send(hello);
    }
    return uartReady;
}

void CoreZigbeeRuntime::poll() {
    if (!uartReady) return;
    CoreLinkPacket packet = {};
    while (uart.receive(packet)) {
        handleUartPacket(packet);
    }
}

bool CoreZigbeeRuntime::handleZigbeeMessage(
    Communication&,
    const Message& message,
    void* context
) {
    CoreZigbeeRuntime* runtime = static_cast<CoreZigbeeRuntime*>(context);
    return runtime != nullptr && runtime->dispatchZigbeeMessage(message);
}

bool CoreZigbeeRuntime::dispatchZigbeeMessage(const Message& message) {
    if (message.type == MessageType::GROUP_CONTROL) {
        const GroupProtocolPayload& group = message.group;
        if (!isValidGroupProtocolPayload(group, GROUP_ZIGBEE_MEMBER_CHUNK) ||
            !isGroupProtocolResponse(group.action) ||
            group.mainLocalId != mainLocalId || group.mainCoreId != mainCoreId ||
            message.sourceId != mainLocalId ||
            message.destinationId != CORE_LOGICAL_ID ||
            message.parentMainId != mainCoreId || !uartReady) return true;
        const uint8_t chunkLimit = GROUP_UART_MEMBER_CHUNK;
        const uint8_t packetCount = group.action == GroupProtocolAction::MEMBER_CHUNK
            ? static_cast<uint8_t>((group.memberCount + chunkLimit - 1) / chunkLimit)
            : 1;
        for (uint8_t part = 0; part < packetCount; ++part) {
            CoreLinkPacket response = {};
            response.type = CoreLinkMessageType::GROUPS;
            response.sequence = static_cast<uint16_t>(group.operationId + part);
            response.group = group;
            if (group.action == GroupProtocolAction::MEMBER_CHUNK) {
                const uint8_t offset = static_cast<uint8_t>(part * chunkLimit);
                response.group.memberOffset = static_cast<uint8_t>(group.memberOffset + offset);
                const uint8_t remaining = static_cast<uint8_t>(group.memberCount - offset);
                response.group.memberCount = remaining > chunkLimit
                    ? chunkLimit : remaining;
                for (uint8_t index = 0; index < response.group.memberCount; ++index)
                    response.group.memberIds[index] = group.memberIds[offset + index];
            }
            if (!uart.send(response)) {
                Serial.println("[CORE-ZIGBEE] failed to forward group response over UART");
                break;
            }
        }
        return true;
    }
    if (message.type == MessageType::ACK &&
        commandBridge.handleFromZigbee(message)) {
        return true;
    }
    if (message.type == MessageType::STATE &&
        message.commandType == CORE_TOPOLOGY_ID_ACK) {
        bool accepted = false;
        if (readCoreIdAckMessage(message, mainLocalId, mainCoreId, accepted)) {
            CoreLinkPacket acknowledgement = {};
            acknowledgement.sequence = static_cast<uint16_t>(message.id);
            acknowledgement.type = CoreLinkMessageType::ACK;
            acknowledgement.localId = mainLocalId;
            acknowledgement.parentId = CORE_LOGICAL_ID;
            acknowledgement.resultCode = accepted ? 2 : 3;
            (void)uart.send(acknowledgement);
            return true;
        }
        return false;
    }

    uint32_t pairingMainLocalId = 0;
    uint32_t pairingSessionId = 0;
    if (readCorePairingCandidateMessage(message, pairingMainLocalId,
                                        pairingSessionId)) {
        CoreLinkPacket candidate = {};
        candidate.type = CoreLinkMessageType::PAIRING;
        candidate.sequence = static_cast<uint16_t>(pairingSessionId);
        candidate.pairingAction = CoreLinkPairingAction::CANDIDATE;
        candidate.localId = pairingMainLocalId;
        candidate.sessionId = pairingSessionId;
        candidate.capabilities = message.capabilities;
        strncpy(candidate.hardwareId, message.hardwareId,
                sizeof(candidate.hardwareId) - 1);
        if (pairingMainLocalId != mainLocalId || mainCoreId == 0 ||
            !uartReady || !uart.send(candidate)) {
            Serial.println("[CORE-ZIGBEE] failed to forward pairing candidate");
        }
        return true;
    }

    uint32_t operationSessionId = 0;
    uint32_t operationId = 0;
    CorePairingResultCode operationResult = CorePairingResultCode::NONE;
    uint32_t pairedDeviceId = 0;
    if (readCorePairingOperationResultMessage(message, mainLocalId,
            operationSessionId, operationId, operationResult, pairedDeviceId)) {
        CoreLinkPacket result = {};
        result.type = CoreLinkMessageType::PAIRING;
        result.sequence = static_cast<uint16_t>(operationId);
        result.pairingAction = CoreLinkPairingAction::OPERATION_RESULT;
        result.localId = mainLocalId;
        result.sessionId = operationSessionId;
        result.operationId = operationId;
        result.pairingResult = operationResult;
        result.deviceId = pairedDeviceId;
        strncpy(result.hardwareId, message.hardwareId,
                sizeof(result.hardwareId) - 1);
        if (mainCoreId == 0 || !uartReady || !uart.send(result))
            Serial.println("[CORE-ZIGBEE] failed to forward pairing result");
        return true;
    }

    CorePairingResultCode sessionResult = CorePairingResultCode::NONE;
    if (readCorePairingSessionResultMessage(message, mainLocalId,
            pairingSessionId, sessionResult)) {
        CoreLinkPacket result = {};
        result.type = CoreLinkMessageType::PAIRING;
        result.sequence = static_cast<uint16_t>(pairingSessionId);
        result.pairingAction = CoreLinkPairingAction::SESSION_RESULT;
        result.localId = mainLocalId;
        result.sessionId = pairingSessionId;
        result.pairingResult = sessionResult;
        if (mainCoreId == 0 || !uartReady || !uart.send(result))
            Serial.println("[CORE-ZIGBEE] failed to forward pairing session status");
        return true;
    }

    if (readCorePairingCandidateLimitMessage(message, mainLocalId,
                                              pairingSessionId)) {
        CoreLinkPacket limit = {};
        limit.type = CoreLinkMessageType::PAIRING;
        limit.sequence = static_cast<uint16_t>(pairingSessionId);
        limit.pairingAction = CoreLinkPairingAction::CANDIDATE_LIMIT;
        limit.localId = mainLocalId;
        limit.sessionId = pairingSessionId;
        (void)uart.send(limit);
        return true;
    }

    uint32_t removalOperationId = 0;
    CorePairingResultCode removalResult = CorePairingResultCode::NONE;
    uint32_t removalLampLocalId = 0;
    char removalHardwareId[sizeof(message.hardwareId)] = {};
    if (readCoreRemovalResultMessage(message, mainLocalId, mainCoreId,
            removalOperationId, removalResult, removalLampLocalId,
            removalHardwareId, sizeof(removalHardwareId))) {
        CoreLinkPacket result = {};
        result.type = CoreLinkMessageType::PAIRING;
        result.sequence = static_cast<uint16_t>(removalOperationId);
        result.pairingAction = CoreLinkPairingAction::REMOVAL_RESULT;
        result.localId = mainLocalId;
        result.parentId = mainCoreId;
        result.operationId = removalOperationId;
        result.pairingResult = removalResult;
        result.deviceId = removalLampLocalId;
        result.coreId = removalLampLocalId == 0
            ? 0 : makeCoreModuleId(mainCoreId, removalLampLocalId);
        strncpy(result.hardwareId, removalHardwareId,
                sizeof(result.hardwareId) - 1);
        (void)uart.send(result);
        return true;
    }

    CoreLinkPacket announcement = {};
    if (!coreAnnouncementFromZigbeeMessage(message, mainCoreId, mainLocalId,
                                           announcement)) {
        return false;
    }
    if (!uartReady || !uart.send(announcement)) {
        Serial.println("[CORE-ZIGBEE] unable to forward announcement over UART");
    } else if (message.type == MessageType::STATE) {
        mainLocalId = message.sourceId;
    }
    return true;
}

void CoreZigbeeRuntime::handleUartPacket(const CoreLinkPacket& packet) {
    if (packet.type == CoreLinkMessageType::GROUPS) {
        const GroupProtocolPayload& group = packet.group;
        if (!isValidGroupProtocolPayload(group, GROUP_UART_MEMBER_CHUNK) ||
            !isGroupProtocolRequest(group.action) ||
            group.mainLocalId != mainLocalId || group.mainCoreId != mainCoreId ||
            group.memberCount > GROUP_UART_MEMBER_CHUNK) return;
        Message request = {};
        request.type = MessageType::GROUP_CONTROL;
        request.id = group.operationId;
        request.sourceId = CORE_LOGICAL_ID;
        request.destinationId = mainLocalId;
        request.parentMainId = mainCoreId;
        request.status = MessageStatus::SENT;
        request.executionStatus = ExecutionStatus::NOT_EXECUTED;
        request.group = group;
        if (!sendMessage(zigbee, request))
            Serial.println("[CORE-ZIGBEE] could not forward group request to MAIN");
        return;
    }
    if (packet.type == CoreLinkMessageType::PAIRING) {
        if (packet.localId == 0 || packet.localId != mainLocalId ||
            mainCoreId == 0) return;
        Message request = {};
        bool valid = false;
        if (packet.pairingAction == CoreLinkPairingAction::OPEN_SESSION ||
            packet.pairingAction == CoreLinkPairingAction::CLOSE_SESSION) {
            valid = makeCorePairingSessionCommand(
                packet.localId, packet.sessionId,
                packet.pairingAction == CoreLinkPairingAction::OPEN_SESSION,
                packet.pairingAction == CoreLinkPairingAction::OPEN_SESSION
                    ? packet.pairingDurationSeconds : 0,
                request);
        } else if (packet.pairingAction == CoreLinkPairingAction::PAIR_CANDIDATE) {
            valid = makeCorePairingOperationCommand(packet.localId,
                packet.sessionId, packet.operationId, packet.hardwareId,
                packet.pairingName, request);
        } else if (packet.pairingAction == CoreLinkPairingAction::REMOVE_DEVICE) {
            valid = packet.parentId == mainCoreId && packet.deviceId != 0 &&
                packet.coreId == makeCoreModuleId(mainCoreId, packet.deviceId) &&
                makeCoreRemovalControlMessage(packet.localId, mainCoreId,
                    CoreRemovalCommand::REMOVE, packet.operationId,
                    packet.deviceId, packet.coreId, request);
        } else if (packet.pairingAction == CoreLinkPairingAction::QUERY_REMOVAL) {
            valid = packet.parentId == mainCoreId &&
                makeCoreRemovalControlMessage(packet.localId, mainCoreId,
                    CoreRemovalCommand::QUERY, packet.operationId, 0, 0,
                    request);
        }
        if (!valid) return;
        request.timestamp = millis();
        if (sendMessage(zigbee, request)) return;

        CoreLinkPacket failure = {};
        failure.type = CoreLinkMessageType::PAIRING;
        failure.localId = mainLocalId;
        if (packet.pairingAction == CoreLinkPairingAction::REMOVE_DEVICE ||
            packet.pairingAction == CoreLinkPairingAction::QUERY_REMOVAL) {
            failure.pairingAction = CoreLinkPairingAction::REMOVAL_RESULT;
            failure.operationId = packet.operationId;
            failure.parentId = packet.parentId;
            failure.deviceId = packet.deviceId;
            failure.coreId = packet.coreId;
            failure.pairingResult = CorePairingResultCode::UNCERTAIN;
        } else if (packet.pairingAction == CoreLinkPairingAction::PAIR_CANDIDATE) {
            failure.pairingAction = CoreLinkPairingAction::OPERATION_RESULT;
            failure.sessionId = packet.sessionId;
            failure.operationId = packet.operationId;
            failure.pairingResult = CorePairingResultCode::FAILED;
            strncpy(failure.hardwareId, packet.hardwareId,
                    sizeof(failure.hardwareId) - 1);
        } else {
            failure.pairingAction = CoreLinkPairingAction::SESSION_RESULT;
            failure.sessionId = packet.sessionId;
            failure.pairingResult = packet.pairingAction ==
                CoreLinkPairingAction::CLOSE_SESSION
                    ? CorePairingResultCode::UNCERTAIN
                    : CorePairingResultCode::FAILED;
        }
        (void)uart.send(failure);
        return;
    }
    if (packet.type == CoreLinkMessageType::COMMAND) {
        (void)commandBridge.handleFromCore(packet.message);
        return;
    }
    if (packet.type == CoreLinkMessageType::ID_ASSIGNMENT ||
        packet.type == CoreLinkMessageType::ERROR) {
        if (packet.localId == 0 ||
            (mainLocalId != 0 && packet.localId != mainLocalId)) {
            return;
        }

        const bool assigned = packet.type == CoreLinkMessageType::ID_ASSIGNMENT &&
            packet.coreId != 0 &&
            (packet.resultCode == static_cast<uint8_t>(CoreModuleUpdateResult::REGISTERED) ||
             packet.resultCode == static_cast<uint8_t>(CoreModuleUpdateResult::UPDATED));
        const uint32_t coreId = assigned ? packet.coreId : 0;
        Message response = {};
        if (makeCoreIdAssignedMessage(packet.localId, coreId,
                                      packet.resultCode, response)) {
            response.id = static_cast<uint32_t>(packet.sequence);
            response.timestamp = millis();
            if (sendMessage(zigbee, response) && assigned) {
                mainLocalId = packet.localId;
                mainCoreId = coreId;
                commandBridge.setMain(mainLocalId, mainCoreId);
                CoreLinkPacket acknowledgement = {};
                acknowledgement.sequence = packet.sequence;
                acknowledgement.type = CoreLinkMessageType::ACK;
                acknowledgement.localId = mainLocalId;
                acknowledgement.parentId = CORE_LOGICAL_ID;
                acknowledgement.resultCode = 1;
                (void)uart.send(acknowledgement);
            }
        }
        return;
    }

    if (packet.type == CoreLinkMessageType::ACK && packet.resultCode == 0) {
        Serial.println("[CORE-ZIGBEE] UART peer ready");
    }
}
