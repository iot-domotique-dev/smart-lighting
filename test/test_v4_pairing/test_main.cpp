#include <unity.h>
#include <stdio.h>
#include <string.h>

#include "provisioning.h"
#include "core_module_service.h"
#include "simulation_transport.h"

FakeHardwareSerial Serial;
static uint32_t fakeMillis = 0;

extern "C" void setUp(void) { fakeMillis = 0; }
extern "C" void tearDown(void) {}

uint32_t millis() {
    return fakeMillis;
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

static void setPacketEnvelope(ProvisioningPacket& packet, uint32_t sourceId,
                              uint32_t destinationId) {
    packet.sourceId = sourceId;
    packet.destinationId = destinationId;
}

static void savePairedLampRecord(
    MemoryProvisioningStore& storage,
    const char* hardwareId,
    uint32_t deviceId,
    uint32_t mainId
) {
    ProvisioningRecord record = {};
    record.deviceId = deviceId;
    strncpy(record.hardwareId, hardwareId, sizeof(record.hardwareId) - 1);
    strncpy(record.name, "LAMP", sizeof(record.name) - 1);
    record.parentMainId = mainId;
    record.pairingState = PairingState::PAIRED;
    TEST_ASSERT_TRUE(storage.save(record));
}

class CorruptProvisioningReservationStore : public ProvisioningReservationStore {
public:
    bool load(
        uint32_t mainId,
        MainProvisioningReservationState& state
    ) const override {
        initializeMainProvisioningReservationState(mainId, state);
        state.count = 1;
        state.identities[0].deviceId = 11;
        strncpy(state.identities[0].hardwareId, "HW-CORRUPT",
                sizeof(state.identities[0].hardwareId) - 1);
        // The reservation lies above its watermark and must fail validation.
        return true;
    }

    bool save(const MainProvisioningReservationState&) override {
        return false;
    }
};

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
    MemoryProvisioningReservationStore reservations;
    MainProvisioning main(10, registry, &reservations);
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
    MemoryProvisioningReservationStore reservations;
    MainProvisioning main(10, registry, &reservations);
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

    MemoryProvisioningReservationStore reservations;
    MainProvisioning main(10, registry, &reservations);
    MemoryProvisioningStore storage;
    ProvisioningLamp lamp("HW-3", "0.4.0", 0, storage);
    main.discover(lamp.announce());
    main.startCommissioning();
    ProvisioningPacket confirmation = {};
    TEST_ASSERT_TRUE(completePairing(main, lamp, "HW-3", "LAMP", confirmation));

    TEST_ASSERT_EQUAL_UINT32(11, confirmation.deviceId);
    TEST_ASSERT_NOT_EQUAL(1, confirmation.deviceId);
}

