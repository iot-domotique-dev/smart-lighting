#include <unity.h>
#include <string.h>

#include "provisioning.h"
#include "simulation_transport.h"

extern "C" void setUp(void) {}
extern "C" void tearDown(void) {}

FakeHardwareSerial Serial;

uint32_t millis() {
    return 0;
}

bool receiveMessage(Communication&, Message&) {
    return false;
}

bool sendMessage(Communication&, Message) {
    return true;
}

static bool completePairing(
    MainProvisioning& main,
    ProvisioningLamp& lamp,
    const char* hardwareId,
    const char* name,
    ProvisioningPacket& confirmation
) {
    ProvisioningPacket request = {};
    ProvisioningPacket rejection = {};
    ProvisioningPacket acceptance = {};
    if (!main.createPairRequest(hardwareId, request) ||
        !lamp.handlePairRequest(request, rejection) ||
        !main.createPairAcceptance(hardwareId, name, acceptance) ||
        !lamp.handlePairAccept(acceptance, confirmation) ||
        !main.confirmPairing(confirmation)) {
        return false;
    }
    return true;
}

void test_unconfigured_lamp_announces_identity_through_simulation_transport() {
    MemoryProvisioningStore storage;
    ProvisioningLamp lamp("HW-ABC123", "0.4.0", 0x05, storage);
    ProvisioningPacket announcement = lamp.announce();

    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(ProvisioningPacketType::DEVICE_ANNOUNCE),
        static_cast<int>(announcement.type)
    );
    TEST_ASSERT_EQUAL_STRING("HW-ABC123", announcement.hardwareId);
    TEST_ASSERT_EQUAL_STRING("", announcement.firmwareVersion);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(DeviceRole::LAMP),
                          static_cast<int>(announcement.role));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(PairingState::UNPAIRED),
                          static_cast<int>(announcement.pairingState));
    TEST_ASSERT_EQUAL_UINT32(0, announcement.deviceId);

    Message encoded = {};
    TEST_ASSERT_TRUE(encodeProvisioningPacket(announcement, 42, 0, 0, encoded));
    SimulationTransport transport;
    TEST_ASSERT_TRUE(transport.begin());
    TEST_ASSERT_TRUE(transport.send(encoded));
    Message received = {};
    TEST_ASSERT_TRUE(transport.receive(received));
    ProvisioningPacket decoded = {};
    TEST_ASSERT_TRUE(decodeProvisioningPacket(received, decoded));
    TEST_ASSERT_EQUAL_STRING("HW-ABC123", decoded.hardwareId);
    TEST_ASSERT_EQUAL_STRING("", decoded.firmwareVersion);
    TEST_ASSERT_EQUAL_UINT32(0x05, decoded.capabilities);
}

void test_discovery_does_not_automatically_pair_lamp() {
    LampRegistry registry;
    initLampRegistry(registry);
    MainProvisioning main(10, registry);
    MemoryProvisioningStore storage;
    ProvisioningLamp lamp("HW-1", "0.4.0", 0, storage);

    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(DiscoveryResult::DISCOVERED_UNPAIRED),
        static_cast<int>(main.discover(lamp.announce()))
    );
    TEST_ASSERT_EQUAL_UINT8(1, main.detectedCount());
    TEST_ASSERT_EQUAL_UINT8(0, registry.count);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(PairingState::UNPAIRED),
                          static_cast<int>(lamp.state()));
}

void test_explicit_commissioning_is_required_before_pair_request() {
    LampRegistry registry;
    initLampRegistry(registry);
    MainProvisioning main(10, registry);
    MemoryProvisioningStore storage;
    ProvisioningLamp lamp("HW-2", "0.4.0", 0, storage);
    ProvisioningPacket request = {};

    main.discover(lamp.announce());
    TEST_ASSERT_FALSE(main.createPairRequest("HW-2", request));
    main.startCommissioning();
    TEST_ASSERT_TRUE(main.createPairRequest("HW-2", request));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ProvisioningPacketType::PAIR_REQUEST),
                          static_cast<int>(request.type));
}

