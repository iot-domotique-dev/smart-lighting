#pragma once

#include "core_uart_transport.h"
#include "device_registry.h"
#include "wifi_transport.h"

/* Owns the global V7 inventory and the CORE-WIFI physical services. */
class CoreWifiRuntime {
private:
    DeviceRegistry& registry;
    CoreUartTransport uart;
    WiFiTransport wifi;
    bool uartReady;

    void handlePacket(const CoreLinkPacket& packet);

public:
    explicit CoreWifiRuntime(DeviceRegistry& coreRegistry);

    bool begin();
    void poll();
    WiFiTransport& wifiTransport();
    CoreUartTransport& uartTransport();
};