void test_main_assigns_parent_main_id() {
    LampRegistry registry;
    initLampRegistry(registry);
    MemoryProvisioningReservationStore reservations;
    MainProvisioning main(17, registry, &reservations);
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
    MemoryProvisioningReservationStore reservations;
    MainProvisioning main(10, registry, &reservations);
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
    MemoryProvisioningReservationStore reservations;
    MainProvisioning main(10, registry, &reservations);
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
    MemoryProvisioningReservationStore reservations;
    MainProvisioning main(10, registry, &reservations);
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
    MemoryProvisioningReservationStore reservations;
    MainProvisioning main(10, registry, &reservations);
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
    MemoryProvisioningReservationStore reservations;
    MainProvisioning firstMain(10, firstRegistry, &reservations);
    MemoryProvisioningStore storage;
    ProvisioningLamp firstBoot("HW-9", "0.4.0", 0, storage);
    firstMain.discover(firstBoot.announce());
    firstMain.startCommissioning();
    ProvisioningPacket original = {};
    TEST_ASSERT_TRUE(completePairing(firstMain, firstBoot, "HW-9", "Desk", original));

    ProvisioningLamp rebootedLamp("HW-9", "0.4.0", 0, storage);
    LampRegistry restartedRegistry;
    initLampRegistry(restartedRegistry);
    MainProvisioning restartedMain(10, restartedRegistry, &reservations);
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
    MemoryProvisioningReservationStore firstReservations;
    MainProvisioning firstMain(10, firstRegistry, &firstReservations);
    MemoryProvisioningStore storage;
    ProvisioningLamp lamp("HW-10", "0.4.0", 0, storage);
    firstMain.discover(lamp.announce());
    firstMain.startCommissioning();
    ProvisioningPacket firstPair = {};
    TEST_ASSERT_TRUE(completePairing(firstMain, lamp, "HW-10", "LAMP", firstPair));

    LampRegistry secondRegistry;
    initLampRegistry(secondRegistry);
    MemoryProvisioningReservationStore secondReservations;
    MainProvisioning secondMain(11, secondRegistry, &secondReservations);
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
    MemoryProvisioningReservationStore firstReservations;
    MemoryProvisioningReservationStore secondReservations;
    MainProvisioning firstMain(20, firstRegistry, &firstReservations);
    MainProvisioning secondMain(21, secondRegistry, &secondReservations);
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
    MemoryProvisioningReservationStore reservations;
    MainProvisioning main(30, registry, &reservations);
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
    MemoryProvisioningReservationStore reservations;
    MainProvisioning main(31, registry, &reservations);
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

void test_upgrade_reserves_legacy_ids_while_old_lamp_is_absent() {
    MemoryProvisioningStore legacyLampStorage;
    savePairedLampRecord(legacyLampStorage, "HW-LEGACY", 1, 40);
    ProvisioningLamp legacyLamp("HW-LEGACY", "0.4.0", 0, legacyLampStorage);

    MemoryProvisioningReservationStore reservations;
    LampRegistry registry;
    initLampRegistry(registry);
    MainProvisioning main(40, registry, &reservations);
    MemoryProvisioningStore newLampStorage;
    ProvisioningLamp newLamp("HW-NEW", "0.4.0", 0, newLampStorage);

    TEST_ASSERT_EQUAL_INT(static_cast<int>(DiscoveryResult::DISCOVERED_UNPAIRED),
                          static_cast<int>(main.discover(newLamp.announce())));
    main.startCommissioning();
    ProvisioningPacket newPair = {};
    TEST_ASSERT_TRUE(completePairing(main, newLamp, "HW-NEW", "New", newPair));
    TEST_ASSERT_EQUAL_UINT32(11, newPair.deviceId);

    TEST_ASSERT_EQUAL_INT(static_cast<int>(DiscoveryResult::RESTORED_PAIRED),
                          static_cast<int>(main.discover(legacyLamp.announce())));
    TEST_ASSERT_EQUAL_UINT8(2, registry.count);
    TEST_ASSERT_NOT_EQUAL(legacyLamp.announce().deviceId, newPair.deviceId);
    TEST_ASSERT_EQUAL_UINT32(1, legacyLamp.announce().deviceId);
}

void test_main_restart_keeps_id_reserved_while_lamp_is_absent() {
    MemoryProvisioningReservationStore reservations;
    MemoryProvisioningStore oldLampStorage;
    LampRegistry firstRegistry;
    initLampRegistry(firstRegistry);
    MainProvisioning firstMain(41, firstRegistry, &reservations);
    ProvisioningLamp oldLamp("HW-OLD", "0.4.0", 0, oldLampStorage);
    firstMain.discover(oldLamp.announce());
    firstMain.startCommissioning();
    ProvisioningPacket oldPair = {};
    TEST_ASSERT_TRUE(completePairing(firstMain, oldLamp, "HW-OLD", "Old", oldPair));
    TEST_ASSERT_EQUAL_UINT32(11, oldPair.deviceId);

    // The MAIN restarts before the paired lamp announces itself.
    LampRegistry restartedRegistry;
    initLampRegistry(restartedRegistry);
    MainProvisioning restartedMain(41, restartedRegistry, &reservations);
    MemoryProvisioningStore newLampStorage;
    ProvisioningLamp newLamp("HW-NEXT", "0.4.0", 0, newLampStorage);
    restartedMain.discover(newLamp.announce());
    restartedMain.startCommissioning();
    ProvisioningPacket newPair = {};
    TEST_ASSERT_TRUE(completePairing(
        restartedMain, newLamp, "HW-NEXT", "Next", newPair));
    TEST_ASSERT_EQUAL_UINT32(12, newPair.deviceId);

    TEST_ASSERT_EQUAL_INT(static_cast<int>(DiscoveryResult::RESTORED_PAIRED),
                          static_cast<int>(restartedMain.discover(oldLamp.announce())));
    TEST_ASSERT_EQUAL_UINT8(2, restartedRegistry.count);
    TEST_ASSERT_NOT_EQUAL(oldPair.deviceId, newPair.deviceId);
}

void test_conflicting_legacy_identity_is_rejected_without_replacing_first() {
    MemoryProvisioningStore firstStorage;
    MemoryProvisioningStore secondStorage;
    savePairedLampRecord(firstStorage, "HW-FIRST", 1, 42);
    savePairedLampRecord(secondStorage, "HW-SECOND", 1, 42);
    ProvisioningLamp first("HW-FIRST", "0.4.0", 0, firstStorage);
    ProvisioningLamp second("HW-SECOND", "0.4.0", 0, secondStorage);

    MemoryProvisioningReservationStore reservations;
    LampRegistry registry;
    initLampRegistry(registry);
    MainProvisioning main(42, registry, &reservations);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(DiscoveryResult::RESTORED_PAIRED),
                          static_cast<int>(main.discover(first.announce())));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(DiscoveryResult::IDENTITY_CONFLICT),
                          static_cast<int>(main.discover(second.announce())));
    TEST_ASSERT_EQUAL_UINT8(1, registry.count);
    TEST_ASSERT_EQUAL_STRING("HW-FIRST", registry.lamps[0].identity.hardwareId);
}

void test_expired_pairing_releases_lamp_and_rejects_late_confirmation() {
    MemoryProvisioningReservationStore reservations;
    LampRegistry registry;
    initLampRegistry(registry);
    MainProvisioning main(43, registry, &reservations);

    MemoryProvisioningStore oldStorage;
    savePairedLampRecord(oldStorage, "HW-EXISTING", 1, 43);
    ProvisioningLamp existing("HW-EXISTING", "0.4.0", 0, oldStorage);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(DiscoveryResult::RESTORED_PAIRED),
                          static_cast<int>(main.discover(existing.announce())));

    MemoryProvisioningStore retryStorage;
    ProvisioningLamp retryLamp("HW-RETRY", "0.4.0", 0, retryStorage);
    main.discover(retryLamp.announce());
    main.startCommissioning();
    ProvisioningPacket request = {};
    ProvisioningPacket rejection = {};
    ProvisioningPacket firstAcceptance = {};
    TEST_ASSERT_TRUE(main.createPairRequest("HW-RETRY", request));
    TEST_ASSERT_TRUE(retryLamp.handlePairRequest(request, rejection));
    TEST_ASSERT_TRUE(main.createPairAcceptance(
        "HW-RETRY", "Retry", firstAcceptance));
    TEST_ASSERT_EQUAL_UINT32(11, firstAcceptance.deviceId);

    ProvisioningPacket lateConfirmation = {};
    lateConfirmation.type = ProvisioningPacketType::PAIR_CONFIRM;
    lateConfirmation.role = DeviceRole::LAMP;
    lateConfirmation.pairingState = PairingState::PAIRED;
    lateConfirmation.deviceId = firstAcceptance.deviceId;
    lateConfirmation.parentMainId = firstAcceptance.parentMainId;
    strncpy(lateConfirmation.hardwareId, firstAcceptance.hardwareId,
            sizeof(lateConfirmation.hardwareId) - 1);
    strncpy(lateConfirmation.name, firstAcceptance.name,
            sizeof(lateConfirmation.name) - 1);

    fakeMillis += PROVISIONING_TRANSACTION_TIMEOUT_MS;
    main.expirePendingPairing(fakeMillis);
    retryLamp.expirePendingRequest(fakeMillis);
    TEST_ASSERT_FALSE(main.confirmPairing(lateConfirmation));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(PairingState::UNPAIRED),
                          static_cast<int>(retryLamp.state()));
    TEST_ASSERT_EQUAL_UINT8(1, registry.count);
    TEST_ASSERT_EQUAL_UINT32(1, registry.lamps[0].device.id);

    // A fresh announcement permits retry, which consumes a new ID.
    main.discover(retryLamp.announce());
    TEST_ASSERT_TRUE(main.createPairRequest("HW-RETRY", request));
    TEST_ASSERT_TRUE(retryLamp.handlePairRequest(request, rejection));
    ProvisioningPacket secondAcceptance = {};
    TEST_ASSERT_TRUE(main.createPairAcceptance(
        "HW-RETRY", "Retry", secondAcceptance));
    TEST_ASSERT_EQUAL_UINT32(12, secondAcceptance.deviceId);
    TEST_ASSERT_FALSE(main.confirmPairing(lateConfirmation));

    ProvisioningPacket validConfirmation = {};
    TEST_ASSERT_TRUE(retryLamp.handlePairAccept(secondAcceptance, validConfirmation));
    TEST_ASSERT_TRUE(main.confirmPairing(validConfirmation));
    TEST_ASSERT_FALSE(main.confirmPairing(validConfirmation));
    TEST_ASSERT_EQUAL_UINT8(2, registry.count);
    TEST_ASSERT_EQUAL_UINT32(1, registry.lamps[0].device.id);
    TEST_ASSERT_EQUAL_UINT32(12, registry.lamps[1].device.id);
}

