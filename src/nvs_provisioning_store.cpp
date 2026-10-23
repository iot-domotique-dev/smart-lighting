#include <string.h>
#include <stddef.h>

#include "nvs_provisioning_store.h"

#if defined(SMART_LIGHTING_ZIGBEE)
#include "esp_err.h"
#include "nvs.h"

namespace {
constexpr char NVS_NAMESPACE[] = "lamp_pairing";
constexpr char LAMP_SETTINGS_NAMESPACE[] = "lamp_settings";
constexpr char LAMP_BRIGHTNESS_KEY[] = "last_nonzero_v1";
constexpr uint8_t RECORD_VERSION = 1;
constexpr char KEY_VERSION[] = "version";
constexpr char KEY_DEVICE_ID[] = "device_id";
constexpr char KEY_HARDWARE_ID[] = "hardware_id";
constexpr char KEY_NAME[] = "name";
constexpr char KEY_PARENT_MAIN_ID[] = "parent_main_id";
constexpr char KEY_PAIRING_STATE[] = "pairing_state";
constexpr char MAIN_NVS_NAMESPACE[] = "main_pairing";
constexpr char MAIN_RESERVATIONS_KEY[] = "reservations";
constexpr char MAIN_GROUPS_NAMESPACE[] = "main_groups";
constexpr char MAIN_GROUPS_SLOT_A[] = "groups_slot_a";
constexpr char MAIN_GROUPS_SLOT_B[] = "groups_slot_b";
constexpr uint32_t MAIN_GROUPS_MAGIC = 0x534C4752UL;  // "SLGR"
constexpr uint16_t MAIN_GROUPS_FORMAT_VERSION = 1;
constexpr char LAMP_REMOVAL_NAMESPACE[] = "lamp_removal";
constexpr char LAMP_REMOVAL_KEY[] = "receipt";

struct MainGroupsRecord {
    uint32_t magic;
    uint16_t version;
    uint16_t size;
    uint32_t generation;
    uint32_t mainId;
    uint8_t count;
    uint8_t reserved[3];
    LampGroup groups[MAX_GROUPS];
    uint32_t checksum;
};

static_assert(sizeof(MainGroupsRecord) < 1200,
              "MAIN group snapshot must remain compact for NVS");

enum class GroupSlotState : uint8_t { MISSING, VALID, CORRUPT, UNSUPPORTED, ERROR };

struct GroupSlot {
    GroupSlotState state;
    uint32_t generation;
    uint32_t mainId;
};

uint32_t groupRecordChecksum(const MainGroupsRecord& record) {
    const uint8_t* bytes = reinterpret_cast<const uint8_t*>(&record);
    uint32_t crc = 0xffffffffUL;
    for (size_t index = 0; index < offsetof(MainGroupsRecord, checksum); ++index) {
        crc ^= bytes[index];
        for (uint8_t bit = 0; bit < 8; ++bit) {
            crc = (crc >> 1) ^ (0xedb88320UL &
                static_cast<uint32_t>(-static_cast<int32_t>(crc & 1U)));
        }
    }
    return ~crc;
}

bool groupRecordNewer(uint32_t left, uint32_t right) {
    const uint32_t difference = left - right;
    return difference != 0 && difference < 0x80000000UL;
}

GroupSlot readGroupRecord(nvs_handle_t handle, const char* key,
                          MainGroupsRecord& record) {
    GroupSlot slot = {};
    slot.state = GroupSlotState::MISSING;
    size_t required = 0;
    const esp_err_t lengthResult = nvs_get_blob(handle, key, nullptr, &required);
    if (lengthResult == ESP_ERR_NVS_NOT_FOUND) return slot;
    if (lengthResult != ESP_OK) {
        slot.state = GroupSlotState::ERROR;
        return slot;
    }
    if (required != sizeof(MainGroupsRecord)) {
        slot.state = required >= sizeof(uint32_t) + sizeof(uint16_t)
            ? GroupSlotState::UNSUPPORTED : GroupSlotState::CORRUPT;
        return slot;
    }
    size_t size = sizeof(record);
    if (nvs_get_blob(handle, key, &record, &size) != ESP_OK ||
        size != sizeof(record)) {
        slot.state = GroupSlotState::ERROR;
        return slot;
    }
    if (record.magic != MAIN_GROUPS_MAGIC) {
        slot.state = GroupSlotState::CORRUPT;
        return slot;
    }
    if (record.version != MAIN_GROUPS_FORMAT_VERSION ||
        record.size != sizeof(MainGroupsRecord)) {
        slot.state = GroupSlotState::UNSUPPORTED;
        return slot;
    }
    if (record.generation == 0 || record.mainId == 0 ||
        record.count > MAX_GROUPS || record.reserved[0] != 0 ||
        record.reserved[1] != 0 || record.reserved[2] != 0 ||
        record.checksum != groupRecordChecksum(record)) {
        slot.state = GroupSlotState::CORRUPT;
        return slot;
    }
    GroupRegistry candidate = {};
    candidate.mainId = record.mainId;
    candidate.count = record.count;
    candidate.persistenceReady = true;
    memcpy(candidate.groups, record.groups, sizeof(candidate.groups));
    if (!validateGroupRegistry(candidate, candidate.mainId)) {
        slot.state = GroupSlotState::CORRUPT;
        return slot;
    }
    slot.state = GroupSlotState::VALID;
    slot.generation = record.generation;
    slot.mainId = record.mainId;
    return slot;
}

GroupSlot readGroupSlot(nvs_handle_t handle, const char* key) {
    MainGroupsRecord record = {};
    return readGroupRecord(handle, key, record);
}

bool buildGroupRecord(const GroupRegistry& registry, uint32_t generation,
                      MainGroupsRecord& record) {
    if (generation == 0 || registry.mainId == 0 ||
        !validateGroupRegistry(registry, registry.mainId)) return false;
    memset(&record, 0, sizeof(record));
    record.magic = MAIN_GROUPS_MAGIC;
    record.version = MAIN_GROUPS_FORMAT_VERSION;
    record.size = static_cast<uint16_t>(sizeof(MainGroupsRecord));
    record.generation = generation;
    record.mainId = registry.mainId;
    record.count = registry.count;
    memcpy(record.groups, registry.groups, sizeof(record.groups));
    record.checksum = groupRecordChecksum(record);
    return true;
}

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

LampBrightnessLoadResult NvsProvisioningStore::loadLastNonzeroBrightness(
    uint8_t& brightness
) const {
    brightness = 0;
    nvs_handle_t handle;
    const esp_err_t openResult =
        nvs_open(LAMP_SETTINGS_NAMESPACE, NVS_READONLY, &handle);
    if (openResult == ESP_ERR_NVS_NOT_FOUND)
        return LampBrightnessLoadResult::NOT_FOUND;
    if (openResult != ESP_OK) return LampBrightnessLoadResult::ERROR;

    uint8_t stored = 0;
    const esp_err_t readResult =
        nvs_get_u8(handle, LAMP_BRIGHTNESS_KEY, &stored);
    nvs_close(handle);
    if (readResult == ESP_ERR_NVS_NOT_FOUND)
        return LampBrightnessLoadResult::NOT_FOUND;
    if (readResult == ESP_ERR_NVS_TYPE_MISMATCH)
        return LampBrightnessLoadResult::INVALID;
    if (readResult != ESP_OK) return LampBrightnessLoadResult::ERROR;
    if (stored < 1 || stored > 100)
        return LampBrightnessLoadResult::INVALID;

    brightness = stored;
    return LampBrightnessLoadResult::FOUND;
}

bool NvsProvisioningStore::saveLastNonzeroBrightness(uint8_t brightness) {
    if (brightness < 1 || brightness > 100) return false;

    nvs_handle_t handle;
    if (nvs_open(LAMP_SETTINGS_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK)
        return false;
    const esp_err_t result = nvs_set_u8(
        handle, LAMP_BRIGHTNESS_KEY, brightness) == ESP_OK
        ? nvs_commit(handle)
        : ESP_FAIL;
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

GroupStoreLoadResult NvsMainGroupStore::load(
    uint32_t expectedMainId,
    GroupRegistry& registry
) const {
    if (expectedMainId == 0) return GroupStoreLoadResult::ERROR;
    nvs_handle_t handle;
    const esp_err_t openResult = nvs_open(
        MAIN_GROUPS_NAMESPACE, NVS_READONLY, &handle);
    if (openResult == ESP_ERR_NVS_NOT_FOUND) return GroupStoreLoadResult::NOT_FOUND;
    if (openResult != ESP_OK) return GroupStoreLoadResult::ERROR;

    const GroupSlot slotA = readGroupSlot(handle, MAIN_GROUPS_SLOT_A);
    const GroupSlot slotB = readGroupSlot(handle, MAIN_GROUPS_SLOT_B);

    if (slotA.state == GroupSlotState::UNSUPPORTED ||
        slotB.state == GroupSlotState::UNSUPPORTED) {
        nvs_close(handle);
        return GroupStoreLoadResult::UNSUPPORTED_VERSION;
    }
    if (slotA.state == GroupSlotState::ERROR ||
        slotB.state == GroupSlotState::ERROR) {
        nvs_close(handle);
        return GroupStoreLoadResult::ERROR;
    }
    if (slotA.state == GroupSlotState::MISSING &&
        slotB.state == GroupSlotState::MISSING) {
        nvs_close(handle);
        return GroupStoreLoadResult::NOT_FOUND;
    }

    const bool validA = slotA.state == GroupSlotState::VALID;
    const bool validB = slotB.state == GroupSlotState::VALID;
    if (!validA && !validB) {
        nvs_close(handle);
        return GroupStoreLoadResult::CORRUPT;
    }
    const bool selectedIsA = validA && (!validB ||
        !groupRecordNewer(slotB.generation, slotA.generation));
    const char* selectedKey = selectedIsA
        ? MAIN_GROUPS_SLOT_A : MAIN_GROUPS_SLOT_B;
    MainGroupsRecord selected = {};
    const GroupSlot selectedStatus = readGroupRecord(handle, selectedKey, selected);
    nvs_close(handle);
    if (selectedStatus.state != GroupSlotState::VALID) {
        if (selectedStatus.state == GroupSlotState::UNSUPPORTED)
            return GroupStoreLoadResult::UNSUPPORTED_VERSION;
        return selectedStatus.state == GroupSlotState::ERROR
            ? GroupStoreLoadResult::ERROR : GroupStoreLoadResult::CORRUPT;
    }
    if (selected.mainId != expectedMainId) return GroupStoreLoadResult::CORRUPT;

    registry = {};
    registry.mainId = selected.mainId;
    registry.count = selected.count;
    registry.persistenceReady = true;
    memcpy(registry.groups, selected.groups, sizeof(registry.groups));
    return GroupStoreLoadResult::FOUND;
}

bool NvsMainGroupStore::save(const GroupRegistry& registry) {
    if (registry.mainId == 0 ||
        !validateGroupRegistry(registry, registry.mainId)) return false;
    nvs_handle_t handle;
    if (nvs_open(MAIN_GROUPS_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK)
        return false;

    const GroupSlot slotA = readGroupSlot(handle, MAIN_GROUPS_SLOT_A);
    const GroupSlot slotB = readGroupSlot(handle, MAIN_GROUPS_SLOT_B);
    if (slotA.state == GroupSlotState::UNSUPPORTED ||
        slotB.state == GroupSlotState::UNSUPPORTED ||
        slotA.state == GroupSlotState::ERROR ||
        slotB.state == GroupSlotState::ERROR) {
        nvs_close(handle);
        return false;
    }

    const bool validA = slotA.state == GroupSlotState::VALID;
    const bool validB = slotB.state == GroupSlotState::VALID;
    if (!validA && !validB &&
        (slotA.state == GroupSlotState::CORRUPT ||
         slotB.state == GroupSlotState::CORRUPT)) {
        nvs_close(handle);
        return false;
    }

    uint32_t generation = 1;
    const char* targetKey = MAIN_GROUPS_SLOT_A;
    if (validA || validB) {
        if ((validA && slotA.mainId != registry.mainId) ||
            (validB && slotB.mainId != registry.mainId)) {
            nvs_close(handle);
            return false;
        }
        const bool newestIsA = validA && (!validB ||
            !groupRecordNewer(slotB.generation, slotA.generation));
        const GroupSlot& newest = newestIsA ? slotA : slotB;
        generation = newest.generation + 1;
        if (generation == 0) generation = 1;
        targetKey = newestIsA ? MAIN_GROUPS_SLOT_B : MAIN_GROUPS_SLOT_A;
    } else if (slotA.state == GroupSlotState::CORRUPT) {
        targetKey = MAIN_GROUPS_SLOT_A;
    } else if (slotB.state == GroupSlotState::CORRUPT) {
        targetKey = MAIN_GROUPS_SLOT_B;
    }

    MainGroupsRecord record = {};
    const bool built = buildGroupRecord(registry, generation, record);
    const esp_err_t writeResult = built
        ? nvs_set_blob(handle, targetKey, &record, sizeof(record))
        : ESP_FAIL;
    const esp_err_t commitResult = writeResult == ESP_OK
        ? nvs_commit(handle) : ESP_FAIL;
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

LampBrightnessLoadResult NvsProvisioningStore::loadLastNonzeroBrightness(
    uint8_t& brightness
) const {
    brightness = 0;
    return LampBrightnessLoadResult::ERROR;
}

bool NvsProvisioningStore::saveLastNonzeroBrightness(uint8_t) {
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

GroupStoreLoadResult NvsMainGroupStore::load(
    uint32_t,
    GroupRegistry& registry
) const {
    initGroupRegistry(registry);
    return GroupStoreLoadResult::ERROR;
}

bool NvsMainGroupStore::save(const GroupRegistry&) {
    return false;
}

#endif
