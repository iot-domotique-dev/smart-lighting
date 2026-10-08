#include <string.h>
#include <stdio.h>

#include <unity.h>

#include "core_api_service.h"
#include "core_module_service.h"
#include "communication.h"

FakeHardwareSerial Serial;

uint32_t millis() {
    return 1234;
}

bool receiveMessage(Communication&, Message&) {
    return false;
}

bool sendMessage(Communication&, Message) {
    return true;
}

namespace {
DeviceRegistry registry;
CoreApiRuntimeInfo runtime = {123456, "v7.1-test", false, true};
char response[CORE_API_MAX_RESPONSE_BYTES];

void addDevice(uint32_t id, const char* name, DeviceRole role,
               DeviceStatus status, uint32_t parentId, uint32_t localId,
               uint32_t capabilities = 0) {
    Device device = {};
    device.id = id;
    device.name = name;
    device.role = role;
    device.status = status;
    device.lastSeen = 7654;
    device.capabilities = capabilities;
    device.parentId = parentId;
    device.localId = localId;
    TEST_ASSERT_TRUE(registerDevice(registry, device));
}

void initRoot() {
    initDeviceRegistry(registry);
    addDevice(CORE_LOGICAL_ID, "CORE", DeviceRole::CORE,
              DeviceStatus::ONLINE, 0, CORE_LOGICAL_ID);
}

uint16_t request(const char* method, const char* uri, size_t& length) {
    memset(response, 0, sizeof(response));
    return handleCoreApiRequest(registry, runtime, method, uri, response,
                                sizeof(response), length);
}

void assertContains(const char* value) {
    TEST_ASSERT_NOT_NULL(strstr(response, value));
}
}  // namespace

void setUp() {
    initRoot();
    runtime = {123456, "v7.1-test", false, true};
}

void tearDown() {}

void test_health_response_reports_root_runtime_and_transport_state() {
    size_t length = 0;
    TEST_ASSERT_EQUAL_UINT16(200, request("GET", "/api/v1/health", length));
    TEST_ASSERT_GREATER_THAN_UINT(0, length);
    assertContains("\"status\":\"ok\"");
    assertContains("\"core_id\":1");
    assertContains("\"firmware_version\":\"v7.1-test\"");
    assertContains("\"uptime_ms\":123456");
    assertContains("\"wifi_connected\":false");
    assertContains("\"uart_driver_ready\":true");
}

void test_core_endpoint_uses_public_fields_and_stable_root_id() {
    size_t length = 0;
    TEST_ASSERT_EQUAL_UINT16(200, request("GET", "/api/v1/core", length));
    assertContains("\"core\":{\"id\":1,\"name\":\"CORE\"");
    assertContains("\"role\":\"core\"");
    assertContains("\"api_version\":\"v1\"");
}

void test_modules_list_filters_by_main_role_and_preserves_ids() {
    const uint32_t mainLocalId = 0x12345678;
    const uint32_t mainId = makeCoreModuleId(CORE_LOGICAL_ID, mainLocalId);
    addDevice(mainId, "MAIN_LIGHTING", DeviceRole::MAIN,
              DeviceStatus::ONLINE, CORE_LOGICAL_ID, mainLocalId,
              DEVICE_CAP_LIGHTING | DEVICE_CAP_GROUPS);
    addDevice(makeCoreModuleId(mainId, 17), "LAMP_C6", DeviceRole::LAMP,
              DeviceStatus::OFFLINE, mainId, 17,
              DEVICE_CAP_POWER | DEVICE_CAP_BRIGHTNESS);

    size_t length = 0;
    TEST_ASSERT_EQUAL_UINT16(200, request("GET", "/api/v1/modules", length));
    char idField[40] = {};
    snprintf(idField, sizeof(idField), "\"id\":%lu",
             static_cast<unsigned long>(mainId));
    assertContains("\"modules\":[");
    assertContains(idField);
    assertContains("\"name\":\"MAIN_LIGHTING\"");
    assertContains("\"role\":\"main\"");
    assertContains("\"capabilities\":[\"lighting\",\"groups\"]");
    assertContains("\"count\":1");
    TEST_ASSERT_NULL(strstr(response, "LAMP_C6"));
}

