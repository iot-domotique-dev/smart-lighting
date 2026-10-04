#include <string.h>

#include "wifi_transport.h"

#if defined(SMART_LIGHTING_CORE_WIFI)

#include <Arduino.h>

#include "esp_event.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "esp_wifi.h"

namespace {
constexpr uint32_t WIFI_RECONNECT_INTERVAL_MS = 5000;

volatile bool stationHasAddress = false;
volatile bool stationDisconnectedEvent = false;
volatile bool stationGotIpEvent = false;
volatile uint8_t stationDisconnectReason = 0;
bool networkObjectsCreated = false;
esp_netif_t* stationNetif = nullptr;

bool deadlineReached(uint32_t now, uint32_t deadline) {
    return static_cast<int32_t>(now - deadline) >= 0;
}

void onWifiEvent(void*, esp_event_base_t eventBase, int32_t eventId,
                 void* eventData) {
    if (eventBase == WIFI_EVENT && eventId == WIFI_EVENT_STA_DISCONNECTED) {
        stationHasAddress = false;
        stationDisconnectReason = eventData == nullptr
            ? 0
            : static_cast<wifi_event_sta_disconnected_t*>(eventData)->reason;
        stationDisconnectedEvent = true;
    }
    if (eventBase == IP_EVENT && eventId == IP_EVENT_STA_GOT_IP) {
        stationHasAddress = true;
        stationGotIpEvent = true;
    }
}
}  // namespace

WiFiTransport::WiFiTransport()
    : started(false), credentialsConfigured(false), reconnectPending(false),
      reconnectAtMs(0), configuredSsid{} {}

bool WiFiTransport::begin() {
    if (started) return true;

    // ESP-IDF's Wi-Fi driver reads its configuration from NVS. Initialize
    // the default NVS partition before starting the network interfaces.
    esp_err_t result = nvs_flash_init();
    if (result != ESP_OK && result != ESP_ERR_INVALID_STATE) {
        Serial.print("[CORE-WIFI] NVS init failed: ");
        Serial.println(esp_err_to_name(result));
        if (result == ESP_ERR_NVS_NO_FREE_PAGES ||
            result == ESP_ERR_NVS_NEW_VERSION_FOUND) {
            Serial.println("[CORE-WIFI] NVS needs recovery; refusing to erase stored data automatically");
        }
        return false;
    }

    result = esp_netif_init();
    if (result != ESP_OK && result != ESP_ERR_INVALID_STATE) {
        Serial.print("[CORE-WIFI] network interface init failed: ");
        Serial.println(esp_err_to_name(result));
        return false;
    }

    result = esp_event_loop_create_default();
    if (result != ESP_OK && result != ESP_ERR_INVALID_STATE) {
        Serial.print("[CORE-WIFI] event loop init failed: ");
        Serial.println(esp_err_to_name(result));
        return false;
    }

    if (!networkObjectsCreated) {
        stationNetif = esp_netif_create_default_wifi_sta();
        if (stationNetif == nullptr) {
            Serial.println("[CORE-WIFI] station network interface creation failed");
            return false;
        }
        wifi_init_config_t config = WIFI_INIT_CONFIG_DEFAULT();
        result = esp_wifi_init(&config);
        if (result != ESP_OK && result != ESP_ERR_INVALID_STATE) {
            Serial.print("[CORE-WIFI] driver init failed: ");
            Serial.println(esp_err_to_name(result));
            return false;
        }

        result = esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                            onWifiEvent, nullptr);
        if (result != ESP_OK) {
            Serial.print("[CORE-WIFI] Wi-Fi event registration failed: ");
            Serial.println(esp_err_to_name(result));
            return false;
        }
        result = esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                            onWifiEvent, nullptr);
        if (result != ESP_OK) {
            Serial.print("[CORE-WIFI] IP event registration failed: ");
            Serial.println(esp_err_to_name(result));
            return false;
        }
        networkObjectsCreated = true;
    }

    result = esp_wifi_set_mode(WIFI_MODE_STA);
    if (result != ESP_OK) {
        Serial.print("[CORE-WIFI] station mode setup failed: ");
        Serial.println(esp_err_to_name(result));
        return false;
    }
    result = esp_wifi_start();
    if (result != ESP_OK && result != ESP_ERR_WIFI_NOT_STOPPED) {
        Serial.print("[CORE-WIFI] driver start failed: ");
        Serial.println(esp_err_to_name(result));
        return false;
    }

    started = true;
    stationHasAddress = false;
    Serial.println("[CORE-WIFI] Wi-Fi station driver started");
    return true;
}