void test_main_assigns_an_available_device_id() {
    LampRegistry registry;
    initLampRegistry(registry);
    Lamp existing = {};
    existing.device.id = 1;
    existing.device.role = DeviceRole::LAMP;
    existing.device.status = DeviceStatus::ONLINE;
    TEST_ASSERT_TRUE(addLamp(registry, existing));

    MainProvisioning main(10, registry);
    MemoryProvisioningStore storage;
    ProvisioningLamp lamp("HW-3", "0.4.0", 0, storage);
    main.discover(lamp.announce());
    main.startCommissioning();
    ProvisioningPacket confirmation = {};
    TEST_ASSERT_TRUE(completePairing(main, lamp, "HW-3", "LAMP", confirmation));

    TEST_ASSERT_EQUAL_UINT32(2, confirmation.deviceId);
    TEST_ASSERT_NOT_EQUAL(1, confirmation.deviceId);
}

void test_main_assigns_parent_main_id() {
    LampRegistry registry;
    initLampRegistry(registry);
    MainProvisioning main(17, registry);
    MemoryProvisioningStore storage;
    ProvisioningLamp lamp("HW-4", "0.4.0", 0, storage);
    main.discover(lamp.announce());
    main.startCommissioning();

    ProvisioningPacket confirmation = {};
    TEST_ASSERT_TRUE(completePairing(main, lamp, "HW-4", "LAMP", confirmation));
    TEST_ASSERT_EQUAL_UINT32(17, confirmation.parentMainId);
    TEST_ASSERT_EQUAL_UINT32(17, findLamp(registry, confirmation.deviceId)
                                     ->identity.parentMainId);
}

void test_lamp_returns_pair_confirm_after_acceptance() {
    LampRegistry registry;
    initLampRegistry(registry);
    MainProvisioning main(10, registry);
    MemoryProvisioningStore storage;
    ProvisioningLamp lamp("HW-5", "0.4.0", 0, storage);
    main.discover(lamp.announce());
    main.startCommissioning();
    ProvisioningPacket request = {};
    ProvisioningPacket rejection = {};
    ProvisioningPacket acceptance = {};
    ProvisioningPacket confirmation = {};

    TEST_ASSERT_TRUE(main.createPairRequest("HW-5", request));
    TEST_ASSERT_TRUE(lamp.handlePairRequest(request, rejection));
    TEST_ASSERT_TRUE(main.createPairAcceptance("HW-5", "Hall", acceptance));
    TEST_ASSERT_TRUE(lamp.handlePairAccept(acceptance, confirmation));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ProvisioningPacketType::PAIR_CONFIRM),
                          static_cast<int>(confirmation.type));
    TEST_ASSERT_EQUAL_STRING("Hall", confirmation.name);
    TEST_ASSERT_EQUAL_STRING("HW-5", confirmation.hardwareId);
}

void test_lamp_is_paired_only_after_persisting_acceptance_and_main_confirmation() {
    LampRegistry registry;
    initLampRegistry(registry);
    MainProvisioning main(10, registry);
    MemoryProvisioningStore storage;
    ProvisioningLamp lamp("HW-6", "0.4.0", 0, storage);
    main.discover(lamp.announce());
    main.startCommissioning();

    ProvisioningPacket request = {};
    ProvisioningPacket rejection = {};
    ProvisioningPacket acceptance = {};
    ProvisioningPacket confirmation = {};
    TEST_ASSERT_TRUE(main.createPairRequest("HW-6", request));
    TEST_ASSERT_TRUE(lamp.handlePairRequest(request, rejection));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(PairingState::PAIRING),
                          static_cast<int>(lamp.state()));
    TEST_ASSERT_TRUE(main.createPairAcceptance("HW-6", "LAMP", acceptance));
    TEST_ASSERT_TRUE(lamp.handlePairAccept(acceptance, confirmation));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(PairingState::PAIRED),
                          static_cast<int>(lamp.state()));

    ProvisioningRecord saved = {};
    TEST_ASSERT_TRUE(storage.load(saved));
    TEST_ASSERT_EQUAL_STRING("HW-6", saved.hardwareId);
    TEST_ASSERT_EQUAL_UINT32(acceptance.deviceId, saved.deviceId);
    TEST_ASSERT_EQUAL_UINT32(10, saved.parentMainId);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(PairingState::PAIRED),
                          static_cast<int>(saved.pairingState));
    TEST_ASSERT_TRUE(main.confirmPairing(confirmation));
    TEST_ASSERT_EQUAL_UINT8(1, registry.count);
}

