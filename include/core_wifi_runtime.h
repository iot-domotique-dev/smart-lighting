#pragma once

#include "core_http_api.h"
#include "core_uart_transport.h"
#include "core_command_uart.h"
#include "core_command_service.h"
#include "device_registry.h"
#include "event_bus.h"
#include "wifi_transport.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

/* Owns the global V7 inventory and the CORE-WIFI physical services. */
class CoreWifiRuntime {
private:
    DeviceRegistry& registry;
    CoreUartTransport uart;
    CoreCommandUart commandUart;
    Communication commandLink;
    MessageTracker& tracker;
    CoreCommandService commands;
    WiFiTransport wifi;
    SemaphoreHandle_t registryMutex;
    CoreHttpApi api;
    bool uartReady;

    void handlePacket(const CoreLinkPacket& packet);
    void pollConsole();
    void printCommandState(uint32_t id);

public:
    CoreWifiRuntime(DeviceRegistry& coreRegistry, MessageTracker& messages);

    bool begin();
    void poll();
    void refreshRegistryStatus(EventBus& eventBus);
    WiFiTransport& wifiTransport();
    CoreUartTransport& uartTransport();
};
