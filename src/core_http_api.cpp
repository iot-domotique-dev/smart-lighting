#include "core_http_api.h"

#if defined(SMART_LIGHTING_CORE_WIFI)

#include <Arduino.h>

#include "core_module_service.h"
#include "esp_app_desc.h"
#include "esp_timer.h"

namespace {
char responseBuffer[CORE_API_MAX_RESPONSE_BYTES];

const char* statusText(uint16_t status) {
    switch (status) {
        case 200: return "200 OK";
        case 400: return "400 Bad Request";
        case 404: return "404 Not Found";
        case 405: return "405 Method Not Allowed";
        case 500: return "500 Internal Server Error";
        case 503: return "503 Service Unavailable";
        default: return "500 Internal Server Error";
    }
}
}  // namespace

CoreHttpApi::CoreHttpApi(DeviceRegistry& coreRegistry,
                         WiFiTransport& wifiTransport,
                         CoreUartTransport& uartTransport,
                         SemaphoreHandle_t& coreRegistryMutex)
    : registry(coreRegistry), wifi(wifiTransport), uart(uartTransport),
      registryMutex(&coreRegistryMutex), started(false), server(nullptr) {}

bool CoreHttpApi::begin() {
    if (started) return true;
    if (registryMutex == nullptr || *registryMutex == nullptr) return false;

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.uri_match_fn = httpd_uri_match_wildcard;

    if (httpd_start(&server, &config) != ESP_OK) {
        server = nullptr;
        Serial.println("[CORE-WIFI] HTTP server start failed");
        return false;
    }

    httpd_uri_t apiRoute = {};
    apiRoute.uri = "/api/v1/*";
    apiRoute.method = static_cast<httpd_method_t>(HTTP_ANY);
    apiRoute.handler = handleRequest;
    apiRoute.user_ctx = this;
    if (httpd_register_uri_handler(server, &apiRoute) != ESP_OK) {
        (void)httpd_stop(server);
        server = nullptr;
        Serial.println("[CORE-WIFI] HTTP route registration failed");
        return false;
    }

    started = true;
    return true;
}

bool CoreHttpApi::isReady() const {
    return started;
}

esp_err_t CoreHttpApi::handleRequest(httpd_req_t* request) {
    if (request == nullptr || request->user_ctx == nullptr) return ESP_FAIL;
    CoreHttpApi* api = static_cast<CoreHttpApi*>(request->user_ctx);
    const char* method = request->method == HTTP_GET ? "GET" : "UNSUPPORTED";
    const esp_app_desc_t* description = esp_app_get_description();
    const CoreApiRuntimeInfo runtime = {
        static_cast<uint64_t>(esp_timer_get_time() / 1000),
        description == nullptr ? "unknown" : description->version,
        api->wifi.isReady(),
        api->uart.isReady()
    };

    static DeviceRegistry registrySnapshot = {};
    if (api->registryMutex == nullptr || *api->registryMutex == nullptr ||
        xSemaphoreTake(*api->registryMutex, pdMS_TO_TICKS(250)) != pdTRUE) {
        initDeviceRegistry(registrySnapshot);
    } else {
        registrySnapshot = api->registry;
        const uint8_t safeCount = registrySnapshot.count > MAX_DEVICES
            ? MAX_DEVICES
            : registrySnapshot.count;
        for (uint8_t index = 0; index < safeCount; ++index) {
            registrySnapshot.devices[index].name = registrySnapshot.names[index];
        }
        (void)xSemaphoreGive(*api->registryMutex);
    }

    size_t responseLength = 0;
    const uint16_t status = handleCoreApiRequest(
        registrySnapshot,
        runtime,
        method,
        request->uri,
        responseBuffer,
        sizeof(responseBuffer),
        responseLength
    );

    if (status != 200 &&
        httpd_resp_set_status(request, statusText(status)) != ESP_OK) {
        return ESP_FAIL;
    }
    if (httpd_resp_set_type(request, "application/json; charset=utf-8") != ESP_OK) {
        return ESP_FAIL;
    }
    if (status == 405 && httpd_resp_set_hdr(request, "Allow", "GET") != ESP_OK) {
        return ESP_FAIL;
    }
    return httpd_resp_send(request, responseBuffer,
                           static_cast<ssize_t>(responseLength));
}

#else

CoreHttpApi::CoreHttpApi(DeviceRegistry& coreRegistry,
                         WiFiTransport& wifiTransport,
                         CoreUartTransport& uartTransport,
                         SemaphoreHandle_t& coreRegistryMutex)
    : registry(coreRegistry), wifi(wifiTransport), uart(uartTransport),
      registryMutex(&coreRegistryMutex), started(false) {}

bool CoreHttpApi::begin() { return false; }
bool CoreHttpApi::isReady() const { return started; }

#endif
