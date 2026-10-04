#include <Arduino.h>

#include "core_wifi_runtime.h"

#include "core_link_service.h"
#include "core_module_service.h"

CoreWifiRuntime::CoreWifiRuntime(DeviceRegistry& coreRegistry)
    : registry(coreRegistry), uart(), wifi(), uartReady(false) {}

bool CoreWifiRuntime::begin() {
    const bool wifiStarted = wifi.begin();
    uartReady = uart.begin();
    Serial.print("[CORE-WIFI] Wi-Fi adapter: ");
    Serial.println(wifiStarted ? "ready" : "error");
    Serial.print("[CORE-WIFI] UART link: ");
    Serial.println(uartReady ? "ready" : "error");
    return uartReady && wifiStarted;
}

void CoreWifiRuntime::poll() {
    if (!uartReady) return;
    CoreLinkPacket packet = {};
    while (uart.receive(packet)) {
        handlePacket(packet);
    }
}

void CoreWifiRuntime::handlePacket(const CoreLinkPacket& packet) {
    if (packet.type == CoreLinkMessageType::MODULE_ANNOUNCEMENT) {
        CoreLinkPacket response = {};
        const CoreModuleUpdateResult result = processCoreLinkAnnouncement(
            registry,
            packet,
            millis(),
            response
        );
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
            (void)setDeviceStatus(registry, mainId, DeviceStatus::ONLINE, millis());
            Serial.print("[CORE-WIFI] MAIN confirmed CORE id=");
            Serial.println(static_cast<unsigned long>(mainId));
        }
        return;
    }

    if (packet.type == CoreLinkMessageType::STATE && packet.coreId != 0) {
        const DeviceStatus status = packet.statusCode == 0
            ? DeviceStatus::ONLINE
            : DeviceStatus::OFFLINE;
        (void)setDeviceStatus(registry, packet.coreId, status, packet.timestamp);
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
