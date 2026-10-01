#include <string.h>

#include "nvs_provisioning_store.h"

#if defined(SMART_LIGHTING_ZIGBEE)
#include "esp_err.h"
#include "nvs.h"

namespace {
constexpr char NVS_NAMESPACE[] = "lamp_pairing";
constexpr uint8_t RECORD_VERSION = 1;
constexpr char KEY_VERSION[] = "version";
constexpr char KEY_DEVICE_ID[] = "device_id";
constexpr char KEY_HARDWARE_ID[] = "hardware_id";
constexpr char KEY_NAME[] = "name";
constexpr char KEY_PARENT_MAIN_ID[] = "parent_main_id";
constexpr char KEY_PAIRING_STATE[] = "pairing_state";

bool getString(nvs_handle_t handle, const char* key, char* output, size_t capacity) {
    size_t required = 0;
    if (nvs_get_str(handle, key, nullptr, &required) != ESP_OK ||
        required == 0 || required > capacity) {
        return false;
    }
    return nvs_get_str(handle, key, output, &required) == ESP_OK;
}
}  // namespace

bool NvsProvisioningStore::load(ProvisioningRecord& record) const {
    record = {};
    nvs_handle_t handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) {
        return false;
    }

    uint8_t version = 0;
    uint8_t pairingState = 0;
    const bool valid =
        nvs_get_u8(handle, KEY_VERSION, &version) == ESP_OK &&
        version == RECORD_VERSION &&
        nvs_get_u32(handle, KEY_DEVICE_ID, &record.deviceId) == ESP_OK &&
        getString(handle, KEY_HARDWARE_ID, record.hardwareId,
                  sizeof(record.hardwareId)) &&
        getString(handle, KEY_NAME, record.name, sizeof(record.name)) &&
        nvs_get_u32(handle, KEY_PARENT_MAIN_ID, &record.parentMainId) == ESP_OK &&
        nvs_get_u8(handle, KEY_PAIRING_STATE, &pairingState) == ESP_OK;
    nvs_close(handle);

    if (!valid || record.deviceId == 0 || record.parentMainId == 0 ||
        pairingState != static_cast<uint8_t>(PairingState::PAIRED) ||
        record.hardwareId[0] == '\0') {
        record = {};
        return false;
    }
    record.pairingState = static_cast<PairingState>(pairingState);
    return true;
}

bool NvsProvisioningStore::save(const ProvisioningRecord& record) {
    if (record.deviceId == 0 || record.parentMainId == 0 ||
        record.hardwareId[0] == '\0' || record.pairingState != PairingState::PAIRED ||
        record.hardwareId[sizeof(record.hardwareId) - 1] != '\0' ||
        record.name[sizeof(record.name) - 1] != '\0') {
        return false;
    }

    nvs_handle_t handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK) {
        return false;
    }
    const esp_err_t result =
        nvs_set_u8(handle, KEY_VERSION, RECORD_VERSION) == ESP_OK &&
        nvs_set_u32(handle, KEY_DEVICE_ID, record.deviceId) == ESP_OK &&
        nvs_set_str(handle, KEY_HARDWARE_ID, record.hardwareId) == ESP_OK &&
        nvs_set_str(handle, KEY_NAME, record.name) == ESP_OK &&
        nvs_set_u32(handle, KEY_PARENT_MAIN_ID, record.parentMainId) == ESP_OK &&
        nvs_set_u8(handle, KEY_PAIRING_STATE,
                   static_cast<uint8_t>(record.pairingState)) == ESP_OK
            ? nvs_commit(handle)
            : ESP_FAIL;
    nvs_close(handle);
    return result == ESP_OK;
}

bool NvsProvisioningStore::clear() {
    nvs_handle_t handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK) {
        return true;
    }
    const esp_err_t eraseResult = nvs_erase_all(handle);
    const esp_err_t commitResult = eraseResult == ESP_OK ? nvs_commit(handle) : eraseResult;
    nvs_close(handle);
    return commitResult == ESP_OK;
}

#else

bool NvsProvisioningStore::load(ProvisioningRecord& record) const {
    record = {};
    return false;
}

bool NvsProvisioningStore::save(const ProvisioningRecord&) {
    return false;
}

bool NvsProvisioningStore::clear() {
    return true;
}

#endif
