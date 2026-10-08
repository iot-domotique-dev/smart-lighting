#include "core_command_uart.h"

CoreCommandUart::CoreCommandUart(CoreUartTransport& transport)
    : uart(transport), nextSequence(1) {}

bool CoreCommandUart::begin() {
    return uart.isReady();
}

bool CoreCommandUart::send(const Message& message) {
    if (!isReady()) return false;
    CoreLinkPacket packet = {};
    if (message.type == MessageType::COMMAND) {
        packet.type = CoreLinkMessageType::COMMAND;
    } else if (message.type == MessageType::ACK) {
        packet.type = CoreLinkMessageType::COMMAND_RESULT;
    } else {
        return false;
    }
    packet.sequence = nextSequence++;
    packet.message = message;
    return uart.send(packet);
}

bool CoreCommandUart::receive(Message&) {
    // CoreWifiRuntime/CoreZigbeeRuntime also handle topology on this UART.
    return false;
}

bool CoreCommandUart::isReady() const {
    return uart.isReady();
}

const char* CoreCommandUart::name() const {
    return "CORE_UART_COMMANDS";
}
