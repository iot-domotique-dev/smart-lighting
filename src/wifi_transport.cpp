#include <string.h>

#include "wifi_transport.h"

#if defined(SMART_LIGHTING_CORE_WIFI)

#include <Arduino.h>

#include "esp_event.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "esp_wifi.h"

namespace {
volatile bool stationHasAddress = false;
bool networkObjectsCreated = false;

void onWifiEvent(void*, esp_event_base_t eventBase, int32_t eventId, void*) {
    if (eventBase == WIFI_EVENT && eventId == WIFI_EVENT_STA_DISCONNECTED) {
        stationHasAddress = false;
    }
    if (eventBase == IP_EVENT && eventId == IP_EVENT_STA_GOT_IP) {
        stationHasAddress = true;
    }
}
}  // namespace

WiFiTransport::WiFiTransport() : started(false) {}

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
    if (result != ESP_OK && result != ESP_ERR_INVALID_STATE) return false;

    result = esp_event_loop_create_default();
    if (result != ESP_OK && result != ESP_ERR_INVALID_STATE) return false;

    if (!networkObjectsCreated) {
        if (esp_netif_create_default_wifi_sta() == nullptr) return false;
        wifi_init_config_t config = WIFI_INIT_CONFIG_DEFAULT();
        result = esp_wifi_init(&config);
        if (result != ESP_OK && result != ESP_ERR_INVALID_STATE) return false;

        result = esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                            onWifiEvent, nullptr);
        if (result != ESP_OK) return false;
        result = esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                            onWifiEvent, nullptr);
        if (result != ESP_OK) return false;
        networkObjectsCreated = true;
    }

    result = esp_wifi_set_mode(WIFI_MODE_STA);
    if (result != ESP_OK) return false;
    result = esp_wifi_start();
    if (result != ESP_OK && result != ESP_ERR_WIFI_NOT_STOPPED) return false;

    started = true;
    Serial.println("[CORE-WIFI] station ready; credentials not configured");
    return true;
}

bool WiFiTransport::connect(const char* ssid, const char* password) {
    if (!started || ssid == nullptr || password == nullptr) return false;
    wifi_config_t config = {};
    const size_t ssidLength = strlen(ssid);
    const size_t passwordLength = strlen(password);
    if (ssidLength == 0 || ssidLength >= sizeof(config.sta.ssid) ||
        passwordLength >= sizeof(config.sta.password)) {
        return false;
    }

    memcpy(config.sta.ssid, ssid, ssidLength);
    memcpy(config.sta.password, password, passwordLength);
    config.sta.threshold.authmode = WIFI_AUTH_OPEN;
    if (esp_wifi_set_config(WIFI_IF_STA, &config) != ESP_OK) return false;
    stationHasAddress = false;
    return esp_wifi_connect() == ESP_OK;
}

bool WiFiTransport::isReady() const {
    return started && stationHasAddress;
}

const char* WiFiTransport::name() const {
    return "WIFI_STATION";
}

#else

WiFiTransport::WiFiTransport() : started(false) {}
bool WiFiTransport::begin() { return false; }
bool WiFiTransport::connect(const char*, const char*) { return false; }
bool WiFiTransport::isReady() const { return false; }
const char* WiFiTransport::name() const { return "WIFI_UNAVAILABLE"; }

#endif
