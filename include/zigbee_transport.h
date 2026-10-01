#pragma once

#include "communication_transport.h"

/*
 * CommunicationTransportInterface adapter for Espressif's ESP-Zigbee SDK.
 * The SDK implementation is compiled only by the ESP32-C6 IDF environments.
 * Arduino/native builds keep the type available but do not emulate Zigbee.
 */
class ZigbeeTransport : public CommunicationTransportInterface {
private:
    volatile bool started;

public:
    ZigbeeTransport();

    bool begin() override;
    bool send(const Message& message) override;
    bool receive(Message& message) override;
    bool isReady() const override;
    const char* name() const override;

    /* Used by the SDK worker after esp_zigbee_start() succeeds. */
    void markStackStarted();
};
