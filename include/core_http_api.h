#pragma once

#include "core_api_service.h"
#include "core_command_service.h"
#include "core_pairing_state.h"
#include "core_uart_transport.h"
#include "device_registry.h"
#include "message_tracker.h"
#include "wifi_transport.h"
#include "group_manager.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#if defined(SMART_LIGHTING_CORE_WIFI)
#include "esp_http_server.h"
#endif

enum class CoreGroupOperationState : uint8_t {
    EMPTY, SENT, ACCEPTED, PERSISTED, COMPLETED, FAILED, UNCERTAIN
};

struct CoreGroupMemberResult {
    uint32_t localId;
    uint32_t childCommandId;
    GroupProtocolResult result;
    uint8_t index;
    bool valid;
};

struct CoreGroupOperationRecord {
    uint32_t id;
    uint32_t startedAt;
    uint32_t lastQueryAt;
    uint32_t retainedAt;
    uint32_t resultRevision;
    CoreGroupOperationState state;
    GroupProtocolResult result;
    GroupProtocolPayload request;
    uint8_t expectedGroupCount;
    uint8_t receivedGroupCount;
    uint8_t receivedMembersForCurrent;
    uint8_t statusQueryCount;
    CoreGroupMemberResult members[MAX_GROUP_LAMPS];
    bool listStarted;
    bool mainFinalResultReceived;
};

struct CoreGroupReadOperationRecord : CoreGroupOperationRecord {
    GroupRegistry snapshot;
};

constexpr uint8_t CORE_GROUP_OPERATION_SLOTS = 32;

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
    CorePairingState pairing;
    uint32_t removalOperationId;
    uint32_t removalTargetId;
    uint32_t removalMainLocalId;
    uint32_t removalMainCoreId;
    uint32_t removalLampLocalId;
    uint32_t removalLastQueryAt;
    CorePairingResultCode removalResult;
    SemaphoreHandle_t groupStateMutex;
    uint32_t activeGroupOperationId;
    uint8_t nextGroupOperationSlot;
    CoreGroupOperationRecord groupOperations[CORE_GROUP_OPERATION_SLOTS];
    /* One in-flight read context; completed reads are released by their HTTP handler. */
    CoreGroupReadOperationRecord groupReadOperation;
    bool started;
#if defined(SMART_LIGHTING_CORE_WIFI)
    httpd_handle_t server;
    static esp_err_t handleRequest(httpd_req_t* request);
    esp_err_t handlePairingRequest(httpd_req_t* request);
    esp_err_t handleRemovalRequest(httpd_req_t* request);
    esp_err_t handleGroupRequest(httpd_req_t* request);
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
    void handlePairingPacket(const CoreLinkPacket& packet);
    void handleGroupPacket(const CoreLinkPacket& packet);
    void pollPairing(uint32_t now);
    void pollGroupOperations(uint32_t now);
};