void test_repeated_expired_reservations_for_same_identity_do_not_fill_table() {
    constexpr uint32_t MAIN_ID = 48;
    constexpr uint32_t FAILED_ATTEMPTS = 24;
    MemoryProvisioningReservationStore reservations;
    LampRegistry registry;
    initLampRegistry(registry);
    MainProvisioning main(MAIN_ID, registry, &reservations);
    MemoryProvisioningStore lampStorage;
    ProvisioningLamp lamp("HW-RETRY-MANY", "0.4.0", 0, lampStorage);

    TEST_ASSERT_EQUAL_INT(static_cast<int>(DiscoveryResult::DISCOVERED_UNPAIRED),
                          static_cast<int>(main.discover(lamp.announce())));
    main.startCommissioning();

    for (uint32_t attempt = 0; attempt < FAILED_ATTEMPTS; ++attempt) {
        ProvisioningPacket request = {};
        ProvisioningPacket rejection = {};
        ProvisioningPacket acceptance = {};
        TEST_ASSERT_TRUE(main.createPairRequest("HW-RETRY-MANY", request));
        TEST_ASSERT_TRUE(lamp.handlePairRequest(request, rejection));
        TEST_ASSERT_TRUE(main.createPairAcceptance(
            "HW-RETRY-MANY", "Retry", acceptance));
        TEST_ASSERT_EQUAL_UINT32(11 + attempt, acceptance.deviceId);

        fakeMillis += PROVISIONING_TRANSACTION_TIMEOUT_MS;
        main.expirePendingPairing(fakeMillis);
        lamp.expirePendingRequest(fakeMillis);
        TEST_ASSERT_EQUAL_INT(static_cast<int>(PairingState::UNPAIRED),
                              static_cast<int>(lamp.state()));
        TEST_ASSERT_EQUAL_INT(static_cast<int>(DiscoveryResult::DUPLICATE_ANNOUNCEMENT),
                              static_cast<int>(main.discover(lamp.announce())));
    }

    MainProvisioningReservationState state = {};
    TEST_ASSERT_TRUE(reservations.load(MAIN_ID, state));
    TEST_ASSERT_EQUAL_UINT8(1, state.count);
    TEST_ASSERT_EQUAL_UINT32(10 + FAILED_ATTEMPTS, state.highWaterDeviceId);
    TEST_ASSERT_EQUAL_UINT32(10 + FAILED_ATTEMPTS,
                            state.identities[0].deviceId);
    TEST_ASSERT_EQUAL_UINT8(0, registry.count);

    ProvisioningPacket confirmedPair = {};
    TEST_ASSERT_TRUE(completePairing(
        main, lamp, "HW-RETRY-MANY", "Retry", confirmedPair));
    TEST_ASSERT_EQUAL_UINT32(11 + FAILED_ATTEMPTS, confirmedPair.deviceId);
    TEST_ASSERT_EQUAL_UINT8(1, registry.count);
    TEST_ASSERT_EQUAL_UINT32(confirmedPair.deviceId, registry.lamps[0].device.id);
}

void test_invalid_pair_confirm_does_not_change_registry_or_pending_pair() {
    MemoryProvisioningReservationStore reservations;
    LampRegistry registry;
    initLampRegistry(registry);
    MainProvisioning main(44, registry, &reservations);
    MemoryProvisioningStore storage;
    ProvisioningLamp lamp("HW-INVALID-ACK", "0.4.0", 0, storage);
    main.discover(lamp.announce());
    main.startCommissioning();

    ProvisioningPacket request = {};
    ProvisioningPacket rejection = {};
    ProvisioningPacket acceptance = {};
    ProvisioningPacket confirmation = {};
    TEST_ASSERT_TRUE(main.createPairRequest("HW-INVALID-ACK", request));
    TEST_ASSERT_TRUE(lamp.handlePairRequest(request, rejection));
    TEST_ASSERT_TRUE(main.createPairAcceptance(
        "HW-INVALID-ACK", "Test", acceptance));
    TEST_ASSERT_TRUE(lamp.handlePairAccept(acceptance, confirmation));

    ProvisioningPacket invalid = confirmation;
    invalid.role = DeviceRole::MAIN;
    TEST_ASSERT_FALSE(main.confirmPairing(invalid));
    invalid = confirmation;
    invalid.parentMainId += 1;
    TEST_ASSERT_FALSE(main.confirmPairing(invalid));
    invalid = confirmation;
    invalid.deviceId += 1;
    TEST_ASSERT_FALSE(main.confirmPairing(invalid));
    invalid = confirmation;
    strncpy(invalid.hardwareId, "HW-OTHER", sizeof(invalid.hardwareId) - 1);
    TEST_ASSERT_FALSE(main.confirmPairing(invalid));
    invalid = confirmation;
    strncpy(invalid.name, "Other", sizeof(invalid.name) - 1);
    TEST_ASSERT_FALSE(main.confirmPairing(invalid));
    TEST_ASSERT_EQUAL_UINT8(0, registry.count);

    TEST_ASSERT_TRUE(main.confirmPairing(confirmation));
    TEST_ASSERT_FALSE(main.confirmPairing(confirmation));
    TEST_ASSERT_EQUAL_UINT8(1, registry.count);
}

