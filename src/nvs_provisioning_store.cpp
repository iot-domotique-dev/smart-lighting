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
constexpr char MAIN_NVS_NAMESPACE[] = "main_pairing";
constexpr char MAIN_RESERVATIONS_KEY[] = "reservations";
constexpr char LAMP_REMOVAL_NAMESPACE[] = "lamp_removal";
constexpr char LAMP_REMOVAL_KEY[] = "receipt";

bool validRemovalReceipt(const LampRemovalReceipt& receipt) {
    return receipt.version == LAMP_REMOVAL_RECEIPT_VERSION &&
        receipt.reserved == 0 && receipt.operationId != 0 &&
        receipt.deviceId != 0 && receipt.parentMainId != 0 &&
        receipt.hardwareId[0] != '\0' &&
        memchr(receipt.hardwareId, '\0', sizeof(receipt.hardwareId)) != nullptr &&
        (receipt.stage == LampRemovalReceiptStage::PREPARED ||
         receipt.stage == LampRemovalReceiptStage::CLEARED);
}

bool getString(nvs_handle_t handle, const char* key, char* output, size_t capacity) {
    size_t required = 0;
    if (nvs_get_str(handle, key, nullptr, &required) != ESP_OK ||
        required == 0 || required > capacity) {
        return false;
    }
    return nvs_get_str(handle, key, output, &required) == ESP_OK;
}

bool saveReservationBlob(const MainProvisioningReservationState& state) {
    if (!isValidMainProvisioningReservationState(state, state.mainId))
        return false;
    nvs_handle_t handle;
    if (nvs_open(MAIN_NVS_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK)
        return false;
    const esp_err_t result = nvs_set_blob(
            handle, MAIN_RESERVATIONS_KEY, &state, sizeof(state)) == ESP_OK
        ? nvs_commit(handle)
        : ESP_FAIL;
    nvs_close(handle);
    return result == ESP_OK;
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

bool NvsProvisioningStore::loadRemovalReceipt(
    LampRemovalReceipt& receipt
) const {
    receipt = {};
    nvs_handle_t handle;
    if (nvs_open(LAMP_REMOVAL_NAMESPACE, NVS_READONLY, &handle) != ESP_OK)
        return false;
    size_t required = 0;
    const bool validSize = nvs_get_blob(handle, LAMP_REMOVAL_KEY, nullptr,
                                        &required) == ESP_OK &&
                           required == sizeof(receipt);
    const esp_err_t result = validSize
        ? nvs_get_blob(handle, LAMP_REMOVAL_KEY, &receipt, &required)
        : ESP_FAIL;
    nvs_close(handle);
    if (result != ESP_OK || !validRemovalReceipt(receipt)) {
        receipt = {};
        return false;
    }
    return true;
}

bool NvsProvisioningStore::saveRemovalReceipt(
    const LampRemovalReceipt& receipt
) {
    if (!validRemovalReceipt(receipt)) return false;
    nvs_handle_t handle;
    if (nvs_open(LAMP_REMOVAL_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK)
        return false;
    const esp_err_t result = nvs_set_blob(handle, LAMP_REMOVAL_KEY,
                                          &receipt, sizeof(receipt)) == ESP_OK
        ? nvs_commit(handle) : ESP_FAIL;
    nvs_close(handle);
    return result == ESP_OK;
}

bool NvsProvisioningStore::clearRemovalReceipt() {
    nvs_handle_t handle;
    const esp_err_t opened = nvs_open(LAMP_REMOVAL_NAMESPACE,
                                      NVS_READWRITE, &handle);
    if (opened == ESP_ERR_NVS_NOT_FOUND) return true;
    if (opened != ESP_OK) return false;
    const esp_err_t erased = nvs_erase_key(handle, LAMP_REMOVAL_KEY);
    const esp_err_t result = erased == ESP_ERR_NVS_NOT_FOUND
        ? ESP_OK : (erased == ESP_OK ? nvs_commit(handle) : erased);
    nvs_close(handle);
    return result == ESP_OK;
}

bool NvsProvisioningReservationStore::load(
    uint32_t mainId,
    MainProvisioningReservationState& state
) const {
    initializeMainProvisioningReservationState(mainId, state);
    if (mainId == 0) {
        return false;
    }

    nvs_handle_t handle;
    const esp_err_t openResult =
        nvs_open(MAIN_NVS_NAMESPACE, NVS_READONLY, &handle);
    if (openResult == ESP_ERR_NVS_NOT_FOUND) {
        return true;
    }
    if (openResult != ESP_OK) {
        return false;
    }

    size_t required = 0;
    const esp_err_t sizeResult = nvs_get_blob(
        handle, MAIN_RESERVATIONS_KEY, nullptr, &required);
    if (sizeResult != ESP_OK) {
        nvs_close(handle);
        return false;
    }
    if (required == sizeof(state)) {
        const esp_err_t readResult = nvs_get_blob(
            handle, MAIN_RESERVATIONS_KEY, &state, &required);
        nvs_close(handle);
        return readResult == ESP_OK &&
               isValidMainProvisioningReservationState(state, mainId);
    }

    MainProvisioningReservationStateV1 legacy = {};
    if (required != sizeof(legacy)) {
        nvs_close(handle);
        return false;
    }
    const esp_err_t readResult = nvs_get_blob(
        handle, MAIN_RESERVATIONS_KEY, &legacy, &required);
    nvs_close(handle);
    MainProvisioningReservationState migrated = {};
    if (readResult != ESP_OK ||
        !migrateMainProvisioningReservationStateV1(legacy, mainId, migrated) ||
        !saveReservationBlob(migrated)) return false;
    state = migrated;
    return true;
}

bool NvsProvisioningReservationStore::save(
    const MainProvisioningReservationState& state
) {
    return saveReservationBlob(state);
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

bool NvsProvisioningStore::loadRemovalReceipt(LampRemovalReceipt& receipt) const {
    memset(&receipt, 0, sizeof(receipt));
    return false;
}

bool NvsProvisioningStore::saveRemovalReceipt(const LampRemovalReceipt&) {
    return false;
}

bool NvsProvisioningStore::clearRemovalReceipt() {
    return false;
}

bool NvsProvisioningReservationStore::load(
    uint32_t,
    MainProvisioningReservationState& state
) const {
    initializeMainProvisioningReservationState(0, state);
    return false;
}

bool NvsProvisioningReservationStore::save(
    const MainProvisioningReservationState&
) {
    return false;
}

#endif