void test_module_item_returns_known_main_and_unknown_module_is_404() {
    const uint32_t localId = 0x1234;
    const uint32_t coreId = makeCoreModuleId(CORE_LOGICAL_ID, localId);
    addDevice(coreId, "MAIN_LIGHTING", DeviceRole::MAIN,
              DeviceStatus::ONLINE, CORE_LOGICAL_ID, localId);

    char path[64] = {};
    snprintf(path, sizeof(path), "/api/v1/modules/%lu",
             static_cast<unsigned long>(coreId));
    size_t length = 0;
    TEST_ASSERT_EQUAL_UINT16(200, request("GET", path, length));
    assertContains("\"module\":{\"id\":");
    assertContains("\"state\":null");
    TEST_ASSERT_NULL(strstr(response, "\"last_confirmed_state\""));

    TEST_ASSERT_EQUAL_UINT16(404,
        request("GET", "/api/v1/modules/987654321", length));
    assertContains("\"code\":\"not_found\"");
}

void test_devices_list_and_known_device_report_connectivity_not_fake_domain_state() {
    const uint32_t mainLocalId = 0x1234;
    const uint32_t mainId = makeCoreModuleId(CORE_LOGICAL_ID, mainLocalId);
    const uint32_t lampId = makeCoreModuleId(mainId, 17);
    addDevice(mainId, "MAIN_LIGHTING", DeviceRole::MAIN,
              DeviceStatus::ONLINE, CORE_LOGICAL_ID, mainLocalId);
    addDevice(lampId, "LAMP_C6", DeviceRole::LAMP,
              DeviceStatus::OFFLINE, mainId, 17,
              DEVICE_CAP_POWER | DEVICE_CAP_BRIGHTNESS);

    size_t length = 0;
    TEST_ASSERT_EQUAL_UINT16(200, request("GET", "/api/v1/devices", length));
    assertContains("\"devices\":[");
    assertContains("\"name\":\"LAMP_C6\"");
    assertContains("\"status\":\"offline\"");
    assertContains("\"parent_id\":");
    assertContains("\"state\":null");
    assertContains("\"last_confirmed_state\":{\"power\":null,\"status\":\"unknown\"}");
    assertContains("\"count\":1");
    TEST_ASSERT_NULL(strstr(response, "MAIN_LIGHTING"));

    char path[64] = {};
    snprintf(path, sizeof(path), "/api/v1/devices/%lu",
             static_cast<unsigned long>(lampId));
    TEST_ASSERT_EQUAL_UINT16(200, request("GET", path, length));
    assertContains("\"device\":{\"id\":");
    assertContains("\"online\":false");
    assertContains("\"state\":null");
    assertContains("\"last_confirmed_state\":{\"power\":null,\"status\":\"unknown\"}");
}

void test_last_confirmed_power_is_additive_and_offline_history_is_stale() {
    const uint32_t mainLocalId = 0x1234;
    const uint32_t mainId = makeCoreModuleId(CORE_LOGICAL_ID, mainLocalId);
    const uint32_t lampId = makeCoreModuleId(mainId, 17);
    addDevice(mainId, "MAIN_LIGHTING", DeviceRole::MAIN,
              DeviceStatus::ONLINE, CORE_LOGICAL_ID, mainLocalId);
    addDevice(lampId, "LAMP_C6", DeviceRole::LAMP,
              DeviceStatus::ONLINE, mainId, 17, DEVICE_CAP_POWER);
    Device* lamp = findDeviceById(registry, lampId);
    TEST_ASSERT_NOT_NULL(lamp);
    lamp->lastConfirmedPower = LastConfirmedPower::ON;
    lamp->lastConfirmedPowerStatus = LastConfirmedPowerStatus::CONFIRMED;

    size_t length = 0;
    TEST_ASSERT_EQUAL_UINT16(200, request("GET", "/api/v1/devices", length));
    assertContains("\"state\":null");
    assertContains("\"last_confirmed_state\":{\"power\":\"on\",\"status\":\"confirmed\"}");

    TEST_ASSERT_TRUE(setDeviceStatus(registry, lampId, DeviceStatus::OFFLINE, 9000));
    char path[64] = {};
    snprintf(path, sizeof(path), "/api/v1/devices/%lu",
             static_cast<unsigned long>(lampId));
    TEST_ASSERT_EQUAL_UINT16(200, request("GET", path, length));
    assertContains("\"online\":false");
    assertContains("\"state\":null");
    assertContains("\"last_confirmed_state\":{\"power\":\"on\",\"status\":\"stale\"}");
}