void test_reservation_write_failure_blocks_new_pairing_and_keeps_existing_lamp() {
    MemoryProvisioningReservationStore reservations;
    LampRegistry registry;
    initLampRegistry(registry);
    MainProvisioning main(45, registry, &reservations);
    MemoryProvisioningStore oldStorage;
    savePairedLampRecord(oldStorage, "HW-SAFE", 1, 45);
    ProvisioningLamp oldLamp("HW-SAFE", "0.4.0", 0, oldStorage);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(DiscoveryResult::RESTORED_PAIRED),
                          static_cast<int>(main.discover(oldLamp.announce())));

    MemoryProvisioningStore newStorage;
    ProvisioningLamp newLamp("HW-WRITE-FAIL", "0.4.0", 0, newStorage);
    main.discover(newLamp.announce());
    main.startCommissioning();
    ProvisioningPacket request = {};
    TEST_ASSERT_TRUE(main.createPairRequest("HW-WRITE-FAIL", request));
    reservations.setFailWrites(true);
    ProvisioningPacket acceptance = {};
    TEST_ASSERT_FALSE(main.createPairAcceptance(
        "HW-WRITE-FAIL", "New", acceptance));
    main.abortPendingPairing();
    TEST_ASSERT_FALSE(main.createPairRequest("HW-WRITE-FAIL", request));
    TEST_ASSERT_EQUAL_UINT8(1, registry.count);
    TEST_ASSERT_EQUAL_STRING("HW-SAFE", registry.lamps[0].identity.hardwareId);
    TEST_ASSERT_EQUAL_UINT32(1, registry.lamps[0].device.id);
}

void test_reservation_state_from_another_main_disables_pairing() {
    MemoryProvisioningReservationStore reservations;
    MainProvisioningReservationState wrongOwner = {};
    initializeMainProvisioningReservationState(99, wrongOwner);
    TEST_ASSERT_TRUE(reservations.save(wrongOwner));

    LampRegistry registry;
    initLampRegistry(registry);
    MainProvisioning main(46, registry, &reservations);
    MemoryProvisioningStore storage;
    ProvisioningLamp lamp("HW-OWNER", "0.4.0", 0, storage);
    main.discover(lamp.announce());
    main.startCommissioning();
    ProvisioningPacket request = {};
    TEST_ASSERT_FALSE(main.createPairRequest("HW-OWNER", request));
    TEST_ASSERT_EQUAL_UINT8(0, registry.count);
}

void test_incoherent_reservation_state_fails_closed_without_restoring_lamps() {
    CorruptProvisioningReservationStore reservations;
    LampRegistry registry;
    initLampRegistry(registry);
    MainProvisioning main(47, registry, &reservations);
    MemoryProvisioningStore legacyStorage;
    savePairedLampRecord(legacyStorage, "HW-LEGACY-SAFE", 1, 47);
    ProvisioningLamp legacy("HW-LEGACY-SAFE", "0.4.0", 0, legacyStorage);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(DiscoveryResult::IDENTITY_CONFLICT),
                          static_cast<int>(main.discover(legacy.announce())));

    MemoryProvisioningStore candidateStorage;
    ProvisioningLamp candidate("HW-CANDIDATE", "0.4.0", 0, candidateStorage);
    main.discover(candidate.announce());
    main.startCommissioning();
    ProvisioningPacket request = {};
    TEST_ASSERT_FALSE(main.createPairRequest("HW-CANDIDATE", request));
    TEST_ASSERT_EQUAL_UINT8(0, registry.count);
}

