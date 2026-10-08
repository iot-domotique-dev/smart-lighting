#pragma once

#include "communication.h"
#include "core_command_service.h"
#include "core_command_uart.h"
#include "core_uart_transport.h"

/* Bridges topology messages between the existing Zigbee PAN and CORE-WIFI. */
class CoreZigbeeRuntime {
private:
    Communication& zigbee;
    CoreUartTransport uart;
    CoreCommandUart commandTransport;
    Communication commandLink;
    CoreCommandBridge commandBridge;
    uint32_t mainLocalId;
    uint32_t mainCoreId;
    uint16_t helloSequence;
    bool uartReady;

    static bool handleZigbeeMessage(
        Communication& communication,
        const Message& message,
        void* context
    );
    bool dispatchZigbeeMessage(const Message& message);
    void handleUartPacket(const CoreLinkPacket& packet);

public:
    explicit CoreZigbeeRuntime(Communication& zigbeeCommunication);

    bool begin();
    void poll();
};
