#include <Arduino.h>

#include "zigbee_transport.h"

#if defined(SMART_LIGHTING_ZIGBEE)

#include <string.h>

#include "esp_err.h"
#include "esp_log.h"
#include "esp_zigbee.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "roles.h"
#include "zigbee_message_codec.h"

namespace {
constexpr char TAG[] = "smart-lighting-zigbee";
constexpr uint8_t APP_ENDPOINT = 1;
constexpr uint16_t APP_PROFILE_ID = 0x0104;  // Home Automation profile.
constexpr uint16_t APP_CLUSTER_ID = 0xFC01;  // Manufacturer-specific cluster.
constexpr uint16_t BROADCAST_RX_ON_WHEN_IDLE = 0xFFFD;
constexpr uint8_t PEER_CAPACITY = 10;
constexpr uint8_t RX_QUEUE_LENGTH = 12;
constexpr char ZIGBEE_STORAGE_PARTITION[] = "zb_storage";

struct PeerRoute {
    uint32_t logicalId;
    char hardwareId[32];
    uint16_t shortAddress;
    bool used;
};

ZigbeeTransport* activeTransport = nullptr;
QueueHandle_t incomingMessages = nullptr;
PeerRoute peerRoutes[PEER_CAPACITY] = {};
portMUX_TYPE peerRoutesMux = portMUX_INITIALIZER_UNLOCKED;
volatile bool activeNetwork = false;
volatile bool stackStarted = false;

void rememberRoute(const Message& message, uint16_t shortAddress) {
    portENTER_CRITICAL(&peerRoutesMux);
    int8_t freeIndex = -1;
    int8_t matchIndex = -1;
    for (uint8_t i = 0; i < PEER_CAPACITY; ++i) {
        if (!peerRoutes[i].used && freeIndex < 0) {
            freeIndex = static_cast<int8_t>(i);
        }
        if (peerRoutes[i].used &&
            ((message.sourceId != 0 && peerRoutes[i].logicalId == message.sourceId) ||
             (message.hardwareId[0] != '\0' &&
              strncmp(peerRoutes[i].hardwareId, message.hardwareId,
                      sizeof(peerRoutes[i].hardwareId)) == 0))) {
            matchIndex = static_cast<int8_t>(i);
            break;
        }
    }

    const int8_t index = matchIndex >= 0 ? matchIndex : freeIndex;
    if (index >= 0) {
        PeerRoute& route = peerRoutes[index];
        route.logicalId = message.sourceId;
        route.shortAddress = shortAddress;
        route.used = true;
        if (message.hardwareId[0] != '\0') {
            strncpy(route.hardwareId, message.hardwareId,
                    sizeof(route.hardwareId) - 1);
            route.hardwareId[sizeof(route.hardwareId) - 1] = '\0';
        }
    }
    portEXIT_CRITICAL(&peerRoutesMux);
}

bool findRoute(const Message& message, uint16_t& shortAddress) {
    bool found = false;
    portENTER_CRITICAL(&peerRoutesMux);
    for (uint8_t i = 0; i < PEER_CAPACITY; ++i) {
        if (!peerRoutes[i].used) {
            continue;
        }
        const bool logicalMatch = message.destinationId != 0 &&
                                  peerRoutes[i].logicalId == message.destinationId;
        const bool hardwareMatch = message.hardwareId[0] != '\0' &&
                                   strncmp(peerRoutes[i].hardwareId,
                                           message.hardwareId,
                                           sizeof(peerRoutes[i].hardwareId)) == 0;
        if (logicalMatch || hardwareMatch) {
            shortAddress = peerRoutes[i].shortAddress;
            found = true;
            break;
        }
    }
    portEXIT_CRITICAL(&peerRoutesMux);
    return found;
}

bool isBroadcast(const Message& message) {
    return message.destinationId == 0 &&
           (message.type == MessageType::DEVICE_ANNOUNCE ||
            (message.hardwareId[0] == '\0' &&
             message.type != MessageType::PAIR_REQUEST &&
             message.type != MessageType::PAIR_ACCEPT &&
             message.type != MessageType::PAIR_CONFIRM &&
             message.type != MessageType::PAIR_REJECT));
}

bool onApsDataIndication(const ezb_apsde_data_ind_t* indication) {
    if (indication == nullptr || indication->status != 0 ||
        indication->profile_id != APP_PROFILE_ID ||
        indication->cluster_id != APP_CLUSTER_ID ||
        indication->dst_endpoint != APP_ENDPOINT ||
        indication->src_address.addr_mode != EZB_ADDR_MODE_SHORT ||
        indication->asdu == nullptr) {
        return false;
    }

    Message message = {};
    if (!decodeZigbeeMessage(indication->asdu, indication->asdu_length, message)) {
        ESP_LOGW(TAG, "Discarded invalid application frame (%u bytes)",
                 indication->asdu_length);
        return true;
    }

    const uint16_t sourceAddress = indication->src_address.u.short_addr;
    rememberRoute(message, sourceAddress);
    if (incomingMessages != nullptr &&
        xQueueSend(incomingMessages, &message, 0) == pdTRUE) {
        Serial.println("[ZIGBEE] message received");
    } else {
        ESP_LOGW(TAG, "Application receive queue full; frame dropped");
    }
    return true;
}

void onApsDataConfirm(const ezb_apsde_data_confirm_t* confirmation) {
    if (confirmation != nullptr && confirmation->status == 0) {
        Serial.println("[ZIGBEE] message sent");
    } else if (confirmation != nullptr) {
        ESP_LOGW(TAG, "APS send failed (status %u)", confirmation->status);
    }
}

void startCommissioning(ezb_bdb_comm_mode_t mode) {
    const ezb_err_t result = ezb_bdb_start_top_level_commissioning(mode);
    if (result != EZB_ERR_NONE) {
        ESP_LOGW(TAG, "Commissioning request failed (mode %u, error 0x%04x)",
                 static_cast<unsigned>(mode), result);
    }
}

void retryCommissioningTask(void* context) {
    const ezb_bdb_comm_mode_t mode = static_cast<ezb_bdb_comm_mode_t>(
        reinterpret_cast<uintptr_t>(context));
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_zigbee_lock_acquire(portMAX_DELAY);
    startCommissioning(mode);
    esp_zigbee_lock_release();
    vTaskDelete(nullptr);
}

void scheduleCommissioningRetry(ezb_bdb_comm_mode_t mode) {
    (void)xTaskCreate(
        retryCommissioningTask,
        "zb_commission_retry",
        3072,
        reinterpret_cast<void*>(static_cast<uintptr_t>(mode)),
        4,
        nullptr);
}

bool onAppSignal(const ezb_app_signal_t* signal) {
    if (signal == nullptr) {
        return false;
    }

    const ezb_app_signal_type_t signalType = ezb_app_signal_get_type(signal);
    const void* params = ezb_app_signal_get_params(signal);
    switch (signalType) {
        case EZB_ZDO_SIGNAL_SKIP_STARTUP:
            Serial.println("[ZIGBEE] initializing commissioning");
            startCommissioning(EZB_BDB_MODE_INITIALIZATION);
            return true;

        case EZB_BDB_SIGNAL_DEVICE_FIRST_START:
        case EZB_BDB_SIGNAL_DEVICE_REBOOT: {
            const ezb_bdb_comm_status_t status = params == nullptr
                ? static_cast<ezb_bdb_comm_status_t>(0xFF)
                : *static_cast<const ezb_bdb_comm_status_t*>(params);
            if (status != EZB_BDB_STATUS_SUCCESS) {
                ESP_LOGW(TAG, "Device initialization failed (status %u)", status);
                return true;
            }

            if (!ezb_bdb_is_factory_new()) {
                activeNetwork = true;
                Serial.println("[ZIGBEE] network joined");
                if (DEVICE_ROLE == DeviceRole::MAIN) {
                    (void)ezb_bdb_open_network(180);
                }
            } else if (DEVICE_ROLE == DeviceRole::MAIN) {
                startCommissioning(EZB_BDB_MODE_NETWORK_FORMATION);
            } else {
                startCommissioning(EZB_BDB_MODE_NETWORK_STEERING);
            }
            return true;
        }

        case EZB_BDB_SIGNAL_FORMATION: {
            const ezb_bdb_comm_status_t status = params == nullptr
                ? static_cast<ezb_bdb_comm_status_t>(0xFF)
                : *static_cast<const ezb_bdb_comm_status_t*>(params);
            if (status == EZB_BDB_STATUS_SUCCESS) {
                activeNetwork = true;
                Serial.println("[ZIGBEE] network formed");
                (void)ezb_bdb_open_network(180);
            } else {
                ESP_LOGW(TAG, "Network formation failed (status %u)", status);
                scheduleCommissioningRetry(EZB_BDB_MODE_NETWORK_FORMATION);
            }
            return true;
        }

        case EZB_BDB_SIGNAL_STEERING: {
            const ezb_bdb_comm_status_t status = params == nullptr
                ? static_cast<ezb_bdb_comm_status_t>(0xFF)
                : *static_cast<const ezb_bdb_comm_status_t*>(params);
            if (status == EZB_BDB_STATUS_SUCCESS) {
                activeNetwork = true;
                Serial.println("[ZIGBEE] network joined");
            } else {
                ESP_LOGW(TAG, "Network steering failed (status %u)", status);
                scheduleCommissioningRetry(EZB_BDB_MODE_NETWORK_STEERING);
            }
            return true;
        }

        case EZB_ZDO_SIGNAL_LEAVE:
            activeNetwork = false;
            return true;

        case EZB_ZDO_SIGNAL_DEVICE_ANNCE:
            Serial.println("[ZIGBEE] device connected");
            return true;

        default:
            return false;
    }
}

void zigbeeTask(void*) {
    esp_zigbee_config_t config = {};
    config.device_config.device_type = DEVICE_ROLE == DeviceRole::MAIN
        ? EZB_NWK_DEVICE_TYPE_COORDINATOR
        : EZB_NWK_DEVICE_TYPE_ROUTER;
    config.device_config.install_code_policy = false;
    config.device_config.zczr_config.max_children = 10;
    config.platform_config.storage_partition_name = ZIGBEE_STORAGE_PARTITION;
    config.platform_config.radio_config.radio_mode = ESP_ZIGBEE_RADIO_MODE_NATIVE;

    if (esp_zigbee_init(&config) != ESP_OK) {
        ESP_LOGE(TAG, "esp_zigbee_init failed");
        vTaskDelete(nullptr);
        return;
    }

    (void)ezb_app_signal_add_handler(onAppSignal);
    ezb_apsde_data_indication_handler_register(onApsDataIndication);
    ezb_apsde_data_confirm_handler_register(onApsDataConfirm);

    Serial.println("[ZIGBEE] starting");
    if (esp_zigbee_start(false) != ESP_OK) {
        ESP_LOGE(TAG, "esp_zigbee_start failed");
        vTaskDelete(nullptr);
        return;
    }
    if (activeTransport != nullptr) {
        activeTransport->markStackStarted();
    }

    esp_zigbee_launch_mainloop();
    vTaskDelete(nullptr);
}
}  // namespace