void test_removal_is_transactional_and_repair_gets_a_new_id() {
    constexpr uint32_t MAIN_LOCAL_ID = 88;
    LampRegistry registry;
    initLampRegistry(registry);
    MemoryProvisioningReservationStore reservations;
    MainProvisioning main(MAIN_LOCAL_ID, registry, &reservations);
    MemoryProvisioningStore firstStore;
    MemoryProvisioningStore secondStore;
    ProvisioningLamp first("C6-0000000000000001", "0.4.0", 0, firstStore);
    ProvisioningLamp second("C6-0000000000000002", "0.4.0", 0, secondStore);
    main.discover(first.announce());
    main.discover(second.announce());
    main.startCommissioning();
    ProvisioningPacket firstConfirmation = {};
    ProvisioningPacket secondConfirmation = {};
    TEST_ASSERT_TRUE(completePairing(main, first, "C6-0000000000000001",
                                    "FIRST", firstConfirmation));
    TEST_ASSERT_TRUE(completePairing(main, second, "C6-0000000000000002",
                                    "SECOND", secondConfirmation));
    const ProvisioningPacket oldFirstAnnouncement = first.announce();
    const uint32_t mainCoreId = makeCoreModuleId(CORE_LOGICAL_ID, MAIN_LOCAL_ID);
    const uint32_t firstCoreId = makeCoreModuleId(mainCoreId, firstConfirmation.deviceId);

    const uint32_t operationId = 0x12345678;
    findLamp(registry, firstConfirmation.deviceId)->device.status =
        DeviceStatus::OFFLINE;
    TEST_ASSERT_FALSE(main.beginRemoval(operationId, firstConfirmation.deviceId,
                                        firstCoreId));
    findLamp(registry, firstConfirmation.deviceId)->device.status =
        DeviceStatus::ONLINE;
    TEST_ASSERT_TRUE(main.beginRemoval(operationId, firstConfirmation.deviceId,
                                       firstCoreId));
    TEST_ASSERT_FALSE(main.beginRemoval(operationId + 1,
        secondConfirmation.deviceId,
        makeCoreModuleId(mainCoreId, secondConfirmation.deviceId)));
    TEST_ASSERT_TRUE(findLamp(registry, firstConfirmation.deviceId)
                         ->identity.removalPending);
    TEST_ASSERT_FALSE(findLamp(registry, secondConfirmation.deviceId)
                          ->identity.removalPending);
    TEST_ASSERT_EQUAL_UINT8(2, registry.count);

    ProvisioningPacket request = {};
    TEST_ASSERT_TRUE(main.buildRemovalPacket(request));
    setPacketEnvelope(request, MAIN_LOCAL_ID, firstConfirmation.deviceId);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ProvisioningPacketType::UNPAIR_REQUEST),
                          static_cast<int>(request.type));
    TEST_ASSERT_EQUAL_UINT32(operationId, request.transactionId);
    TEST_ASSERT_EQUAL_UINT32(firstConfirmation.deviceId, request.deviceId);
    TEST_ASSERT_EQUAL_UINT32(MAIN_LOCAL_ID, request.parentMainId);

    ProvisioningPacket confirmation = {};
    ProvisioningPacket wrong = request;
    ++wrong.sourceId;
    TEST_ASSERT_FALSE(first.handleUnpairRequest(wrong, confirmation));
    wrong = request;
    ++wrong.destinationId;
    TEST_ASSERT_FALSE(first.handleUnpairRequest(wrong, confirmation));
    wrong = request;
    strcpy(wrong.hardwareId, "C6-00000000000000FF");
    TEST_ASSERT_FALSE(first.handleUnpairRequest(wrong, confirmation));
    wrong = request;
    ++wrong.deviceId;
    TEST_ASSERT_FALSE(first.handleUnpairRequest(wrong, confirmation));
    wrong = request;
    ++wrong.parentMainId;
    TEST_ASSERT_FALSE(first.handleUnpairRequest(wrong, confirmation));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(PairingState::PAIRED),
                          static_cast<int>(first.state()));

    TEST_ASSERT_TRUE(first.handleUnpairRequest(request, confirmation));
    setPacketEnvelope(confirmation, firstConfirmation.deviceId, MAIN_LOCAL_ID);
    const ProvisioningPacket validConfirmation = confirmation;
    wrong = request;
    ++wrong.transactionId;
    TEST_ASSERT_FALSE(first.handleUnpairRequest(wrong, confirmation));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ProvisioningPacketType::UNPAIR_REJECT),
                          static_cast<int>(confirmation.type));
    confirmation = validConfirmation;
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ProvisioningPacketType::UNPAIR_CONFIRM),
                          static_cast<int>(confirmation.type));
    ProvisioningRecord absentRecord = {};
    TEST_ASSERT_FALSE(firstStore.load(absentRecord));

    ProvisioningPacket commit = {};
    bool completed = false;
    wrong = confirmation;
    ++wrong.sourceId;
    ProvisioningPacket rejectedCommit = {};
    bool rejectedCompleted = false;
    TEST_ASSERT_FALSE(main.reconcileRemovalPacket(
        wrong, rejectedCommit, rejectedCompleted));
    TEST_ASSERT_EQUAL_UINT8(2, registry.count);
    TEST_ASSERT_TRUE(main.reconcileRemovalPacket(confirmation, commit, completed));
    TEST_ASSERT_TRUE(completed);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ProvisioningPacketType::UNPAIR_COMMIT),
                          static_cast<int>(commit.type));
    TEST_ASSERT_EQUAL_UINT8(1, registry.count);
    TEST_ASSERT_NULL(findLamp(registry, firstConfirmation.deviceId));
    TEST_ASSERT_NOT_NULL(findLamp(registry, secondConfirmation.deviceId));
    MainProvisioningReservationState saved = {};
    TEST_ASSERT_TRUE(reservations.load(MAIN_LOCAL_ID, saved));
    TEST_ASSERT_EQUAL_UINT8(1, saved.count);
    TEST_ASSERT_EQUAL_UINT8(MAIN_REMOVAL_SUCCEEDED, saved.removalState);
    setPacketEnvelope(commit, MAIN_LOCAL_ID, firstConfirmation.deviceId);
    wrong = commit;
    ++wrong.sourceId;
    TEST_ASSERT_FALSE(first.handleUnpairCommit(wrong));
    TEST_ASSERT_TRUE(first.handleUnpairCommit(commit));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(PairingState::UNPAIRED),
                          static_cast<int>(first.state()));
    TEST_ASSERT_FALSE(first.handleUnpairCommit(commit));

    TEST_ASSERT_EQUAL_INT(static_cast<int>(DiscoveryResult::IDENTITY_CONFLICT),
        static_cast<int>(main.discover(oldFirstAnnouncement)));
    TEST_ASSERT_EQUAL_UINT8(1, registry.count);

    TEST_ASSERT_EQUAL_INT(static_cast<int>(DiscoveryResult::DUPLICATE_ANNOUNCEMENT),
        static_cast<int>(main.discover(first.announce())));
    main.startCommissioning();
    ProvisioningPacket repairedConfirmation = {};
    TEST_ASSERT_TRUE(completePairing(main, first, "C6-0000000000000001",
                                    "FIRST_AGAIN", repairedConfirmation));
    TEST_ASSERT_TRUE(repairedConfirmation.deviceId > firstConfirmation.deviceId);
    TEST_ASSERT_FALSE(first.handleUnpairCommit(commit));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(PairingState::PAIRED),
                          static_cast<int>(first.state()));
    TEST_ASSERT_EQUAL_UINT8(2, registry.count);
}

