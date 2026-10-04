#include <Arduino.h>

#include "core_zigbee_runtime.h"

#include "core_module_service.h"
#include "core_topology_protocol.h"
#include "message_router.h"

CoreZigbeeRuntime::CoreZigbeeRuntime(Communication& zigbeeCommunication)
    : zigbee(zigbeeCommunication), uart(), mainLocalId(0), mainCoreId(0),
      helloSequence(1), uartReady(false) {}

bool CoreZigbeeRuntime::begin() {
    uartReady = uart.begin();
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
