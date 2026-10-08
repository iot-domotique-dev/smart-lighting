#pragma once

#include "core_api_service.h"
#include "core_command_service.h"
#include "core_uart_transport.h"
#include "device_registry.h"
#include "message_tracker.h"
#include "wifi_transport.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#if defined(SMART_LIGHTING_CORE_WIFI)
#include "esp_http_server.h"
#endif

/* HTTP adapter owned exclusively by C6-WIFI. */
class CoreHttpApi {
private:
    DeviceRegistry& registry;
    WiFiTransport& wifi;
    CoreUartTransport& uart;
    CoreCommandService& commands;
    MessageTracker& tracker;
    const char* apiToken;
    SemaphoreHandle_t* registryMutex;
    bool started;
#if defined(SMART_LIGHTING_CORE_WIFI)
    httpd_handle_t server;
    static esp_err_t handleRequest(httpd_req_t* request);
#endif

public:
    CoreHttpApi(DeviceRegistry& coreRegistry,
                WiFiTransport& wifiTransport,
                CoreUartTransport& uartTransport,
                SemaphoreHandle_t& coreRegistryMutex,
                CoreCommandService& commandService,
                MessageTracker& messageTracker,
                const char* commandApiToken);

    bool begin();
    bool isReady() const;
};
