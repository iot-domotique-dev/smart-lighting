#include <unity.h>

#include "communication.h"
#include "device_manager.h"
#include "device_registry.h"
#include "event_bus.h"

extern "C" void setUp(void) {}
extern "C" void tearDown(void) {}

FakeHardwareSerial Serial;

static uint32_t fakeNow = 0;

uint32_t millis() {
    return fakeNow;
}

bool receiveMessage(Communication&, Message&) {
    return false;
}

bool sendMessage(Communication&, Message) {
    return true;
}

static Device makeDevice(
    uint32_t id,
    const char* name,
    DeviceRole role,
    DeviceStatus status = DeviceStatus::ONLINE,
    uint32_t parentId = 0,
    uint32_t capabilities = 0
) {
    Device device = {};
    device.id = id;
    device.name = name;
    device.role = role;
    device.status = status;
    device.lastSeen = fakeNow;
    device.parentId = parentId;
    device.capabilities = capabilities;
    return device;
}

void test_registry_starts_empty_and_exposes_list() {
    DeviceRegistry registry;
    initDeviceRegistry(registry);

    uint8_t count = 99;
    const Device* devices = getAllDevices(registry, count);

    TEST_ASSERT_EQUAL_UINT8(0, count);
    TEST_ASSERT_NOT_NULL(devices);
    TEST_ASSERT_NULL(findDeviceById(registry, 1));
}

void test_registry_stores_core_main_and_lamp_together() {
    DeviceRegistry registry;
    initDeviceRegistry(registry);

    TEST_ASSERT_TRUE(registerDevice(
        registry,
        makeDevice(100, "CORE_001", DeviceRole::CORE)
    ));
    TEST_ASSERT_TRUE(registerDevice(
        registry,
        makeDevice(200, "MAIN_LIGHTING_001", DeviceRole::MAIN,
                   DeviceStatus::ONLINE, 100,
                   DEVICE_CAP_LIGHTING | DEVICE_CAP_GROUPS |
                       DEVICE_CAP_SCENES | DEVICE_CAP_AUTOMATION)
    ));
    TEST_ASSERT_TRUE(registerDevice(
        registry,
        makeDevice(201, "LAMP_C6_001", DeviceRole::LAMP,
                   DeviceStatus::ONLINE, 200,
                   DEVICE_CAP_POWER | DEVICE_CAP_BRIGHTNESS |
                       DEVICE_CAP_AUTOMATIC)
    ));

    uint8_t count = 0;
    const Device* devices = getAllDevices(registry, count);
    TEST_ASSERT_EQUAL_UINT8(3, count);
    TEST_ASSERT_EQUAL_UINT32(100, devices[0].id);
    TEST_ASSERT_EQUAL_UINT32(200, devices[1].id);
    TEST_ASSERT_EQUAL_UINT32(201, devices[2].id);
    TEST_ASSERT_EQUAL_UINT32(100, devices[1].parentId);
    TEST_ASSERT_EQUAL_UINT32(200, devices[2].parentId);
}

void test_registry_owns_device_names() {
    DeviceRegistry registry;
    initDeviceRegistry(registry);
    char temporaryName[] = "MAIN_SECURITY_001";

    Device device = makeDevice(30, temporaryName, DeviceRole::MAIN);
    TEST_ASSERT_TRUE(registerDevice(registry, device));
    temporaryName[0] = 'X';

    TEST_ASSERT_EQUAL_STRING(
        "MAIN_SECURITY_001",
        findDeviceById(registry, 30)->name
    );
}

