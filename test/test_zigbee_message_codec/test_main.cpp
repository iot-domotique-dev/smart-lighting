#include <string.h>

#include <unity.h>

#include "communication.h"
#include "provisioning.h"
#include "zigbee_message_codec.h"

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

void setUp() {}
void tearDown() {}

static void assertProvisioningWireEnvelope(
    const ProvisioningPacket& packet, uint32_t sourceId, uint32_t destinationId
) {
    Message applicationMessage = {};
    TEST_ASSERT_TRUE(encodeProvisioningPacket(packet, 101, sourceId,
        destinationId, applicationMessage));
    uint8_t bytes[ZIGBEE_MESSAGE_MAX_ENCODED_SIZE] = {};
    size_t length = 0;
    TEST_ASSERT_TRUE(encodeZigbeeMessage(applicationMessage, bytes,
        sizeof(bytes), length));
    Message receivedMessage = {};
    TEST_ASSERT_TRUE(decodeZigbeeMessage(bytes, length, receivedMessage));
    ProvisioningPacket decoded = {};
    TEST_ASSERT_TRUE(decodeProvisioningPacket(receivedMessage, decoded));
    TEST_ASSERT_EQUAL_UINT32(sourceId, decoded.sourceId);
    TEST_ASSERT_EQUAL_UINT32(destinationId, decoded.destinationId);
    TEST_ASSERT_EQUAL_UINT32(packet.transactionId, decoded.transactionId);
    TEST_ASSERT_EQUAL_UINT32(packet.deviceId, decoded.deviceId);
}

void test_command_round_trip_uses_stable_wire_format() {
    Message input = {};
    input.id = 0x12345678;
    input.sourceId = 17;
    input.destinationId = 3;
    input.type = MessageType::COMMAND;
    input.timestamp = 991;
    input.commandType = -7;
    input.value = 1;
    input.value2 = -100;
    input.status = MessageStatus::SENT;
    input.executionStatus = ExecutionStatus::NOT_EXECUTED;

    uint8_t bytes[ZIGBEE_MESSAGE_MAX_ENCODED_SIZE] = {};
    size_t length = 0;
    TEST_ASSERT_TRUE(encodeZigbeeMessage(input, bytes, sizeof(bytes), length));
    TEST_ASSERT_TRUE(length > 0);
    TEST_ASSERT_EQUAL_UINT8('S', bytes[0]);
    TEST_ASSERT_EQUAL_UINT8('L', bytes[1]);
    TEST_ASSERT_EQUAL_UINT8(1, bytes[2]);

    Message output = {};
    TEST_ASSERT_TRUE(decodeZigbeeMessage(bytes, length, output));
    TEST_ASSERT_EQUAL_UINT32(input.id, output.id);
    TEST_ASSERT_EQUAL_UINT32(input.sourceId, output.sourceId);
    TEST_ASSERT_EQUAL_UINT32(input.destinationId, output.destinationId);
    TEST_ASSERT_EQUAL_INT(input.commandType, output.commandType);
    TEST_ASSERT_EQUAL_INT(input.value, output.value);
    TEST_ASSERT_EQUAL_INT(input.value2, output.value2);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(input.type),
                            static_cast<uint8_t>(output.type));
}

void test_provisioning_payload_round_trip() {
    Message input = {};
    input.type = MessageType::DEVICE_ANNOUNCE;
    input.status = MessageStatus::PENDING;
    input.executionStatus = ExecutionStatus::NOT_EXECUTED;
    input.parentMainId = 0x11223344;
    input.capabilities = 0x55aa;
    input.provisioningDeviceId = 9;
    input.deviceRole = 2;
    input.pairingState = 1;
    strcpy(input.hardwareId, "C6-0011223344556677");
    strcpy(input.name, "LAMP01");
    strcpy(input.firmwareVersion, "5.1.0");

    uint8_t bytes[ZIGBEE_MESSAGE_MAX_ENCODED_SIZE] = {};
    size_t length = 0;
    TEST_ASSERT_TRUE(encodeZigbeeMessage(input, bytes, sizeof(bytes), length));
    Message output = {};
    TEST_ASSERT_TRUE(decodeZigbeeMessage(bytes, length, output));
    TEST_ASSERT_EQUAL_STRING(input.hardwareId, output.hardwareId);
    TEST_ASSERT_EQUAL_STRING(input.name, output.name);
    TEST_ASSERT_EQUAL_STRING(input.firmwareVersion, output.firmwareVersion);
    TEST_ASSERT_EQUAL_UINT32(input.parentMainId, output.parentMainId);
    TEST_ASSERT_EQUAL_UINT32(input.capabilities, output.capabilities);
    TEST_ASSERT_EQUAL_UINT32(input.provisioningDeviceId,
                             output.provisioningDeviceId);
}

void test_decoder_rejects_truncated_and_unknown_version_frames() {
    Message input = {};
    input.type = MessageType::HEARTBEAT;
    uint8_t bytes[ZIGBEE_MESSAGE_MAX_ENCODED_SIZE] = {};
    size_t length = 0;
    TEST_ASSERT_TRUE(encodeZigbeeMessage(input, bytes, sizeof(bytes), length));

    Message output = {};
    TEST_ASSERT_FALSE(decodeZigbeeMessage(bytes, length - 1, output));
    bytes[2] = 2;
    TEST_ASSERT_FALSE(decodeZigbeeMessage(bytes, length, output));
}