void test_lost_removal_confirmation_reconciles_after_main_and_lamp_restart() {
    constexpr uint32_t MAIN_LOCAL_ID = 89;
    MemoryProvisioningReservationStore reservations;
    MemoryProvisioningStore lampStore;
    LampRegistry firstRegistry;
    initLampRegistry(firstRegistry);
    MainProvisioning firstMain(MAIN_LOCAL_ID, firstRegistry, &reservations);
    ProvisioningLamp firstLamp("C6-0000000000000003", "0.4.0", 0, lampStore);
    firstMain.discover(firstLamp.announce());
    firstMain.startCommissioning();
    ProvisioningPacket paired = {};
    TEST_ASSERT_TRUE(completePairing(firstMain, firstLamp,
        "C6-0000000000000003", "LAMP", paired));
    const ProvisioningPacket stalePairedAnnouncement = firstLamp.announce();
    const uint32_t mainCoreId = makeCoreModuleId(CORE_LOGICAL_ID, MAIN_LOCAL_ID);
    const uint32_t lampCoreId = makeCoreModuleId(mainCoreId, paired.deviceId);
    const uint32_t operationId = 0x87654321;
    TEST_ASSERT_TRUE(firstMain.beginRemoval(operationId, paired.deviceId, lampCoreId));
    ProvisioningPacket request = {};
    TEST_ASSERT_TRUE(firstMain.buildRemovalPacket(request));
    setPacketEnvelope(request, MAIN_LOCAL_ID, paired.deviceId);
    ProvisioningPacket lostConfirmation = {};
    TEST_ASSERT_TRUE(firstLamp.handleUnpairRequest(request, lostConfirmation));
    setPacketEnvelope(lostConfirmation, paired.deviceId, MAIN_LOCAL_ID);

    ProvisioningLamp restartedLamp("C6-0000000000000003", "0.4.0", 0, lampStore);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(PairingState::UNPAIRING),
                          static_cast<int>(restartedLamp.state()));
    LampRegistry restartedRegistry;
    initLampRegistry(restartedRegistry);
    MainProvisioning restartedMain(MAIN_LOCAL_ID, restartedRegistry, &reservations);
    ProvisioningPacket evidence = restartedLamp.announce();
    setPacketEnvelope(evidence, paired.deviceId, 0);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(PairingState::UNPAIRING),
                          static_cast<int>(evidence.pairingState));
    TEST_ASSERT_EQUAL_UINT32(operationId, evidence.transactionId);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(DiscoveryResult::REMOVAL_PENDING),
        static_cast<int>(restartedMain.discover(evidence)));

    ProvisioningPacket commit = {};
    bool completed = false;
    TEST_ASSERT_TRUE(restartedMain.reconcileRemovalPacket(evidence, commit, completed));
    TEST_ASSERT_TRUE(completed);
    TEST_ASSERT_EQUAL_UINT8(0, restartedRegistry.count);
    setPacketEnvelope(commit, MAIN_LOCAL_ID, paired.deviceId);
    TEST_ASSERT_TRUE(restartedLamp.handleUnpairCommit(commit));

    ProvisioningPacket duplicateCommit = {};
    bool duplicateCompleted = false;
    TEST_ASSERT_TRUE(restartedMain.reconcileRemovalPacket(
        lostConfirmation, duplicateCommit, duplicateCompleted));
    TEST_ASSERT_TRUE(duplicateCompleted);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ProvisioningPacketType::UNPAIR_COMMIT),
                          static_cast<int>(duplicateCommit.type));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(DiscoveryResult::IDENTITY_CONFLICT),
        static_cast<int>(restartedMain.discover(stalePairedAnnouncement)));
    TEST_ASSERT_EQUAL_UINT8(0, restartedRegistry.count);
}

void test_lamp_clear_failure_keeps_binding_and_main_reservation() {
    constexpr uint32_t MAIN_LOCAL_ID = 90;
    LampRegistry registry;
    initLampRegistry(registry);
    MemoryProvisioningReservationStore reservations;
    MemoryProvisioningStore lampStore;
    MainProvisioning main(MAIN_LOCAL_ID, registry, &reservations);
    ProvisioningLamp lamp("C6-0000000000000004", "0.4.0", 0, lampStore);
    main.discover(lamp.announce());
    main.startCommissioning();
    ProvisioningPacket paired = {};
    TEST_ASSERT_TRUE(completePairing(main, lamp, "C6-0000000000000004",
                                    "LAMP", paired));
    const uint32_t mainCoreId = makeCoreModuleId(CORE_LOGICAL_ID, MAIN_LOCAL_ID);
    const uint32_t lampCoreId = makeCoreModuleId(mainCoreId, paired.deviceId);
    TEST_ASSERT_TRUE(main.beginRemoval(0x11112222, paired.deviceId, lampCoreId));
    ProvisioningPacket request = {};
    TEST_ASSERT_TRUE(main.buildRemovalPacket(request));
    setPacketEnvelope(request, MAIN_LOCAL_ID, paired.deviceId);
    lampStore.setFailWrites(true);
    ProvisioningPacket rejection = {};
    TEST_ASSERT_FALSE(lamp.handleUnpairRequest(request, rejection));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ProvisioningPacketType::UNPAIR_REJECT),
                          static_cast<int>(rejection.type));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(PairingState::PAIRED),
                          static_cast<int>(lamp.state()));
    TEST_ASSERT_EQUAL_UINT8(1, registry.count);

    ProvisioningPacket ignored = {};
    bool completed = false;
    setPacketEnvelope(rejection, paired.deviceId, MAIN_LOCAL_ID);
    TEST_ASSERT_TRUE(main.reconcileRemovalPacket(rejection, ignored, completed));
    TEST_ASSERT_FALSE(completed);
    MainProvisioningReservationState saved = {};
    TEST_ASSERT_TRUE(reservations.load(MAIN_LOCAL_ID, saved));
    TEST_ASSERT_EQUAL_UINT8(MAIN_REMOVAL_FAILED, saved.removalState);
    TEST_ASSERT_EQUAL_UINT8(1, saved.count);
}

