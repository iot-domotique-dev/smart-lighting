#include <string.h>

#include <unity.h>

#include "core_link_codec.h"
#include "core_link_service.h"
#include "core_module_service.h"
#include "core_topology_protocol.h"
#include "communication.h"
#include "device_registry.h"

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

void setUp() {}
void tearDown() {}

static size_t encode(
    const CoreLinkPacket& packet,
    uint8_t* bytes,
    size_t capacity
) {
    size_t length = 0;
    if (!encodeCoreLinkFrame(packet, bytes, capacity, length)) return 0;
    return length;
}

static void initRegistryWithCoreRoot(DeviceRegistry& registry) {
    initDeviceRegistry(registry);
    Device root = {};
    root.id = CORE_LOGICAL_ID;
    root.name = "CORE";
    root.role = DeviceRole::CORE;
    root.status = DeviceStatus::ONLINE;
    root.localId = CORE_LOGICAL_ID;
    TEST_ASSERT_TRUE(registerDevice(registry, root));
}

void test_uart_announcement_frame_round_trip() {
    CoreLinkPacket input = {};
    input.sequence = 0x1234;
    input.type = CoreLinkMessageType::MODULE_ANNOUNCEMENT;
    input.localId = 0x10203040;
    input.parentId = CORE_LOGICAL_ID;
    input.capabilities = DEVICE_CAP_LIGHTING | DEVICE_CAP_POWER;
    input.role = static_cast<uint8_t>(DeviceRole::MAIN);
    strncpy(input.name, "MAIN_LIGHTING", sizeof(input.name) - 1);

    uint8_t bytes[CORE_LINK_MAX_FRAME_SIZE] = {};
    const size_t length = encode(input, bytes, sizeof(bytes));
    TEST_ASSERT_GREATER_THAN_UINT(0, length);
    CoreLinkPacket output = {};
    TEST_ASSERT_TRUE(decodeCoreLinkFrame(bytes, length, output));
    TEST_ASSERT_EQUAL_UINT16(input.sequence, output.sequence);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(input.type),
                            static_cast<uint8_t>(output.type));
    TEST_ASSERT_EQUAL_UINT32(input.localId, output.localId);
    TEST_ASSERT_EQUAL_UINT32(input.parentId, output.parentId);
    TEST_ASSERT_EQUAL_UINT32(input.capabilities, output.capabilities);
    TEST_ASSERT_EQUAL_UINT8(input.role, output.role);
    TEST_ASSERT_EQUAL_STRING(input.name, output.name);
}

void test_uart_codec_checks_exact_length_version_and_crc() {
    CoreLinkPacket input = {};
    input.sequence = 7;
    input.type = CoreLinkMessageType::ID_ASSIGNMENT;
    input.localId = 99;
    input.coreId = 0x92345678;
    input.resultCode = static_cast<uint8_t>(CoreModuleUpdateResult::REGISTERED);

    uint8_t bytes[CORE_LINK_MAX_FRAME_SIZE] = {};
    const size_t length = encode(input, bytes, sizeof(bytes));
    TEST_ASSERT_GREATER_THAN_UINT(0, length);
    CoreLinkPacket output = {};

    TEST_ASSERT_FALSE(decodeCoreLinkFrame(bytes, length - 1, output));
    TEST_ASSERT_FALSE(decodeCoreLinkFrame(bytes, length + 1, output));

    uint8_t altered[CORE_LINK_MAX_FRAME_SIZE] = {};
    memcpy(altered, bytes, length);
    altered[2] = CORE_LINK_PROTOCOL_VERSION + 1;
    TEST_ASSERT_FALSE(decodeCoreLinkFrame(altered, length, output));

    memcpy(altered, bytes, length);
    altered[CORE_LINK_HEADER_SIZE] ^= 0x01;
    TEST_ASSERT_FALSE(decodeCoreLinkFrame(altered, length, output));

    memcpy(altered, bytes, length);
    altered[6] = 0xFF;
    altered[7] = 0x7F;
    TEST_ASSERT_FALSE(decodeCoreLinkFrame(altered, length, output));
}

