#include <Arduino.h>

#include "core_wifi_runtime.h"

#include "core_link_service.h"
#include "core_module_service.h"
#include "device_manager.h"

#if __has_include("wifi_credentials.h")
#include "wifi_credentials.h"
#else
#define SMART_LIGHTING_WIFI_SSID ""
#define SMART_LIGHTING_WIFI_PASSWORD ""
#endif

CoreWifiRuntime::CoreWifiRuntime(DeviceRegistry& coreRegistry)
    : registry(coreRegistry), uart(), wifi(), registryMutex(nullptr),
      api(coreRegistry, wifi, uart, registryMutex), uartReady(false) {}

bool CoreWifiRuntime::begin() {
    if (registryMutex == nullptr) registryMutex = xSemaphoreCreateMutex();
    if (registryMutex == nullptr) {
        Serial.println("[CORE-WIFI] registry mutex creation failed");
        return false;
    }

    const bool wifiStarted = wifi.begin();
    if (wifiStarted) {
        if (SMART_LIGHTING_WIFI_SSID[0] == '\0') {
            Serial.println("[CORE-WIFI] Wi-Fi credentials not configured; HTTP server starts but is not reachable over Wi-Fi yet");
        } else if (!wifi.connect(SMART_LIGHTING_WIFI_SSID,
                                 SMART_LIGHTING_WIFI_PASSWORD)) {
            Serial.println("[CORE-WIFI] initial Wi-Fi connection could not start; HTTP API stays active and reconnect will continue");
        }
    }
    uartReady = uart.begin();
    const bool apiStarted = api.begin();
    Serial.print("[CORE-WIFI] Wi-Fi adapter: ");
    Serial.println(wifiStarted ? "ready" : "error");
    Serial.print("[CORE-WIFI] UART link: ");
    Serial.println(uartReady ? "ready" : "error");
    Serial.print("[CORE-WIFI] HTTP API: ");
    Serial.println(apiStarted ? "ready on port 80" : "error");
    // Wi-Fi association is optional for startup: keep HTTP/UART services alive
    // so a temporary AP outage cannot prevent the local API server from starting.
    return uartReady && apiStarted;
}

void CoreWifiRuntime::poll() {
    wifi.poll();
    if (!uartReady) return;
    CoreLinkPacket packet = {};
    while (uart.receive(packet)) {
        handlePacket(packet);
    }
}

void CoreWifiRuntime::refreshRegistryStatus(EventBus& eventBus) {
    if (registryMutex == nullptr ||
        xSemaphoreTake(registryMutex, pdMS_TO_TICKS(10)) != pdTRUE) {
        return;
    }
    Device* core = findDeviceById(registry, CORE_LOGICAL_ID);
    if (core != nullptr) updateDeviceSeen(*core, &eventBus);
    updateDeviceStatus(registry, &eventBus);
    (void)xSemaphoreGive(registryMutex);
}

void CoreWifiRuntime::handlePacket(const CoreLinkPacket& packet) {
    if (packet.type == CoreLinkMessageType::MODULE_ANNOUNCEMENT) {
        CoreLinkPacket response = {};
        if (registryMutex == nullptr ||
            xSemaphoreTake(registryMutex, pdMS_TO_TICKS(100)) != pdTRUE) {
            Serial.println("[CORE-WIFI] registry busy; announcement deferred");
            return;
        }
        const CoreModuleUpdateResult result = processCoreLinkAnnouncement(
            registry,
            packet,
            millis(),
            response
        );
        (void)xSemaphoreGive(registryMutex);
        if (!uart.send(response)) {
            Serial.println("[CORE-WIFI] failed to return module identity over UART");
        } else if (result == CoreModuleUpdateResult::REGISTERED ||
                   result == CoreModuleUpdateResult::UPDATED) {
            Serial.print("[CORE-WIFI] module id=");
            Serial.println(static_cast<unsigned long>(response.coreId));
        } else {
            Serial.print("[CORE-WIFI] announcement rejected, result=");
            Serial.println(static_cast<unsigned>(result));
        }
        return;
    }

    if (packet.type == CoreLinkMessageType::HELLO) {
        CoreLinkPacket acknowledgement = {};
        acknowledgement.sequence = packet.sequence;
        acknowledgement.type = CoreLinkMessageType::ACK;
        acknowledgement.resultCode = 0;
        (void)uart.send(acknowledgement);
        return;
    }

    if (packet.type == CoreLinkMessageType::ACK) {
        if (packet.resultCode == 2 && packet.localId != 0) {
            const uint32_t mainId = makeCoreModuleId(
                CORE_LOGICAL_ID,
                packet.localId
            );
            if (registryMutex != nullptr &&
                xSemaphoreTake(registryMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
                (void)setDeviceStatus(registry, mainId, DeviceStatus::ONLINE, millis());
                (void)xSemaphoreGive(registryMutex);
            }
            Serial.print("[CORE-WIFI] MAIN confirmed CORE id=");
            Serial.println(static_cast<unsigned long>(mainId));
        }
        return;
    }

    if (packet.type == CoreLinkMessageType::STATE && packet.coreId != 0) {
        const DeviceStatus status = packet.statusCode == 0
            ? DeviceStatus::ONLINE
            : DeviceStatus::OFFLINE;
        if (registryMutex != nullptr &&
            xSemaphoreTake(registryMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            (void)setDeviceStatus(registry, packet.coreId, status, packet.timestamp);
            (void)xSemaphoreGive(registryMutex);
        }
        return;
    }

    if (packet.type == CoreLinkMessageType::ERROR) {
        Serial.print("[CORE-WIFI] UART peer error, result=");
        Serial.println(static_cast<unsigned>(packet.resultCode));
    }
}

WiFiTransport& CoreWifiRuntime::wifiTransport() {
    return wifi;
}

CoreUartTransport& CoreWifiRuntime::uartTransport() {
    return uart;
}