void test_unknown_device_is_404() {
    size_t length = 0;
    TEST_ASSERT_EQUAL_UINT16(404,
        request("GET", "/api/v1/devices/55", length));
    assertContains("\"code\":\"not_found\"");
}

void test_invalid_ids_unknown_routes_and_unsupported_methods_have_http_errors() {
    size_t length = 0;
    TEST_ASSERT_EQUAL_UINT16(400,
        request("GET", "/api/v1/devices/abc", length));
    assertContains("\"code\":\"bad_request\"");

    TEST_ASSERT_EQUAL_UINT16(400,
        request("GET", "/api/v1/devices/4294967296", length));
    assertContains("\"code\":\"bad_request\"");

    TEST_ASSERT_EQUAL_UINT16(400,
        request("GET", "/api/v1/modules?role=main", length));
    TEST_ASSERT_EQUAL_UINT16(404,
        request("GET", "/api/v1/unknown", length));
    TEST_ASSERT_EQUAL_UINT16(405,
        request("POST", "/api/v1/modules", length));
    assertContains("\"code\":\"method_not_allowed\"");
}

void test_empty_inventory_returns_empty_arrays() {
    size_t length = 0;
    TEST_ASSERT_EQUAL_UINT16(200, request("GET", "/api/v1/modules", length));
    TEST_ASSERT_EQUAL_STRING("{\"modules\":[],\"count\":0}", response);
    TEST_ASSERT_EQUAL_UINT16(200, request("GET", "/api/v1/devices", length));
    TEST_ASSERT_EQUAL_STRING("{\"devices\":[],\"count\":0}", response);
}

void test_missing_or_inconsistent_core_root_returns_503() {
    initDeviceRegistry(registry);
    size_t length = 0;
    TEST_ASSERT_EQUAL_UINT16(503, request("GET", "/api/v1/health", length));
    assertContains("\"code\":\"core_not_ready\"");

    addDevice(CORE_LOGICAL_ID, "NOT_CORE", DeviceRole::MAIN,
              DeviceStatus::ONLINE, 0, CORE_LOGICAL_ID);
    TEST_ASSERT_EQUAL_UINT16(503, request("GET", "/api/v1/core", length));
}

void test_json_strings_are_escaped() {
    addDevice(2, "LIGHT \"A\"", DeviceRole::MAIN,
              DeviceStatus::ONLINE, CORE_LOGICAL_ID, 2);
    runtime.firmwareVersion = "v\"1\\test";

    size_t length = 0;
    TEST_ASSERT_EQUAL_UINT16(200, request("GET", "/api/v1/health", length));
    assertContains("\"firmware_version\":\"v\\\"1\\\\test\"");

    TEST_ASSERT_EQUAL_UINT16(200, request("GET", "/api/v1/modules", length));
    assertContains("LIGHT \\\"A\\\"");
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_health_response_reports_root_runtime_and_transport_state);
    RUN_TEST(test_core_endpoint_uses_public_fields_and_stable_root_id);
    RUN_TEST(test_modules_list_filters_by_main_role_and_preserves_ids);
    RUN_TEST(test_module_item_returns_known_main_and_unknown_module_is_404);
    RUN_TEST(test_devices_list_and_known_device_report_connectivity_not_fake_domain_state);
    RUN_TEST(test_last_confirmed_power_is_additive_and_offline_history_is_stale);
    RUN_TEST(test_unknown_device_is_404);
    RUN_TEST(test_invalid_ids_unknown_routes_and_unsupported_methods_have_http_errors);
    RUN_TEST(test_empty_inventory_returns_empty_arrays);
    RUN_TEST(test_missing_or_inconsistent_core_root_returns_503);
    RUN_TEST(test_json_strings_are_escaped);
    return UNITY_END();
}