void test_uart_stream_decoder_accepts_partial_and_concatenated_frames() {
    CoreLinkPacket first = {};
    first.sequence = 1;
    first.type = CoreLinkMessageType::HELLO;
    first.coreId = CORE_LOGICAL_ID;
    CoreLinkPacket second = {};
    second.sequence = 2;
    second.type = CoreLinkMessageType::ACK;
    second.resultCode = 0;

    uint8_t firstBytes[CORE_LINK_MAX_FRAME_SIZE] = {};
    uint8_t secondBytes[CORE_LINK_MAX_FRAME_SIZE] = {};
    const size_t firstLength = encode(first, firstBytes, sizeof(firstBytes));
    const size_t secondLength = encode(second, secondBytes, sizeof(secondBytes));
    TEST_ASSERT_GREATER_THAN_UINT(0, firstLength);
    TEST_ASSERT_GREATER_THAN_UINT(0, secondLength);

    CoreLinkFrameDecoder decoder;
    CoreLinkPacket output = {};
    for (size_t i = 0; i < firstLength; ++i) {
        const bool ready = decoder.pushByte(firstBytes[i], output);
        TEST_ASSERT_EQUAL(i + 1 == firstLength, ready);
    }
    TEST_ASSERT_EQUAL_UINT16(first.sequence, output.sequence);

    for (size_t i = 0; i < secondLength; ++i) {
        const bool ready = decoder.pushByte(secondBytes[i], output);
        TEST_ASSERT_EQUAL(i + 1 == secondLength, ready);
    }
    TEST_ASSERT_EQUAL_UINT16(second.sequence, output.sequence);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(CoreLinkMessageType::ACK),
                            static_cast<uint8_t>(output.type));
}

void test_main_announcement_crosses_uart_and_returns_assigned_id() {
    const uint32_t mainLocalId = 0x12345678;
    Message mainAnnouncement = {};
    TEST_ASSERT_TRUE(makeCoreMainAnnouncement(
        mainLocalId,
        "MAIN_LIGHTING",
        DEVICE_CAP_LIGHTING | DEVICE_CAP_GROUPS,
        mainAnnouncement
    ));
    mainAnnouncement.id = 0x10001;

    // C6-ZIGBEE converts the received Zigbee announcement into a UART packet.
    CoreLinkPacket uartAnnouncement = {};
    TEST_ASSERT_TRUE(coreAnnouncementFromZigbeeMessage(
        mainAnnouncement,
        0,
        0,
        uartAnnouncement
    ));
    uint8_t bytes[CORE_LINK_MAX_FRAME_SIZE] = {};
    size_t length = encode(uartAnnouncement, bytes, sizeof(bytes));
    TEST_ASSERT_GREATER_THAN_UINT(0, length);

    // C6-WIFI decodes the UART frame, registers the MAIN, and assigns its ID.
    CoreLinkPacket decodedAnnouncement = {};
    TEST_ASSERT_TRUE(decodeCoreLinkFrame(bytes, length, decodedAnnouncement));
    DeviceRegistry registry;
    initRegistryWithCoreRoot(registry);
    CoreLinkPacket uartAssignment = {};
    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(CoreModuleUpdateResult::REGISTERED),
        static_cast<int>(processCoreLinkAnnouncement(
            registry, decodedAnnouncement, millis(), uartAssignment))
    );
    TEST_ASSERT_EQUAL_UINT32(2, registry.count);
    TEST_ASSERT_EQUAL_UINT32(
        makeCoreModuleId(CORE_LOGICAL_ID, mainLocalId),
        uartAssignment.coreId
    );
    length = encode(uartAssignment, bytes, sizeof(bytes));

    // The assignment returns over UART and is converted into a Zigbee response.
    CoreLinkPacket decodedAssignment = {};
    TEST_ASSERT_TRUE(decodeCoreLinkFrame(bytes, length, decodedAssignment));
    Message zigbeeAssignment = {};
    TEST_ASSERT_TRUE(makeCoreIdAssignedMessage(
        mainLocalId,
        decodedAssignment.coreId,
        decodedAssignment.resultCode,
        zigbeeAssignment
    ));
    uint32_t assignedId = 0;
    uint8_t resultCode = 0;
    TEST_ASSERT_TRUE(readCoreIdAssignedMessage(
        zigbeeAssignment,
        mainLocalId,
        assignedId,
        resultCode
    ));
    TEST_ASSERT_EQUAL_UINT32(decodedAssignment.coreId, assignedId);
    TEST_ASSERT_EQUAL_UINT8(
        static_cast<uint8_t>(CoreModuleUpdateResult::REGISTERED),
        resultCode
    );

    Message acknowledgement = {};
    TEST_ASSERT_TRUE(makeCoreIdAckMessage(mainLocalId, assignedId, true,
                                          acknowledgement));
    bool accepted = false;
    TEST_ASSERT_TRUE(readCoreIdAckMessage(acknowledgement, mainLocalId,
                                          assignedId, accepted));
    TEST_ASSERT_TRUE(accepted);
}