bool WiFiTransport::connect(const char* ssid, const char* password) {
    if (!started || ssid == nullptr || password == nullptr) return false;
    wifi_config_t config = {};
    const size_t ssidLength = strlen(ssid);
    const size_t passwordLength = strlen(password);
    if (ssidLength == 0 || ssidLength > sizeof(config.sta.ssid) ||
        passwordLength >= sizeof(config.sta.password)) {
        Serial.println("[CORE-WIFI] Wi-Fi credentials are invalid; check the local configuration");
        return false;
    }

    memcpy(config.sta.ssid, ssid, ssidLength);
    memcpy(config.sta.password, password, passwordLength);
    config.sta.threshold.authmode = WIFI_AUTH_OPEN;

    const esp_err_t result = esp_wifi_set_config(WIFI_IF_STA, &config);
    if (result != ESP_OK) {
        Serial.print("[CORE-WIFI] Wi-Fi configuration rejected: ");
        Serial.println(esp_err_to_name(result));
        return false;
    }

    memcpy(configuredSsid, ssid, ssidLength);
    configuredSsid[ssidLength] = '\0';
    credentialsConfigured = true;
    stationHasAddress = false;
    return requestConnection(false);
}

bool WiFiTransport::requestConnection(bool reconnect) {
    if (!started || !credentialsConfigured) return false;

    const esp_err_t result = esp_wifi_connect();
    reconnectAtMs = millis() + WIFI_RECONNECT_INTERVAL_MS;
    reconnectPending = true;
    if (result != ESP_OK) {
        Serial.print("[CORE-WIFI] Wi-Fi connection request failed: ");
        Serial.println(esp_err_to_name(result));
        return false;
    }

    Serial.print(reconnect ? "[CORE-WIFI] Wi-Fi reconnect attempt, SSID: "
                           : "[CORE-WIFI] Wi-Fi connection attempt, SSID: ");
    Serial.println(configuredSsid);
    return true;
}

void WiFiTransport::poll() {
    if (stationGotIpEvent) {
        stationGotIpEvent = false;
        reconnectPending = false;
        Serial.print("[CORE-WIFI] Wi-Fi connected, SSID: ");
        Serial.println(configuredSsid);

        esp_netif_ip_info_t ipInfo = {};
        if (stationNetif != nullptr &&
            esp_netif_get_ip_info(stationNetif, &ipInfo) == ESP_OK) {
            Serial.print("[CORE-WIFI] IP address: ");
            Serial.print(esp_ip4_addr1(&ipInfo.ip));
            Serial.print('.');
            Serial.print(esp_ip4_addr2(&ipInfo.ip));
            Serial.print('.');
            Serial.print(esp_ip4_addr3(&ipInfo.ip));
            Serial.print('.');
            Serial.println(esp_ip4_addr4(&ipInfo.ip));
        } else {
            Serial.println("[CORE-WIFI] DHCP address is not available yet");
        }
    }

    if (stationDisconnectedEvent) {
        stationDisconnectedEvent = false;
        const uint8_t reason = stationDisconnectReason;
        Serial.print("[CORE-WIFI] Wi-Fi connection lost (reason=");
        Serial.print(static_cast<unsigned>(reason));
        Serial.println(')');
        if (credentialsConfigured) {
            reconnectAtMs = millis() + WIFI_RECONNECT_INTERVAL_MS;
            reconnectPending = true;
        }
    }

    if (started && credentialsConfigured && reconnectPending &&
        !stationHasAddress && deadlineReached(millis(), reconnectAtMs)) {
        (void)requestConnection(true);
    }
}

bool WiFiTransport::isReady() const {
    return started && stationHasAddress;
}

const char* WiFiTransport::name() const {
    return "WIFI_STATION";
}

#else

WiFiTransport::WiFiTransport()
    : started(false), credentialsConfigured(false), reconnectPending(false),
      reconnectAtMs(0), configuredSsid{} {}
bool WiFiTransport::begin() { return false; }
bool WiFiTransport::connect(const char*, const char*) { return false; }
void WiFiTransport::poll() {}
bool WiFiTransport::isReady() const { return false; }
const char* WiFiTransport::name() const { return "WIFI_UNAVAILABLE"; }

#endif