void test_registry_lookup_exists_update_and_unregister() {
    DeviceRegistry registry;
    initDeviceRegistry(registry);
    TEST_ASSERT_TRUE(registerDevice(
        registry,
        makeDevice(41, "MAIN_LIGHTING_001", DeviceRole::MAIN)
    ));

    TEST_ASSERT_TRUE(deviceExists(registry, 41));
    TEST_ASSERT_FALSE(deviceExists(registry, 42));
    TEST_ASSERT_NULL(findDeviceById(registry, 42));

    Device replacement = makeDevice(
        41, "MAIN_SECURITY_001", DeviceRole::MAIN,
        DeviceStatus::OFFLINE, 100, DEVICE_CAP_ALARM
    );
    TEST_ASSERT_TRUE(updateDevice(registry, replacement));
    TEST_ASSERT_EQUAL_STRING("MAIN_SECURITY_001", findDeviceById(registry, 41)->name);
    TEST_ASSERT_EQUAL_UINT32(100, findDeviceById(registry, 41)->parentId);
    TEST_ASSERT_EQUAL_UINT32(DEVICE_CAP_ALARM,
                             findDeviceById(registry, 41)->capabilities);
    TEST_ASSERT_FALSE(updateDevice(registry, makeDevice(
        42, "missing", DeviceRole::MAIN
    )));

    TEST_ASSERT_TRUE(unregisterDevice(registry, 41));
    TEST_ASSERT_FALSE(unregisterDevice(registry, 41));
    TEST_ASSERT_FALSE(deviceExists(registry, 41));
}

void test_registry_finds_devices_by_role() {
    DeviceRegistry registry;
    initDeviceRegistry(registry);
    TEST_ASSERT_TRUE(registerDevice(
        registry, makeDevice(1, "MAIN_LIGHTING_001", DeviceRole::MAIN)
    ));
    TEST_ASSERT_TRUE(registerDevice(
        registry, makeDevice(2, "MAIN_SECURITY_001", DeviceRole::MAIN)
    ));
    TEST_ASSERT_TRUE(registerDevice(
        registry, makeDevice(3, "LAMP_C6_001", DeviceRole::LAMP)
    ));
    TEST_ASSERT_TRUE(registerDevice(
        registry, makeDevice(4, "SENSOR_001", DeviceRole::SENSOR)
    ));
    TEST_ASSERT_TRUE(registerDevice(
        registry, makeDevice(5, "CAMERA_001", DeviceRole::CAMERA)
    ));

    Device* matches[1] = {};
    const uint8_t total = findDevicesByRole(
        registry, DeviceRole::MAIN, matches, 1
    );
    TEST_ASSERT_EQUAL_UINT8(2, total);
    TEST_ASSERT_NOT_NULL(matches[0]);
    TEST_ASSERT_EQUAL_UINT32(1, matches[0]->id);

    TEST_ASSERT_EQUAL_UINT8(
        1,
        findDevicesByRole(registry, DeviceRole::SENSOR, nullptr, 0)
    );
    TEST_ASSERT_EQUAL_UINT8(
        1,
        findDevicesByRole(registry, DeviceRole::CAMERA, nullptr, 0)
    );
}

void test_capabilities_cover_lighting_and_future_security_modules() {
    Device lamp = makeDevice(
        7, "LAMP_C6_001", DeviceRole::LAMP,
        DeviceStatus::ONLINE, 5,
        DEVICE_CAP_POWER | DEVICE_CAP_BRIGHTNESS | DEVICE_CAP_AUTOMATIC
    );
    Device main = makeDevice(
        5, "MAIN_LIGHTING_001", DeviceRole::MAIN,
        DeviceStatus::ONLINE, 1,
        DEVICE_CAP_LIGHTING | DEVICE_CAP_GROUPS |
            DEVICE_CAP_SCENES | DEVICE_CAP_AUTOMATION
    );
    Device security = makeDevice(
        8, "MAIN_SECURITY_001", DeviceRole::MAIN,
        DeviceStatus::ONLINE, 1,
        DEVICE_CAP_ALARM | DEVICE_CAP_PRESENCE |
            DEVICE_CAP_DOOR_SENSOR | DEVICE_CAP_SECURITY_MODE
    );

    TEST_ASSERT_TRUE(hasDeviceCapability(lamp, DEVICE_CAP_POWER));
    TEST_ASSERT_TRUE(hasDeviceCapability(lamp, DEVICE_CAP_BRIGHTNESS));
    TEST_ASSERT_TRUE(hasDeviceCapability(lamp, DEVICE_CAP_AUTOMATIC));
    TEST_ASSERT_FALSE(hasDeviceCapability(lamp, DEVICE_CAP_ALARM));
    TEST_ASSERT_TRUE(hasDeviceCapability(main, DEVICE_CAP_LIGHTING));
    TEST_ASSERT_TRUE(hasDeviceCapability(main, DEVICE_CAP_AUTOMATION));
    TEST_ASSERT_TRUE(hasDeviceCapability(security, DEVICE_CAP_ALARM));
    TEST_ASSERT_TRUE(hasDeviceCapability(security, DEVICE_CAP_SECURITY_MODE));
}