void test_repeated_announcement_updates_without_duplicate_and_conflict_is_rejected() {
    const uint32_t mainLocalId = 0x12345678;
    DeviceRegistry registry;
    initRegistryWithCoreRoot(registry);
    CoreLinkPacket announcement = {};
    announcement.sequence = 1;
    announcement.type = CoreLinkMessageType::MODULE_ANNOUNCEMENT;
    announcement.localId = mainLocalId;
    announcement.parentId = CORE_LOGICAL_ID;
    announcement.role = static_cast<uint8_t>(DeviceRole::MAIN);
    strncpy(announcement.name, "MAIN_LIGHTING", sizeof(announcement.name) - 1);

    CoreLinkPacket response = {};
    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(CoreModuleUpdateResult::REGISTERED),
        static_cast<int>(processCoreLinkAnnouncement(
            registry, announcement, 100, response))
    );
    announcement.sequence = 2;
    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(CoreModuleUpdateResult::UPDATED),
        static_cast<int>(processCoreLinkAnnouncement(
            registry, announcement, 200, response))
    );
    TEST_ASSERT_EQUAL_UINT32(2, registry.count);
    TEST_ASSERT_EQUAL_UINT32(200,
        findDeviceById(registry, response.coreId)->lastSeen);
    const uint32_t mainCoreId = response.coreId;

    announcement.role = static_cast<uint8_t>(DeviceRole::RELAY);
    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(CoreModuleUpdateResult::IDENTITY_CONFLICT),
        static_cast<int>(processCoreLinkAnnouncement(
            registry, announcement, 300, response))
    );
    TEST_ASSERT_EQUAL_UINT32(2, registry.count);
    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(DeviceRole::MAIN),
        static_cast<int>(findDeviceById(registry, mainCoreId)->role)
    );
}

void test_paired_lamp_announcement_uses_core_assigned_main_parent_id() {
    const uint32_t mainLocalId = 0x12345678;
    const uint32_t mainCoreId = makeCoreModuleId(CORE_LOGICAL_ID, mainLocalId);
    Message lampAnnouncement = {};
    lampAnnouncement.type = MessageType::DEVICE_ANNOUNCE;
    lampAnnouncement.id = 77;
    lampAnnouncement.sourceId = 17;
    lampAnnouncement.deviceRole = static_cast<uint8_t>(DeviceRole::LAMP);
    lampAnnouncement.pairingState = static_cast<uint8_t>(PairingState::PAIRED);
    lampAnnouncement.provisioningDeviceId = 17;
    lampAnnouncement.parentMainId = mainLocalId;
    lampAnnouncement.capabilities = DEVICE_CAP_POWER | DEVICE_CAP_LIGHTING;
    strncpy(lampAnnouncement.name, "LAMP_C6", sizeof(lampAnnouncement.name) - 1);

    CoreLinkPacket packet = {};
    TEST_ASSERT_TRUE(coreAnnouncementFromZigbeeMessage(
        lampAnnouncement, mainCoreId, mainLocalId, packet));
    TEST_ASSERT_EQUAL_UINT32(mainCoreId, packet.parentId);
    TEST_ASSERT_EQUAL_UINT32(17, packet.localId);

    DeviceRegistry registry;
    initRegistryWithCoreRoot(registry);
    Device main = {};
    main.id = mainCoreId;
    main.name = "MAIN_LIGHTING";
    main.role = DeviceRole::MAIN;
    main.status = DeviceStatus::ONLINE;
    main.parentId = CORE_LOGICAL_ID;
    main.localId = mainLocalId;
    TEST_ASSERT_TRUE(registerDevice(registry, main));

    CoreLinkPacket response = {};
    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(CoreModuleUpdateResult::REGISTERED),
        static_cast<int>(processCoreLinkAnnouncement(
            registry, packet, 400, response))
    );
    TEST_ASSERT_EQUAL_UINT32(mainCoreId, findDeviceById(
        registry, response.coreId)->parentId);
}