ZigbeeTransport::ZigbeeTransport()
    : started(false) {}

void ZigbeeTransport::markStackStarted() {
    started = true;
    stackStarted = true;
}

bool ZigbeeTransport::begin() {
    Serial.println("[ZIGBEE] initializing");
    if (activeTransport != nullptr) {
        ESP_LOGE(TAG, "Only one ZigbeeTransport instance is supported");
        return false;
    }

    const esp_err_t nvsResult = nvs_flash_init();
    if (nvsResult != ESP_OK) {
        ESP_LOGE(TAG, "NVS init failed (%s); no automatic erase performed",
                 esp_err_to_name(nvsResult));
        return false;
    }

    const esp_err_t zigbeeNvsResult =
        nvs_flash_init_partition(ZIGBEE_STORAGE_PARTITION);
    if (zigbeeNvsResult != ESP_OK) {
        ESP_LOGE(TAG,
                 "Zigbee storage partition init failed (%s); no automatic erase performed",
                 esp_err_to_name(zigbeeNvsResult));
        return false;
    }

    incomingMessages = xQueueCreate(RX_QUEUE_LENGTH, sizeof(Message));
    if (incomingMessages == nullptr) {
        ESP_LOGE(TAG, "Unable to allocate receive queue");
        return false;
    }

    memset(peerRoutes, 0, sizeof(peerRoutes));
    activeNetwork = false;
    stackStarted = false;
    activeTransport = this;
    if (xTaskCreate(zigbeeTask, "zigbee", 8192, nullptr, 5, nullptr) != pdPASS) {
        activeTransport = nullptr;
        vQueueDelete(incomingMessages);
        incomingMessages = nullptr;
        ESP_LOGE(TAG, "Unable to create Zigbee task");
        return false;
    }
    return true;
}