void test_main_write_failure_does_not_start_lamp_removal() {
    constexpr uint32_t MAIN_LOCAL_ID = 91;
    LampRegistry registry;
    initLampRegistry(registry);
    MemoryProvisioningReservationStore reservations;
    MainProvisioning main(MAIN_LOCAL_ID, registry, &reservations);
    MemoryProvisioningStore lampStore;
    ProvisioningLamp lamp("C6-0000000000000005", "0.4.0", 0, lampStore);
    main.discover(lamp.announce());
    main.startCommissioning();
    ProvisioningPacket paired = {};
    TEST_ASSERT_TRUE(completePairing(main, lamp, "C6-0000000000000005",
                                    "LAMP", paired));
    const uint32_t mainCoreId = makeCoreModuleId(CORE_LOGICAL_ID, MAIN_LOCAL_ID);
    const uint32_t lampCoreId = makeCoreModuleId(mainCoreId, paired.deviceId);
    reservations.setFailWrites(true);
    TEST_ASSERT_FALSE(main.beginRemoval(0x33334444, paired.deviceId, lampCoreId));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(PairingState::PAIRED),
                          static_cast<int>(lamp.state()));
    TEST_ASSERT_EQUAL_UINT8(1, registry.count);
}

void test_v76_reservation_blob_migration_is_explicit_and_validated() {
    MainProvisioningReservationStateV1 legacy = {};
    legacy.version = 1;
    legacy.count = 1;
    legacy.mainId = 92;
    legacy.highWaterDeviceId = 11;
    legacy.identities[0].deviceId = 11;
    strcpy(legacy.identities[0].hardwareId, "C6-0000000000000006");
    MainProvisioningReservationState migrated = {};
    TEST_ASSERT_TRUE(migrateMainProvisioningReservationStateV1(
        legacy, 92, migrated));
    TEST_ASSERT_EQUAL_UINT8(MAIN_PROVISIONING_STATE_VERSION, migrated.version);
    TEST_ASSERT_EQUAL_UINT8(1, migrated.count);
    TEST_ASSERT_EQUAL_UINT32(11, migrated.highWaterDeviceId);
    TEST_ASSERT_EQUAL_STRING(legacy.identities[0].hardwareId,
                             migrated.identities[0].hardwareId);
    legacy.reserved = 1;
    TEST_ASSERT_FALSE(migrateMainProvisioningReservationStateV1(
        legacy, 92, migrated));
}

void test_removal_of_legacy_id_sets_tombstone_and_rejects_late_announce() {
    constexpr uint32_t MAIN_LOCAL_ID = 93;
    LampRegistry registry;
    initLampRegistry(registry);
    MemoryProvisioningReservationStore reservations;
    MainProvisioning main(MAIN_LOCAL_ID, registry, &reservations);
    MemoryProvisioningStore lampStore;
    savePairedLampRecord(lampStore, "C6-0000000000000007", 1, MAIN_LOCAL_ID);
    ProvisioningLamp lamp("C6-0000000000000007", "0.4.0", 0, lampStore);
    const ProvisioningPacket oldAnnouncement = lamp.announce();
    TEST_ASSERT_EQUAL_INT(static_cast<int>(DiscoveryResult::RESTORED_PAIRED),
        static_cast<int>(main.discover(oldAnnouncement)));

    const uint32_t mainCoreId = makeCoreModuleId(CORE_LOGICAL_ID, MAIN_LOCAL_ID);
    const uint32_t lampCoreId = makeCoreModuleId(mainCoreId, 1);
    TEST_ASSERT_TRUE(main.beginRemoval(0x45454545, 1, lampCoreId));
    ProvisioningPacket request = {};
    TEST_ASSERT_TRUE(main.buildRemovalPacket(request));
    setPacketEnvelope(request, MAIN_LOCAL_ID, 1);
    ProvisioningPacket confirmation = {};
    TEST_ASSERT_TRUE(lamp.handleUnpairRequest(request, confirmation));
    setPacketEnvelope(confirmation, 1, MAIN_LOCAL_ID);
    ProvisioningPacket commit = {};
    bool completed = false;
    TEST_ASSERT_TRUE(main.reconcileRemovalPacket(confirmation, commit, completed));
    TEST_ASSERT_TRUE(completed);
    setPacketEnvelope(commit, MAIN_LOCAL_ID, 1);
    MainProvisioningReservationState state = {};
    TEST_ASSERT_TRUE(reservations.load(MAIN_LOCAL_ID, state));
    TEST_ASSERT_EQUAL_UINT16(1, state.retiredLegacyIds & 1U);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(DiscoveryResult::IDENTITY_CONFLICT),
        static_cast<int>(main.discover(oldAnnouncement)));
    TEST_ASSERT_EQUAL_UINT8(0, registry.count);
}