void test_two_lamps_receive_different_device_ids() {
    LampRegistry registry;
    initLampRegistry(registry);
    MainProvisioning main(10, registry);
    MemoryProvisioningStore firstStorage;
    MemoryProvisioningStore secondStorage;
    ProvisioningLamp first("HW-7A", "0.4.0", 0, firstStorage);
    ProvisioningLamp second("HW-7B", "0.4.0", 0, secondStorage);
    main.discover(first.announce());
    main.discover(second.announce());
    main.startCommissioning();

    ProvisioningPacket firstResult = {};
    ProvisioningPacket secondResult = {};
    TEST_ASSERT_TRUE(completePairing(main, first, "HW-7A", "LAMP", firstResult));
    TEST_ASSERT_TRUE(completePairing(main, second, "HW-7B", "LAMP", secondResult));
    TEST_ASSERT_NOT_EQUAL(firstResult.deviceId, secondResult.deviceId);
    TEST_ASSERT_EQUAL_UINT8(2, registry.count);
}

void test_repeated_hardware_announcement_does_not_duplicate_registry_entry() {
    LampRegistry registry;
    initLampRegistry(registry);
    MainProvisioning main(10, registry);
    MemoryProvisioningStore storage;
    ProvisioningLamp lamp("HW-8", "0.4.0", 0, storage);
    main.discover(lamp.announce());
    main.startCommissioning();
    ProvisioningPacket result = {};
    TEST_ASSERT_TRUE(completePairing(main, lamp, "HW-8", "LAMP", result));

    TEST_ASSERT_EQUAL_INT(static_cast<int>(DiscoveryResult::RECOGNIZED_PAIRED),
                          static_cast<int>(main.discover(lamp.announce())));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(DiscoveryResult::RECOGNIZED_PAIRED),
                          static_cast<int>(main.discover(lamp.announce())));
    TEST_ASSERT_EQUAL_UINT8(1, registry.count);
    TEST_ASSERT_EQUAL_UINT8(1, main.detectedCount());
}

void test_paired_lamp_is_recognized_after_main_restart_without_new_id() {
    LampRegistry firstRegistry;
    initLampRegistry(firstRegistry);
    MainProvisioning firstMain(10, firstRegistry);
    MemoryProvisioningStore storage;
    ProvisioningLamp firstBoot("HW-9", "0.4.0", 0, storage);
    firstMain.discover(firstBoot.announce());
    firstMain.startCommissioning();
    ProvisioningPacket original = {};
    TEST_ASSERT_TRUE(completePairing(firstMain, firstBoot, "HW-9", "Desk", original));

    ProvisioningLamp rebootedLamp("HW-9", "0.4.0", 0, storage);
    LampRegistry restartedRegistry;
    initLampRegistry(restartedRegistry);
    MainProvisioning restartedMain(10, restartedRegistry);
    ProvisioningPacket rebootAnnouncement = rebootedLamp.announce();
    TEST_ASSERT_EQUAL_INT(static_cast<int>(PairingState::PAIRED),
                          static_cast<int>(rebootAnnouncement.pairingState));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(DiscoveryResult::RESTORED_PAIRED),
                          static_cast<int>(restartedMain.discover(rebootAnnouncement)));
    TEST_ASSERT_EQUAL_UINT32(original.deviceId, restartedRegistry.lamps[0].device.id);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(DiscoveryResult::RECOGNIZED_PAIRED),
                          static_cast<int>(restartedMain.discover(rebootedLamp.announce())));
    TEST_ASSERT_EQUAL_UINT8(1, restartedRegistry.count);
}