void test_encoder_rejects_insufficient_buffer_and_invalid_type() {
    Message input = {};
    uint8_t bytes[ZIGBEE_MESSAGE_MAX_ENCODED_SIZE] = {};
    size_t length = 0;
    TEST_ASSERT_FALSE(encodeZigbeeMessage(input, bytes, 50, length));

    input.type = static_cast<MessageType>(255);
    TEST_ASSERT_FALSE(encodeZigbeeMessage(input, bytes, sizeof(bytes), length));
}

void test_v4_pairing_packet_mapping_survives_zigbee_wire_codec() {
    ProvisioningPacket packet = {};
    packet.type = ProvisioningPacketType::PAIR_ACCEPT;
    packet.role = DeviceRole::MAIN;
    packet.pairingState = PairingState::PAIRED;
    packet.deviceId = 7;
    packet.parentMainId = 42;
    strcpy(packet.hardwareId, "C6-0011223344556677");
    strcpy(packet.name, "LAMP01");

    Message applicationMessage = {};
    TEST_ASSERT_TRUE(encodeProvisioningPacket(packet, 81, 42, 0,
                                              applicationMessage));
    uint8_t bytes[ZIGBEE_MESSAGE_MAX_ENCODED_SIZE] = {};
    size_t length = 0;
    TEST_ASSERT_TRUE(encodeZigbeeMessage(applicationMessage, bytes,
                                         sizeof(bytes), length));

    Message receivedMessage = {};
    TEST_ASSERT_TRUE(decodeZigbeeMessage(bytes, length, receivedMessage));
    ProvisioningPacket receivedPacket = {};
    TEST_ASSERT_TRUE(decodeProvisioningPacket(receivedMessage, receivedPacket));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ProvisioningPacketType::PAIR_ACCEPT),
                          static_cast<int>(receivedPacket.type));
    TEST_ASSERT_EQUAL_UINT32(packet.deviceId, receivedPacket.deviceId);
    TEST_ASSERT_EQUAL_UINT32(packet.parentMainId, receivedPacket.parentMainId);
    TEST_ASSERT_EQUAL_STRING(packet.hardwareId, receivedPacket.hardwareId);
    TEST_ASSERT_EQUAL_STRING(packet.name, receivedPacket.name);
}

void test_unpair_packets_bind_transaction_to_real_zigbee_endpoints() {
    ProvisioningPacket request = {};
    request.type = ProvisioningPacketType::UNPAIR_REQUEST;
    request.role = DeviceRole::MAIN;
    request.pairingState = PairingState::UNPAIRING;
    request.deviceId = 11;
    request.parentMainId = 88;
    request.transactionId = 0xAABBCCDD;
    strcpy(request.hardwareId, "C6-0011223344556677");
    assertProvisioningWireEnvelope(request, 88, 11);

    Message encoded = {};
    TEST_ASSERT_TRUE(encodeProvisioningPacket(request, 101, 88, 11, encoded));
    encoded.sourceId = 89;
    ProvisioningPacket decoded = {};
    TEST_ASSERT_FALSE(decodeProvisioningPacket(encoded, decoded));
    encoded.sourceId = 88;
    encoded.destinationId = 12;
    TEST_ASSERT_FALSE(decodeProvisioningPacket(encoded, decoded));

    ProvisioningPacket confirmation = request;
    confirmation.type = ProvisioningPacketType::UNPAIR_CONFIRM;
    confirmation.role = DeviceRole::LAMP;
    assertProvisioningWireEnvelope(confirmation, 11, 88);

    ProvisioningPacket rejection = confirmation;
    rejection.type = ProvisioningPacketType::UNPAIR_REJECT;
    rejection.pairingState = PairingState::PAIRED;
    assertProvisioningWireEnvelope(rejection, 11, 88);

    ProvisioningPacket commit = request;
    commit.type = ProvisioningPacketType::UNPAIR_COMMIT;
    commit.pairingState = PairingState::UNPAIRED;
    assertProvisioningWireEnvelope(commit, 88, 11);

    ProvisioningPacket announcement = confirmation;
    announcement.type = ProvisioningPacketType::DEVICE_ANNOUNCE;
    announcement.pairingState = PairingState::UNPAIRING;
    assertProvisioningWireEnvelope(announcement, 11, 0);
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_command_round_trip_uses_stable_wire_format);
    RUN_TEST(test_provisioning_payload_round_trip);
    RUN_TEST(test_decoder_rejects_truncated_and_unknown_version_frames);
    RUN_TEST(test_encoder_rejects_insufficient_buffer_and_invalid_type);
    RUN_TEST(test_v4_pairing_packet_mapping_survives_zigbee_wire_codec);
    RUN_TEST(test_unpair_packets_bind_transaction_to_real_zigbee_endpoints);
    return UNITY_END();
}