void test_generic_status_manager_updates_all_roles_and_events() {
    DeviceRegistry registry;
    EventBus events;
    initDeviceRegistry(registry);
    initEventBus(events);
    fakeNow = 0;

    TEST_ASSERT_TRUE(registerDevice(
        registry, makeDevice(1, "CORE_001", DeviceRole::CORE)
    ));
    TEST_ASSERT_TRUE(registerDevice(
        registry, makeDevice(2, "MAIN_LIGHTING_001", DeviceRole::MAIN)
    ));
    TEST_ASSERT_TRUE(registerDevice(
        registry, makeDevice(3, "LAMP_C6_001", DeviceRole::LAMP)
    ));

    fakeNow = DEVICE_TIMEOUT + 1;
    updateDeviceStatus(registry, &events);

    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(DeviceStatus::OFFLINE),
        static_cast<int>(findDeviceById(registry, 1)->status)
    );
    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(DeviceStatus::OFFLINE),
        static_cast<int>(findDeviceById(registry, 2)->status)
    );
    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(DeviceStatus::OFFLINE),
        static_cast<int>(findDeviceById(registry, 3)->status)
    );

    Event event = {};
    TEST_ASSERT_TRUE(consumeEvent(events, event));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(EventType::DEVICE_OFFLINE),
                          static_cast<int>(event.type));
    TEST_ASSERT_TRUE(consumeEvent(events, event));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(EventType::DEVICE_OFFLINE),
                          static_cast<int>(event.type));
    TEST_ASSERT_TRUE(consumeEvent(events, event));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(EventType::LAMP_OFFLINE),
                          static_cast<int>(event.type));

    updateDeviceSeen(*findDeviceById(registry, 2), &events);
    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(DeviceStatus::ONLINE),
        static_cast<int>(findDeviceById(registry, 2)->status)
    );
    TEST_ASSERT_TRUE(consumeEvent(events, event));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(EventType::DEVICE_ONLINE),
                          static_cast<int>(event.type));
}

void test_registry_capacity_and_duplicate_ids_are_enforced() {
    DeviceRegistry registry;
    initDeviceRegistry(registry);
    for (uint32_t id = 0; id < MAX_DEVICES; ++id) {
        TEST_ASSERT_TRUE(registerDevice(
            registry,
            makeDevice(id, "device", DeviceRole::RELAY)
        ));
    }
    TEST_ASSERT_FALSE(registerDevice(
        registry,
        makeDevice(MAX_DEVICES, "full", DeviceRole::RELAY)
    ));
    TEST_ASSERT_FALSE(registerDevice(
        registry,
        makeDevice(0, "duplicate", DeviceRole::CORE)
    ));
    TEST_ASSERT_EQUAL_UINT8(MAX_DEVICES, registry.count);
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_registry_starts_empty_and_exposes_list);
    RUN_TEST(test_registry_stores_core_main_and_lamp_together);
    RUN_TEST(test_registry_owns_device_names);
    RUN_TEST(test_registry_lookup_exists_update_and_unregister);
    RUN_TEST(test_registry_finds_devices_by_role);
    RUN_TEST(test_capabilities_cover_lighting_and_future_security_modules);
    RUN_TEST(test_generic_status_manager_updates_all_roles_and_events);
    RUN_TEST(test_registry_capacity_and_duplicate_ids_are_enforced);
    return UNITY_END();
}