void test_paired_lamp_cannot_be_assigned_to_a_second_main() {
    LampRegistry firstRegistry;
    initLampRegistry(firstRegistry);
    MainProvisioning firstMain(10, firstRegistry);
    MemoryProvisioningStore storage;
    ProvisioningLamp lamp("HW-10", "0.4.0", 0, storage);
    firstMain.discover(lamp.announce());
    firstMain.startCommissioning();
    ProvisioningPacket firstPair = {};
    TEST_ASSERT_TRUE(completePairing(firstMain, lamp, "HW-10", "LAMP", firstPair));

    LampRegistry secondRegistry;
    initLampRegistry(secondRegistry);
    MainProvisioning secondMain(11, secondRegistry);
    secondMain.discover(lamp.announce());
    secondMain.startCommissioning();
    ProvisioningPacket request = {};
    TEST_ASSERT_FALSE(secondMain.createPairRequest("HW-10", request));

    request.type = ProvisioningPacketType::PAIR_REQUEST;
    request.role = DeviceRole::MAIN;
    request.parentMainId = 11;
    strncpy(request.hardwareId, "HW-10", sizeof(request.hardwareId) - 1);
    ProvisioningPacket rejection = {};
    TEST_ASSERT_FALSE(lamp.handlePairRequest(request, rejection));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(PairingState::PAIRED),
                          static_cast<int>(lamp.state()));
    TEST_ASSERT_EQUAL_UINT32(10, lamp.announce().parentMainId);
}

void test_two_mains_can_discover_but_only_commissioning_main_can_pair() {
    LampRegistry firstRegistry;
    LampRegistry secondRegistry;
    initLampRegistry(firstRegistry);
    initLampRegistry(secondRegistry);
    MainProvisioning firstMain(20, firstRegistry);
    MainProvisioning secondMain(21, secondRegistry);
    MemoryProvisioningStore storage;
    ProvisioningLamp lamp("HW-11", "0.4.0", 0, storage);
    ProvisioningPacket announcement = lamp.announce();

    TEST_ASSERT_EQUAL_INT(static_cast<int>(DiscoveryResult::DISCOVERED_UNPAIRED),
                          static_cast<int>(firstMain.discover(announcement)));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(DiscoveryResult::DISCOVERED_UNPAIRED),
                          static_cast<int>(secondMain.discover(announcement)));
    firstMain.startCommissioning();
    ProvisioningPacket request = {};
    TEST_ASSERT_TRUE(firstMain.createPairRequest("HW-11", request));
    TEST_ASSERT_FALSE(secondMain.createPairRequest("HW-11", request));

    ProvisioningPacket rejection = {};
    TEST_ASSERT_TRUE(lamp.handlePairRequest(request, rejection));
    ProvisioningPacket acceptance = {};
    ProvisioningPacket confirmation = {};
    TEST_ASSERT_TRUE(firstMain.createPairAcceptance("HW-11", "LAMP", acceptance));
    TEST_ASSERT_TRUE(lamp.handlePairAccept(acceptance, confirmation));
    TEST_ASSERT_TRUE(firstMain.confirmPairing(confirmation));
    TEST_ASSERT_EQUAL_UINT8(1, firstRegistry.count);
    TEST_ASSERT_EQUAL_UINT8(0, secondRegistry.count);
}