bool ZigbeeTransport::send(const Message& message) {
    if (!isReady()) {
        return false;
    }

    uint8_t asdu[ZIGBEE_MESSAGE_MAX_ENCODED_SIZE] = {};
    size_t asduLength = 0;
    if (!encodeZigbeeMessage(message, asdu, sizeof(asdu), asduLength)) {
        ESP_LOGE(TAG, "Message could not be encoded");
        return false;
    }

    ezb_apsde_data_req_t request = {};
    const bool broadcast = isBroadcast(message);
    if (broadcast) {
        ezb_address_set_short(&request.dst_address,
                              BROADCAST_RX_ON_WHEN_IDLE);
    } else {
        uint16_t destination = 0;
        if (!findRoute(message, destination)) {
            ESP_LOGW(TAG, "No Zigbee route for logical destination %lu",
                     static_cast<unsigned long>(message.destinationId));
            return false;
        }
        ezb_address_set_short(&request.dst_address, destination);
    }
    request.src_endpoint = APP_ENDPOINT;
    request.dst_endpoint = APP_ENDPOINT;
    request.cluster_id = APP_CLUSTER_ID;
    request.profile_id = APP_PROFILE_ID;
    request.radius = 10;
    request.tx_options = EZB_APSDE_TX_OPT_FRAG_PERMITTED;
    if (!broadcast) {
        request.tx_options |= EZB_APSDE_TX_OPT_ACK_TX;
    }
    request.asdu_length = static_cast<uint16_t>(asduLength);
    request.asdu = asdu;

    esp_zigbee_lock_acquire(portMAX_DELAY);
    const ezb_err_t result = ezb_apsde_data_request(&request);
    esp_zigbee_lock_release();
    if (result != EZB_ERR_NONE) {
        ESP_LOGW(TAG, "APS request rejected (error 0x%04x)", result);
        return false;
    }
    return true;
}

bool ZigbeeTransport::receive(Message& message) {
    return incomingMessages != nullptr &&
           xQueueReceive(incomingMessages, &message, 0) == pdTRUE;
}

bool ZigbeeTransport::isReady() const {
    return started && stackStarted && activeNetwork;
}

const char* ZigbeeTransport::name() const {
    return "ESP-ZIGBEE-APS";
}

#else

ZigbeeTransport::ZigbeeTransport()
    : started(false) {}

bool ZigbeeTransport::begin() {
    Serial.println(
        "[ZIGBEE] unavailable: build the ESP32-C6 ESP-IDF environment"
    );
    return false;
}

bool ZigbeeTransport::send(const Message&) {
    return false;
}

bool ZigbeeTransport::receive(Message&) {
    return false;
}

bool ZigbeeTransport::isReady() const {
    return false;
}

void ZigbeeTransport::markStackStarted() {}

const char* ZigbeeTransport::name() const {
    return "ESP-ZIGBEE-UNAVAILABLE";
}

#endif