void test_pairing_capacity_is_rejected_before_new_reservation() {
    MemoryProvisioningStore candidateStorage;
    ProvisioningLamp candidate("HW-CAPACITY-CANDIDATE", "0.4.0", 0,
                               candidateStorage);
    ProvisioningPacket request = {};

    MemoryProvisioningReservationStore fullReservationStore;
    MainProvisioningReservationState fullReservations = {};
    initializeMainProvisioningReservationState(70, fullReservations);
    fullReservations.count = MAX_LAMPS;
    for (uint8_t index = 0; index < MAX_LAMPS; ++index) {
        fullReservations.identities[index].deviceId = index + 1;
        snprintf(fullReservations.identities[index].hardwareId,
                 sizeof(fullReservations.identities[index].hardwareId),
                 "HW-RESERVED-%02u", static_cast<unsigned>(index));
    }
    TEST_ASSERT_TRUE(fullReservationStore.save(fullReservations));
    LampRegistry reservationLimitedRegistry;
    initLampRegistry(reservationLimitedRegistry);
    MainProvisioning reservationLimitedMain(70, reservationLimitedRegistry,
                                             &fullReservationStore);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(DiscoveryResult::DISCOVERED_UNPAIRED),
        static_cast<int>(reservationLimitedMain.discover(candidate.announce())));
    reservationLimitedMain.startCommissioning();
    TEST_ASSERT_FALSE(reservationLimitedMain.canPairCandidate(
        "HW-CAPACITY-CANDIDATE"));
    TEST_ASSERT_FALSE(reservationLimitedMain.createPairRequest(
        "HW-CAPACITY-CANDIDATE", request));
    MainProvisioningReservationState preserved = {};
    TEST_ASSERT_TRUE(fullReservationStore.load(70, preserved));
    TEST_ASSERT_EQUAL_UINT8(MAX_LAMPS, preserved.count);
    TEST_ASSERT_EQUAL_UINT32(MAX_LAMPS, preserved.highWaterDeviceId);

    MemoryProvisioningReservationStore lampLimitedStore;
    LampRegistry fullRegistry;
    initLampRegistry(fullRegistry);
    MainProvisioning lampLimitedMain(71, fullRegistry, &lampLimitedStore);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(DiscoveryResult::DISCOVERED_UNPAIRED),
        static_cast<int>(lampLimitedMain.discover(candidate.announce())));
    lampLimitedMain.startCommissioning();
    fullRegistry.count = MAX_LAMPS;
    TEST_ASSERT_FALSE(lampLimitedMain.canPairCandidate("HW-CAPACITY-CANDIDATE"));
    TEST_ASSERT_FALSE(lampLimitedMain.createPairRequest(
        "HW-CAPACITY-CANDIDATE", request));
    TEST_ASSERT_TRUE(lampLimitedStore.load(71, preserved));
    TEST_ASSERT_EQUAL_UINT8(0, preserved.count);
    TEST_ASSERT_EQUAL_UINT32(MAX_LAMPS, preserved.highWaterDeviceId);
}

void test_unpaired_candidate_expires_without_changing_paired_registry() {
    MemoryProvisioningReservationStore reservations;
    LampRegistry registry;
    initLampRegistry(registry);
    MainProvisioning main(72, registry, &reservations);
    MemoryProvisioningStore candidateStorage;
    MemoryProvisioningStore pairedStorage;
    ProvisioningLamp candidate("HW-TTL-CANDIDATE", "0.4.0", 0,
                               candidateStorage);
    ProvisioningLamp existing("HW-TTL-PAIRED", "0.4.0", 0,
                               pairedStorage);
    main.discover(existing.announce());
    main.startCommissioning();
    ProvisioningPacket request = {};
    ProvisioningPacket rejection = {};
    TEST_ASSERT_TRUE(main.createPairRequest("HW-TTL-PAIRED", request));
    TEST_ASSERT_TRUE(existing.handlePairRequest(request, rejection));
    ProvisioningPacket acceptance = {};
    TEST_ASSERT_TRUE(main.createPairAcceptance("HW-TTL-PAIRED", "existing",
                                                acceptance));
    ProvisioningPacket confirmation = {};
    TEST_ASSERT_TRUE(existing.handlePairAccept(acceptance, confirmation));
    TEST_ASSERT_TRUE(main.confirmPairing(confirmation));

    TEST_ASSERT_EQUAL_INT(static_cast<int>(DiscoveryResult::DISCOVERED_UNPAIRED),
        static_cast<int>(main.discover(candidate.announce())));
    TEST_ASSERT_TRUE(main.hasUnpairedCandidate("HW-TTL-CANDIDATE"));
    main.expireUnpairedCandidates(PROVISIONING_TRANSACTION_TIMEOUT_MS);
    TEST_ASSERT_TRUE(main.hasUnpairedCandidate("HW-TTL-CANDIDATE"));
    main.expireUnpairedCandidates(PROVISIONING_TRANSACTION_TIMEOUT_MS + 1);
    TEST_ASSERT_FALSE(main.hasUnpairedCandidate("HW-TTL-CANDIDATE"));
    TEST_ASSERT_EQUAL_UINT8(1, registry.count);
    TEST_ASSERT_EQUAL_STRING("HW-TTL-PAIRED", registry.lamps[0].identity.hardwareId);
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
    RUN_TEST(test_upgrade_reserves_legacy_ids_while_old_lamp_is_absent);
    RUN_TEST(test_main_restart_keeps_id_reserved_while_lamp_is_absent);
    RUN_TEST(test_conflicting_legacy_identity_is_rejected_without_replacing_first);
    RUN_TEST(test_expired_pairing_releases_lamp_and_rejects_late_confirmation);
    RUN_TEST(test_repeated_expired_reservations_for_same_identity_do_not_fill_table);
    RUN_TEST(test_invalid_pair_confirm_does_not_change_registry_or_pending_pair);
    RUN_TEST(test_reservation_write_failure_blocks_new_pairing_and_keeps_existing_lamp);
    RUN_TEST(test_reservation_state_from_another_main_disables_pairing);
    RUN_TEST(test_incoherent_reservation_state_fails_closed_without_restoring_lamps);
    RUN_TEST(test_pairing_capacity_is_rejected_before_new_reservation);
    RUN_TEST(test_unpaired_candidate_expires_without_changing_paired_registry);
    RUN_TEST(test_removal_is_transactional_and_repair_gets_a_new_id);
    RUN_TEST(test_lost_removal_confirmation_reconciles_after_main_and_lamp_restart);
    RUN_TEST(test_lamp_clear_failure_keeps_binding_and_main_reservation);
    RUN_TEST(test_main_write_failure_does_not_start_lamp_removal);
    RUN_TEST(test_v76_reservation_blob_migration_is_explicit_and_validated);
    RUN_TEST(test_removal_of_legacy_id_sets_tombstone_and_rejects_late_announce);
    return UNITY_END();
}