void test_registry_finds_provisioned_lamp_by_hardware_id() {
    LampRegistry registry;
    initLampRegistry(registry);
    MainProvisioning main(30, registry);
    MemoryProvisioningStore storage;
    ProvisioningLamp lamp("HW-12", "0.4.0", 0, storage);
    main.discover(lamp.announce());
    main.startCommissioning();
    ProvisioningPacket result = {};
    TEST_ASSERT_TRUE(completePairing(main, lamp, "HW-12", "LAMP", result));

    Lamp* found = findLampByHardwareId(registry, "HW-12");
    TEST_ASSERT_NOT_NULL(found);
    TEST_ASSERT_EQUAL_UINT32(result.deviceId, found->device.id);
    TEST_ASSERT_EQUAL_PTR(found, findLampByDeviceId(registry, result.deviceId));
    TEST_ASSERT_NULL(findLampByHardwareId(registry, "HW-missing"));

    Lamp duplicate = {};
    duplicate.device.id = result.deviceId + 1;
    strncpy(duplicate.identity.hardwareId, "HW-12",
            sizeof(duplicate.identity.hardwareId) - 1);
    TEST_ASSERT_FALSE(addLamp(registry, duplicate));
    TEST_ASSERT_TRUE(removeLamp(registry, result.deviceId));
    TEST_ASSERT_EQUAL_UINT8(0, registry.count);
    TEST_ASSERT_NULL(findLampByDeviceId(registry, result.deviceId));
}

void test_registry_finds_lamps_by_parent_main_id() {
    LampRegistry registry;
    initLampRegistry(registry);
    MainProvisioning main(31, registry);
    MemoryProvisioningStore firstStorage;
    MemoryProvisioningStore secondStorage;
    ProvisioningLamp first("HW-13A", "0.4.0", 0, firstStorage);
    ProvisioningLamp second("HW-13B", "0.4.0", 0, secondStorage);
    main.discover(first.announce());
    main.discover(second.announce());
    main.startCommissioning();
    ProvisioningPacket firstResult = {};
    ProvisioningPacket secondResult = {};
    TEST_ASSERT_TRUE(completePairing(main, first, "HW-13A", "LAMP", firstResult));
    TEST_ASSERT_TRUE(completePairing(main, second, "HW-13B", "LAMP", secondResult));

    Lamp* matches[MAX_LAMPS] = {};
    TEST_ASSERT_EQUAL_UINT8(2,
        findLampsByParentMainId(registry, 31, matches, MAX_LAMPS));
    TEST_ASSERT_EQUAL_UINT32(31, matches[0]->identity.parentMainId);
    TEST_ASSERT_EQUAL_UINT8(0,
        findLampsByParentMainId(registry, 99, matches, MAX_LAMPS));
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_unconfigured_lamp_announces_identity_through_simulation_transport);
    RUN_TEST(test_discovery_does_not_automatically_pair_lamp);
    RUN_TEST(test_explicit_commissioning_is_required_before_pair_request);
    RUN_TEST(test_main_assigns_an_available_device_id);
    RUN_TEST(test_main_assigns_parent_main_id);
    RUN_TEST(test_lamp_returns_pair_confirm_after_acceptance);
    RUN_TEST(test_lamp_is_paired_only_after_persisting_acceptance_and_main_confirmation);
    RUN_TEST(test_two_lamps_receive_different_device_ids);
    RUN_TEST(test_repeated_hardware_announcement_does_not_duplicate_registry_entry);
    RUN_TEST(test_paired_lamp_is_recognized_after_main_restart_without_new_id);
    RUN_TEST(test_paired_lamp_cannot_be_assigned_to_a_second_main);
    RUN_TEST(test_two_mains_can_discover_but_only_commissioning_main_can_pair);
    RUN_TEST(test_registry_finds_provisioned_lamp_by_hardware_id);
    RUN_TEST(test_registry_finds_lamps_by_parent_main_id);
    return UNITY_END();
}
