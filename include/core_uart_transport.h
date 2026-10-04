#pragma once

#include "core_link_codec.h"

/* UART point-to-point link between the two ESP32-C6 CORE boards. */
class CoreUartTransport {
private:
    int port;
    bool started;
    CoreLinkFrameDecoder decoder;

public:
    explicit CoreUartTransport(int uartPort = -1);

    bool begin();
    bool send(const CoreLinkPacket& packet);
    bool receive(CoreLinkPacket& packet);
    bool isReady() const;
    const char* name() const;
    uint16_t decoderErrorCount() const;
};
