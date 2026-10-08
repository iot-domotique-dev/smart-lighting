#pragma once

#include "communication_transport.h"
#include "core_uart_transport.h"

/* Message adapter for the existing CORE-to-CORE UART. The owning runtime
 * keeps responsibility for polling and dispatching received frames. */
class CoreCommandUart : public CommunicationTransportInterface {
private:
    CoreUartTransport& uart;
    uint16_t nextSequence;

public:
    explicit CoreCommandUart(CoreUartTransport& transport);

    bool begin() override;
    bool send(const Message& message) override;
    bool receive(Message& message) override;
    bool isReady() const override;
    const char* name() const override;
};