void test_lamp_reannouncement_preserves_state_only_for_same_v7_identity() {
    const uint32_t mainLocalId = 0x12345678;
    const uint32_t mainId = makeCoreModuleId(CORE_LOGICAL_ID, mainLocalId);
    DeviceRegistry registry;
    initRegistryWithCoreRoot(registry);

    CoreModuleAnnouncement main = {
        mainLocalId, CORE_LOGICAL_ID, "MAIN_LIGHTING", DeviceRole::MAIN,
        DEVICE_CAP_LIGHTING
    };
    TEST_ASSERT_EQUAL_INT(static_cast<int>(CoreModuleUpdateResult::REGISTERED),
        static_cast<int>(ingestCoreModuleAnnouncement(registry, main, 100)));

    CoreModuleAnnouncement lampAnnouncement = {
        17, mainId, "LAMP_C6", DeviceRole::LAMP, DEVICE_CAP_POWER
    };
    TEST_ASSERT_EQUAL_INT(static_cast<int>(CoreModuleUpdateResult::REGISTERED),
        static_cast<int>(ingestCoreModuleAnnouncement(registry, lampAnnouncement, 200)));
    const uint32_t lampId = makeCoreModuleId(mainId, lampAnnouncement.localId);
    Device* lamp = findDeviceById(registry, lampId);
    TEST_ASSERT_NOT_NULL(lamp);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(LastConfirmedPower::UNKNOWN),
                          static_cast<int>(lamp->lastConfirmedPower));
    lamp->lastConfirmedPower = LastConfirmedPower::ON;
    lamp->lastConfirmedPowerStatus = LastConfirmedPowerStatus::CONFIRMED;

    TEST_ASSERT_EQUAL_INT(static_cast<int>(CoreModuleUpdateResult::UPDATED),
        static_cast<int>(ingestCoreModuleAnnouncement(registry, lampAnnouncement, 300)));
    lamp = findDeviceById(registry, lampId);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(LastConfirmedPower::ON),
                          static_cast<int>(lamp->lastConfirmedPower));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(LastConfirmedPowerStatus::CONFIRMED),
                          static_cast<int>(lamp->lastConfirmedPowerStatus));

    lampAnnouncement.localId = 18;
    TEST_ASSERT_EQUAL_INT(static_cast<int>(CoreModuleUpdateResult::REGISTERED),
        static_cast<int>(ingestCoreModuleAnnouncement(registry, lampAnnouncement, 400)));
    Device* replacement = findDeviceById(
        registry, makeCoreModuleId(mainId, lampAnnouncement.localId));
    TEST_ASSERT_NOT_NULL(replacement);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(LastConfirmedPower::UNKNOWN),
                          static_cast<int>(replacement->lastConfirmedPower));
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_uart_announcement_frame_round_trip);
    RUN_TEST(test_uart_codec_checks_exact_length_version_and_crc);
    RUN_TEST(test_uart_stream_decoder_accepts_partial_and_concatenated_frames);
    RUN_TEST(test_main_announcement_crosses_uart_and_returns_assigned_id);
    RUN_TEST(test_repeated_announcement_updates_without_duplicate_and_conflict_is_rejected);
    RUN_TEST(test_paired_lamp_announcement_uses_core_assigned_main_parent_id);
    RUN_TEST(test_lamp_reannouncement_preserves_state_only_for_same_v7_identity);
    return UNITY_END();
}
