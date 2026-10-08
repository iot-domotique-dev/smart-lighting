#include <Arduino.h>

#include "core_wifi_runtime.h"

#include "core_link_service.h"
#include "core_module_service.h"
#include "device_manager.h"
#include "core_console.h"
#include "message_id_generator.h"
#include "esp_random.h"

#if __has_include("wifi_credentials.h")
#include "wifi_credentials.h"
#else
#define SMART_LIGHTING_WIFI_SSID ""
#define SMART_LIGHTING_WIFI_PASSWORD ""
#endif

#ifndef SMART_LIGHTING_API_TOKEN
#define SMART_LIGHTING_API_TOKEN ""
#endif

CoreWifiRuntime::CoreWifiRuntime(DeviceRegistry& coreRegistry, MessageTracker& messages)
    : registry(coreRegistry), uart(), commandUart(uart),
      commandLink{CommunicationTransportType::CORE_UART, CommunicationState::READY,
                  CORE_LOGICAL_ID, &commandUart}, tracker(messages),
      commands(coreRegistry, messages, commandLink), wifi(), registryMutex(nullptr),
      api(coreRegistry, wifi, uart, registryMutex, commands, messages,
          SMART_LIGHTING_API_TOKEN), uartReady(false) {}

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
    seedMessageIds(esp_random());
    if (!startCoreConsole()) Serial.println("[CORE-WIFI] command console unavailable");
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
    if (uartReady) {
        CoreLinkPacket packet = {};
        while (uart.receive(packet)) {
            handlePacket(packet);
        }
    }
    pollConsole();
    if (registryMutex != nullptr &&
        xSemaphoreTake(registryMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        commands.poll();
        (void)xSemaphoreGive(registryMutex);
    }
}

void CoreWifiRuntime::printCommandState(uint32_t id) {
    PendingMessage snapshot = {};
    bool found = false;
    if (registryMutex != nullptr &&
        xSemaphoreTake(registryMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        const PendingMessage* pending = findPendingMessage(tracker, id);
        if (pending != nullptr) {
            snapshot = *pending;
            found = true;
        }
        (void)xSemaphoreGive(registryMutex);
    }
    if (!found) {
        Serial.println("[CORE-COMMAND] unknown or evicted message id");
        return;
    }
    const PendingMessage* pending = &snapshot;
    Serial.print("[CORE-COMMAND] id=");
    Serial.print(static_cast<unsigned long>(id));
    Serial.print(" state=");
    if (pending->timedOut) Serial.print("expired (execution unknown)");
    else if (pending->completed && pending->message.executionStatus == ExecutionStatus::EXECUTED)
        Serial.print("executed");
    else if (pending->completed) Serial.print("failed");
    else if (pending->accepted) Serial.print("accepted (waiting execution)");
    else Serial.print("sent (waiting acceptance/execution)");
    Serial.print(" retries=");
    Serial.println(static_cast<unsigned>(pending->retryCount));
    if (pending->completed && !pending->timedOut &&
        pending->message.executionStatus == ExecutionStatus::FAILED) {
        Serial.print("[CORE-COMMAND] error=");
        if (pending->message.value2 == static_cast<int32_t>(ExecutionStatus::FAILED))
            Serial.println("execution_failed");
        else Serial.println(coreCommandErrorName(
            static_cast<CoreCommandError>(pending->message.value2)));
    }
}

void CoreWifiRuntime::pollConsole() {
    CoreConsoleRequest request = {};
    while (receiveCoreConsoleRequest(request)) {
        if (request.query) { printCommandState(request.id); continue; }
        if (registryMutex == nullptr ||
            xSemaphoreTake(registryMutex, pdMS_TO_TICKS(100)) != pdTRUE) {
            Serial.println("[CORE-COMMAND] refused: registry busy");
            continue;
        }
        uint32_t id = 0;
        const CoreCommandError result = commands.submitPower(request.id, request.power, id);
        (void)xSemaphoreGive(registryMutex);
        if (result != CoreCommandError::NONE) {
            Serial.print("[CORE-COMMAND] refused: ");
            Serial.println(coreCommandErrorName(result));
        }
        if (id != 0) printCommandState(id);
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
    if (packet.type == CoreLinkMessageType::COMMAND_RESULT) {
        bool handled = false;
        if (registryMutex != nullptr &&
            xSemaphoreTake(registryMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            handled = commands.handleReply(packet.message);
            (void)xSemaphoreGive(registryMutex);
        }
        if (handled)
            printCommandState(static_cast<uint32_t>(packet.message.value2));
        else Serial.println("[CORE-COMMAND] unexpected, late ACK, or busy tracker ignored");
        return;
    }
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
