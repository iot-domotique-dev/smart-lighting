#include <stddef.h>
#include <string.h>
#include <vector>

#include <unity.h>

#include "communication.h"
#include "core_command_service.h"
#include "core_link_codec.h"
#include "core_module_service.h"
#include "core_topology_protocol.h"
#include "device_registry.h"
#include "message_deduplicator.h"
#include "message_router.h"
#include "message_id_generator.h"
#include "zigbee_message_codec.h"
#include "zigbee_route_table.h"

FakeHardwareSerial Serial;
static uint32_t fakeNow = 1000;

uint32_t millis() { return fakeNow; }

// The production Communication wrappers are excluded by the native profile.
// These wrappers dispatch the real services into fault-injected wire transports.
bool sendMessage(Communication& communication, Message message) {
    if (communication.state != CommunicationState::READY ||
        communication.transport == nullptr || !communication.transport->isReady())
        return false;
    if (message.timestamp == 0) message.timestamp = millis();
    message.status = MessageStatus::SENT;
    return communication.transport->send(message);
}

bool receiveMessage(Communication& communication, Message& message) {
    return communication.state == CommunicationState::READY &&
           communication.transport != nullptr && communication.transport->isReady() &&
           communication.transport->receive(message);
}

namespace {
constexpr uint32_t MAIN_LOCAL = 0x12345678;
constexpr uint32_t LAMP_LOCAL = 17;
constexpr uint32_t FIRST_COMMAND = 0x82340042;  // Full uint32_t, never UART sequence.
constexpr uint16_t CORE_PHYSICAL = 0x1122;
constexpr uint16_t MAIN_PHYSICAL = 0x2233;
constexpr uint16_t LAMP_PHYSICAL = 0x3344;

struct Envelope {
    Message message;
    uint32_t hop;
    CommunicationRouteScope scope;
    uint16_t physicalDestination;
};

class WireTransport : public CommunicationTransportInterface {
    bool uart;
    size_t receiveIndex = 0;
    uint16_t sequence = 0xCAFE;
public:
    bool ready = true;
    bool failNext = false;
    bool enforceRoutes = false;
    ZigbeeRouteTable routes;
    unsigned codecErrors = 0;
    std::vector<Envelope> sent;
    std::vector<Message> incoming;

    explicit WireTransport(bool isUart = false) : uart(isUart) {}
    bool begin() override { return ready; }
    bool isReady() const override { return ready; }
    const char* name() const override { return uart ? "TEST-UART" : "TEST-ZIGBEE"; }
    bool send(const Message& message) override { return sendVia(message, 0); }

    bool sendVia(const Message& message, uint32_t hop,
                 CommunicationRouteScope scope = CommunicationRouteScope::LOCAL_DEVICE) override {
        if (!ready || failNext) {
            failNext = false;
            return false;
        }
        Message decoded = {};
        uint16_t physicalDestination = 0;
        if (!uart && enforceRoutes &&
            !(hop != 0 ? routes.find(hop, scope, physicalDestination)
                       : routes.find(message, physicalDestination))) {
            return false;
        }
        bool valid = false;
        if (uart) {
            CoreLinkPacket input = {};
            input.sequence = sequence++;
            input.type = message.type == MessageType::COMMAND
                ? CoreLinkMessageType::COMMAND : CoreLinkMessageType::COMMAND_RESULT;
            input.message = message;
            uint8_t bytes[CORE_LINK_MAX_FRAME_SIZE] = {};
            size_t length = 0;
            if (encodeCoreLinkFrame(input, bytes, sizeof(bytes), length)) {
                // Every UART transfer passes the incremental stream decoder.
                CoreLinkFrameDecoder decoder;
                CoreLinkPacket output = {};
                for (size_t i = 0; i < length; ++i) {
                    const bool complete = decoder.pushByte(bytes[i], output);
                    if (complete && i + 1 == length) valid = true;
                }
                decoded = output.message;
            }
        } else {
            uint8_t bytes[ZIGBEE_MESSAGE_MAX_ENCODED_SIZE] = {};
            size_t length = 0;
            valid = encodeZigbeeMessage(message, bytes, sizeof(bytes), length) &&
                    decodeZigbeeMessage(bytes, length, decoded);
        }
        if (!valid) {
            ++codecErrors;
            return false;
        }
        sent.push_back({decoded, hop, scope, physicalDestination});
        return true;
    }

    bool receive(Message& message) override {
        if (receiveIndex == incoming.size()) return false;
        message = incoming[receiveIndex++];
        return true;
    }
};

Communication communication(WireTransport& transport, uint32_t localId) {
    Communication value = {};
    value.type = CommunicationTransportType::ZIGBEE;
    value.state = CommunicationState::READY;
    value.localDeviceId = localId;
    value.transport = &transport;
    return value;
}

class Network {
    size_t wifiIndex = 0;
    size_t bridgeIndex = 0;
    size_t mainIndex = 0;
    size_t lampIndex = 0;
    size_t uartReplyIndex = 0;
public:
    uint32_t lampLocal;
    uint32_t mainId = makeCoreModuleId(CORE_LOGICAL_ID, MAIN_LOCAL);
    uint32_t lampId;
    DeviceRegistry registry = {};
    LampRegistry mainLamps = {}, lampLamps = {};
    GroupRegistry groups = {};
    SceneRegistry scenes = {};
    MessageTracker coreTracker = {}, lampTracker = {};
    MessageDeduplicator lampDedup = {};
    WireTransport wifiUart{true}, bridgeUart{true}, bridgeRadio, mainRadio, lampRadio;
    Communication wifiLink = communication(wifiUart, CORE_LOGICAL_ID);
    Communication bridgeLink = communication(bridgeUart, CORE_LOGICAL_ID);
    Communication bridgeZigbee = communication(bridgeRadio, CORE_LOGICAL_ID);
    Communication mainZigbee = communication(mainRadio, MAIN_LOCAL);
    Communication lampZigbee = communication(lampRadio, lampLocal);
    CoreCommandService core{registry, coreTracker, wifiLink};
    CoreCommandBridge bridge{bridgeZigbee, bridgeLink};
    MainCommandRelay relay{mainZigbee, mainLamps};
    unsigned dropCoreToMain = 0;
    unsigned dropMainToLamp = 0;
    unsigned dropLampToMain = 0;
    unsigned dropFinalMainToCore = 0;
    unsigned dropFinalUart = 0;
    bool dropAllCommands = false;
    bool dropAllFinalReplies = false;
    unsigned handledReplies = 0;
    unsigned rejectedReplies = 0;
    bool exercisePhysicalRoutes;

    explicit Network(uint32_t localLampId = LAMP_LOCAL, bool routePhysical = false)
        : lampLocal(localLampId), lampId(makeCoreModuleId(mainId, localLampId)),
          exercisePhysicalRoutes(routePhysical) {
        initDeviceRegistry(registry);
        initLampRegistry(mainLamps);
        initLampRegistry(lampLamps);
        initGroupRegistry(groups);
        initSceneRegistry(scenes);
        initMessageTracker(coreTracker);
        initMessageTracker(lampTracker);
        initMessageDeduplicator(lampDedup);
        Device root = {};
        root.id = CORE_LOGICAL_ID;
        root.name = "CORE";
        root.role = DeviceRole::CORE;
        root.status = DeviceStatus::ONLINE;
        root.localId = CORE_LOGICAL_ID;
        TEST_ASSERT_TRUE(registerDevice(registry, root));
        Device main = {};
        main.id = mainId;
        main.name = "MAIN_LIGHTING";
        main.role = DeviceRole::MAIN;
        main.status = DeviceStatus::ONLINE;
        main.parentId = CORE_LOGICAL_ID;
        main.localId = MAIN_LOCAL;
        TEST_ASSERT_TRUE(registerDevice(registry, main));
        Device lamp = {};
        lamp.id = lampId;
        lamp.name = "lamp001";
        lamp.role = DeviceRole::LAMP;
        lamp.status = DeviceStatus::ONLINE;
        lamp.parentId = mainId;
        lamp.localId = lampLocal;
        TEST_ASSERT_TRUE(registerDevice(registry, lamp));
        Lamp localLamp = {};
        localLamp.device.id = lampLocal;
        localLamp.device.name = "lamp001";
        localLamp.device.role = DeviceRole::LAMP;
        localLamp.device.status = DeviceStatus::ONLINE;
        localLamp.identity.parentMainId = MAIN_LOCAL;
        localLamp.identity.pairingState = PairingState::PAIRED;
        TEST_ASSERT_TRUE(addLamp(mainLamps, localLamp));
        TEST_ASSERT_TRUE(addLamp(lampLamps, localLamp));
        bridge.setMain(MAIN_LOCAL, mainId);
        relay.setMain(MAIN_LOCAL, mainId);
        if (exercisePhysicalRoutes) {
            bridgeRadio.enforceRoutes = mainRadio.enforceRoutes = lampRadio.enforceRoutes = true;
            seedRoutesFromAnnouncements();
        }
    }

    void seedRoutesFromAnnouncements() {
        Message mainAnnouncement = {};
        TEST_ASSERT_TRUE(makeCoreMainAnnouncement(MAIN_LOCAL, "MAIN_LIGHTING",
                                                   DEVICE_CAP_POWER, mainAnnouncement));
        // Broadcasts establish the same physical routes as the APS RX callback.
        bridgeRadio.routes.remember(mainAnnouncement, MAIN_PHYSICAL);
        lampRadio.routes.remember(mainAnnouncement, MAIN_PHYSICAL);
        Message assignment = {};
        TEST_ASSERT_TRUE(makeCoreIdAssignedMessage(MAIN_LOCAL, mainId,
            static_cast<uint8_t>(CoreModuleUpdateResult::REGISTERED), assignment));
        mainRadio.routes.remember(assignment, CORE_PHYSICAL);
        refreshLampAnnouncement();
    }

    void refreshLampAnnouncement() {
        Message lampAnnouncement = {};
        lampAnnouncement.type = MessageType::DEVICE_ANNOUNCE;
        lampAnnouncement.sourceId = lampLocal;
        lampAnnouncement.provisioningDeviceId = lampLocal;
        lampAnnouncement.parentMainId = MAIN_LOCAL;
        lampAnnouncement.pairingState = static_cast<uint8_t>(PairingState::PAIRED);
        lampAnnouncement.deviceRole = static_cast<uint8_t>(DeviceRole::LAMP);
        strcpy(lampAnnouncement.hardwareId, "C6-LAMP001");
        mainRadio.routes.remember(lampAnnouncement, LAMP_PHYSICAL);
        bridgeRadio.routes.remember(lampAnnouncement, LAMP_PHYSICAL);
    }

    void submit(bool power = true, uint32_t id = FIRST_COMMAND) {
        uint32_t messageId = 0;
        TEST_ASSERT_EQUAL_INT(static_cast<int>(CoreCommandError::NONE),
            static_cast<int>(core.submitPower(lampId, power, messageId, id)));
        TEST_ASSERT_EQUAL_UINT32(id, messageId);
    }

    PendingMessage* pending(uint32_t id = FIRST_COMMAND) {
        return findPendingMessage(coreTracker, id);
    }

    void consumeMainOutput() {
        while (mainIndex < mainRadio.sent.size()) {
            const Envelope envelope = mainRadio.sent[mainIndex++];
            if (envelope.message.type == MessageType::COMMAND) {
                TEST_ASSERT_EQUAL_UINT32(lampLocal, envelope.hop);
                TEST_ASSERT_EQUAL_INT(static_cast<int>(CommunicationRouteScope::LOCAL_DEVICE),
                                      static_cast<int>(envelope.scope));
                if (dropMainToLamp != 0) { --dropMainToLamp; continue; }
                if (exercisePhysicalRoutes) {
                    TEST_ASSERT_EQUAL_UINT16(LAMP_PHYSICAL, envelope.physicalDestination);
                    lampRadio.routes.remember(envelope.message, MAIN_PHYSICAL);
                }
                lampRadio.incoming.push_back(envelope.message);
            } else {
                TEST_ASSERT_EQUAL_UINT32(CORE_LOGICAL_ID, envelope.hop);
                TEST_ASSERT_EQUAL_INT(static_cast<int>(CommunicationRouteScope::CORE),
                                      static_cast<int>(envelope.scope));
                const bool terminal = envelope.message.executionStatus !=
                    ExecutionStatus::NOT_EXECUTED;
                if (terminal && (dropAllFinalReplies || dropFinalMainToCore != 0)) {
                    if (dropFinalMainToCore != 0) --dropFinalMainToCore;
                    continue;
                }
                if (exercisePhysicalRoutes) {
                    TEST_ASSERT_EQUAL_UINT16(CORE_PHYSICAL, envelope.physicalDestination);
                    bridgeRadio.routes.remember(envelope.message, MAIN_PHYSICAL);
                }
                TEST_ASSERT_TRUE(bridge.handleFromZigbee(envelope.message));
            }
        }
    }

    void consumeUartReplies() {
        while (uartReplyIndex < bridgeUart.sent.size()) {
            const Message reply = bridgeUart.sent[uartReplyIndex++].message;
            if (reply.executionStatus != ExecutionStatus::NOT_EXECUTED &&
                dropFinalUart != 0) { --dropFinalUart; continue; }
            if (core.handleReply(reply)) ++handledReplies;
            else ++rejectedReplies;
        }
    }

    void pump(bool dropAckAtLamp = false) {
        while (wifiIndex < wifiUart.sent.size()) {
            TEST_ASSERT_TRUE(bridge.handleFromCore(wifiUart.sent[wifiIndex++].message));
        }
        while (bridgeIndex < bridgeRadio.sent.size()) {
            const Envelope envelope = bridgeRadio.sent[bridgeIndex++];
            TEST_ASSERT_EQUAL_UINT32(MAIN_LOCAL, envelope.hop);
            if (dropAllCommands || dropCoreToMain != 0) {
                if (dropCoreToMain != 0) --dropCoreToMain;
                continue;
            }
            if (exercisePhysicalRoutes) {
                TEST_ASSERT_EQUAL_UINT16(MAIN_PHYSICAL, envelope.physicalDestination);
                mainRadio.routes.remember(envelope.message, CORE_PHYSICAL);
                refreshLampAnnouncement();
            }
            TEST_ASSERT_TRUE(relay.handleMessage(envelope.message));
        }
        consumeMainOutput();
        consumeUartReplies();
        processMessages(lampZigbee, lampTracker, lampDedup, lampLamps, groups,
                        scenes, dropAckAtLamp);
        while (lampIndex < lampRadio.sent.size()) {
            const Envelope envelope = lampRadio.sent[lampIndex++];
            TEST_ASSERT_EQUAL_UINT32(MAIN_LOCAL, envelope.hop);
            if (dropLampToMain != 0) { --dropLampToMain; continue; }
            if (exercisePhysicalRoutes) {
                TEST_ASSERT_EQUAL_UINT16(MAIN_PHYSICAL, envelope.physicalDestination);
                mainRadio.routes.remember(envelope.message, LAMP_PHYSICAL);
                // A new LAMP broadcast between the receipt and execution ACK
                // must never replace the numeric CORE 1 physical route.
                refreshLampAnnouncement();
            }
            TEST_ASSERT_TRUE(relay.handleMessage(envelope.message));
        }
        consumeMainOutput();
        consumeUartReplies();
        TEST_ASSERT_EQUAL_UINT(0, wifiUart.codecErrors + bridgeUart.codecErrors +
            bridgeRadio.codecErrors + mainRadio.codecErrors + lampRadio.codecErrors);
    }

    void retry() { fakeNow += MESSAGE_TIMEOUT; core.poll(); pump(); }

    void assertExecuted(uint32_t id = FIRST_COMMAND) {
        PendingMessage* result = pending(id);
        TEST_ASSERT_NOT_NULL(result);
        TEST_ASSERT_TRUE(result->accepted);
        TEST_ASSERT_TRUE(result->completed);
        TEST_ASSERT_FALSE(result->waitingForAck);
        TEST_ASSERT_FALSE(result->timedOut);
        TEST_ASSERT_EQUAL_INT(static_cast<int>(ExecutionStatus::EXECUTED),
                              static_cast<int>(result->message.executionStatus));
    }

    void assertFailure(CoreCommandError error, uint32_t id = FIRST_COMMAND) {
        PendingMessage* result = pending(id);
        TEST_ASSERT_NOT_NULL(result);
        TEST_ASSERT_TRUE(result->completed);
        TEST_ASSERT_FALSE(result->waitingForAck);
        TEST_ASSERT_FALSE(result->timedOut);
        TEST_ASSERT_EQUAL_INT(static_cast<int>(ExecutionStatus::FAILED),
                              static_cast<int>(result->message.executionStatus));
        TEST_ASSERT_EQUAL_INT32(static_cast<int32_t>(error), result->message.value2);
    }

    void deliverReply(const Message& reply) {
        // Even deliberately malformed logical replies cross both real codecs.
        TEST_ASSERT_TRUE(mainRadio.sendVia(reply, CORE_LOGICAL_ID,
                                          CommunicationRouteScope::CORE));
        consumeMainOutput();
        consumeUartReplies();
    }
};

void assertSameCommand(const Message& expected, const Message& actual) {
    TEST_ASSERT_EQUAL_UINT32(expected.id, actual.id);
    TEST_ASSERT_EQUAL_UINT32(expected.sourceId, actual.sourceId);
    TEST_ASSERT_EQUAL_UINT32(expected.destinationId, actual.destinationId);
    TEST_ASSERT_EQUAL_UINT32(expected.parentMainId, actual.parentMainId);
    TEST_ASSERT_EQUAL_UINT32(expected.provisioningDeviceId, actual.provisioningDeviceId);
    TEST_ASSERT_EQUAL_UINT32(expected.timestamp, actual.timestamp);
    TEST_ASSERT_EQUAL_INT32(expected.commandType, actual.commandType);
    TEST_ASSERT_EQUAL_INT32(expected.value, actual.value);
}
}  // namespace

void setUp() {
    fakeNow = 1000;
    Serial.powerExecutionCount = 0;
    (void)consumeDropNextCoreExecutionAck();
}
void tearDown() {}

void test_set_power_traverses_uart_main_lamp_and_execution_ack_returns() {
    Network network;
    network.submit();
    const Message original = network.wifiUart.sent[0].message;
    network.pump();
    network.assertExecuted();
    TEST_ASSERT_TRUE(findLamp(network.lampLamps, LAMP_LOCAL)->state.power);
    TEST_ASSERT_FALSE(findLamp(network.mainLamps, LAMP_LOCAL)->state.power);
    TEST_ASSERT_EQUAL_UINT32(1, Serial.powerExecutionCount);
    TEST_ASSERT_EQUAL_UINT(2, network.handledReplies); // Receipt and execution.
    TEST_ASSERT_EQUAL_UINT32(CORE_LOGICAL_ID, original.sourceId);
    TEST_ASSERT_EQUAL_UINT32(network.lampId, original.destinationId);
    TEST_ASSERT_EQUAL_UINT32(network.mainId, original.parentMainId);
    TEST_ASSERT_EQUAL_UINT32(FIRST_COMMAND, original.id);
    TEST_ASSERT_GREATER_THAN_UINT32(65535, original.id);
    assertSameCommand(original, network.bridgeRadio.sent[0].message);
    assertSameCommand(original, network.mainRadio.sent[1].message);
    TEST_ASSERT_EQUAL_UINT32(network.lampId, network.lampRadio.sent[0].message.sourceId);
    TEST_ASSERT_EQUAL_UINT32(FIRST_COMMAND,
        static_cast<uint32_t>(network.lampRadio.sent[0].message.value2));
}

void test_receipt_ack_is_non_terminal_and_does_not_postpone_retry() {
    Network network;
    network.dropMainToLamp = 1;
    network.submit();
    network.pump();
    TEST_ASSERT_TRUE(network.pending()->accepted);
    TEST_ASSERT_TRUE(network.pending()->waitingForAck);
    TEST_ASSERT_FALSE(network.pending()->completed);
    TEST_ASSERT_EQUAL_UINT32(1000, network.pending()->sentAt);
    TEST_ASSERT_EQUAL_UINT32(0, Serial.powerExecutionCount);
    fakeNow += MESSAGE_TIMEOUT - 1;
    network.core.poll();
    TEST_ASSERT_EQUAL_UINT(1, network.wifiUart.sent.size());
    ++fakeNow;
    network.core.poll();
    network.pump();
    network.assertExecuted();
    TEST_ASSERT_EQUAL_UINT8(1, network.pending()->retryCount);
}

void test_lost_command_retries_full_uint32_id_and_executes_once() {
    Network network;
    network.dropCoreToMain = 1;
    network.submit();
    network.pump();
    TEST_ASSERT_FALSE(network.pending()->accepted);
    TEST_ASSERT_EQUAL_UINT32(0, Serial.powerExecutionCount);
    network.retry();
    network.assertExecuted();
    TEST_ASSERT_EQUAL_UINT32(1, Serial.powerExecutionCount);
    TEST_ASSERT_EQUAL_UINT(2, network.wifiUart.sent.size());
    assertSameCommand(network.wifiUart.sent[0].message, network.wifiUart.sent[1].message);
}

void test_lost_lamp_ack_retries_without_second_power_execution() {
    Network network;
    network.submit();
    network.pump(true); // Existing router's intentional ACK loss switch.
    TEST_ASSERT_TRUE(network.pending()->accepted);
    TEST_ASSERT_FALSE(network.pending()->completed);
    TEST_ASSERT_EQUAL_UINT32(1, Serial.powerExecutionCount);
    network.retry();
    network.assertExecuted();
    TEST_ASSERT_EQUAL_UINT32(1, Serial.powerExecutionCount);
    TEST_ASSERT_EQUAL_UINT8(1, network.lampDedup.count);
}

void test_lost_main_to_core_ack_replays_cached_execution_result() {
    Network network;
    network.dropFinalMainToCore = 1;
    network.submit();
    network.pump();
    TEST_ASSERT_FALSE(network.pending()->completed);
    network.retry();
    network.assertExecuted();
    TEST_ASSERT_EQUAL_UINT32(1, Serial.powerExecutionCount);
    TEST_ASSERT_EQUAL_UINT(2, network.lampRadio.sent.size());
}

void test_lost_lamp_to_main_ack_retries_without_second_execution() {
    Network network;
    network.dropLampToMain = 1;
    network.submit();
    network.pump();
    TEST_ASSERT_FALSE(network.pending()->completed);
    network.retry();
    network.assertExecuted();
    TEST_ASSERT_EQUAL_UINT32(1, Serial.powerExecutionCount);
}

void test_console_ack_loss_diagnostic_is_consumed_only_by_core_execution_ack() {
    Network network;
    requestDropNextCoreExecutionAck();
    network.submit();
    network.pump();
    TEST_ASSERT_TRUE(network.pending()->accepted);
    TEST_ASSERT_FALSE(network.pending()->completed);
    TEST_ASSERT_EQUAL_UINT32(1, Serial.powerExecutionCount);
    network.retry();
    network.assertExecuted();
    TEST_ASSERT_EQUAL_UINT32(1, Serial.powerExecutionCount);
    TEST_ASSERT_FALSE(consumeDropNextCoreExecutionAck());
}

void test_long_core_pause_expires_without_retry_after_dedup_window() {
    Network network;
    network.submit();
    network.pump(true);
    fakeNow += 60001;
    network.core.poll();
    TEST_ASSERT_EQUAL_UINT(1, network.wifiUart.sent.size());
    TEST_ASSERT_TRUE(network.pending()->timedOut);
    TEST_ASSERT_EQUAL_UINT8(0, network.pending()->retryCount);
    TEST_ASSERT_EQUAL_UINT32(1, Serial.powerExecutionCount);
}

void test_message_id_seed_preserves_32_bits_and_skips_zero_on_wrap() {
    seedMessageIds(UINT32_MAX);
    TEST_ASSERT_EQUAL_UINT32(UINT32_MAX, generateMessageId());
    TEST_ASSERT_EQUAL_UINT32(1, generateMessageId());
    seedMessageIds(0);
    TEST_ASSERT_EQUAL_UINT32(1, generateMessageId());
}

void test_ack_arriving_after_deadline_before_poll_cannot_claim_execution() {
    Network network;
    network.submit();
    network.pump(true);
    fakeNow += MESSAGE_TIMEOUT * (MAX_MESSAGE_RETRIES + 1);
    const Message reply = makeCoreCommandAck(network.wifiUart.sent[0].message,
        network.lampId, ExecutionStatus::EXECUTED);
    network.deliverReply(reply);
    TEST_ASSERT_TRUE(network.pending()->timedOut);
    TEST_ASSERT_EQUAL_UINT(1, network.wifiUart.sent.size());
    TEST_ASSERT_EQUAL_UINT32(1, Serial.powerExecutionCount);
}

void test_legacy_insertions_cannot_evict_protected_core_results() {
    Network network;
    network.submit();
    network.pump(true);
    for (unsigned i = 0; i < MAX_PROCESSED_MESSAGES * 2; ++i)
        (void)registerProcessedMessage(network.lampDedup, 99, i + 1,
                                       ExecutionStatus::EXECUTED);
    TEST_ASSERT_NOT_NULL(findProcessedMessage(network.lampDedup,
                                              CORE_LOGICAL_ID, FIRST_COMMAND));
    network.retry();
    network.assertExecuted();
    TEST_ASSERT_EQUAL_UINT32(1, Serial.powerExecutionCount);
}

void test_lost_uart_execution_ack_retries_entire_path_with_same_identity() {
    Network network;
    network.dropFinalUart = 1;
    network.submit();
    network.pump();
    TEST_ASSERT_FALSE(network.pending()->completed);
    network.retry();
    network.assertExecuted();
    TEST_ASSERT_EQUAL_UINT32(1, Serial.powerExecutionCount);
    assertSameCommand(network.wifiUart.sent[0].message, network.wifiUart.sent[1].message);
}

void test_duplicate_message_id_rejects_resubmit_and_wire_replay_never_reexecutes() {
    Network network;
    network.submit();
    network.pump();
    uint32_t outputId = 123;
    TEST_ASSERT_EQUAL_INT(static_cast<int>(CoreCommandError::DUPLICATE_ID),
        static_cast<int>(network.core.submitPower(network.lampId, false,
                                                  outputId, FIRST_COMMAND)));
    TEST_ASSERT_EQUAL_UINT32(0, outputId);
    Message replay = network.wifiUart.sent[0].message;
    replay.value = 0; // State alone would miss a duplicate ON execution.
    TEST_ASSERT_TRUE(network.wifiUart.send(replay));
    network.pump();
    TEST_ASSERT_TRUE(findLamp(network.lampLamps, LAMP_LOCAL)->state.power);
    TEST_ASSERT_EQUAL_UINT32(1, Serial.powerExecutionCount);
    TEST_ASSERT_EQUAL_UINT(2, network.lampRadio.sent.size());
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ExecutionStatus::FAILED),
        static_cast<int>(network.lampRadio.sent.back().message.executionStatus));
    TEST_ASSERT_EQUAL_INT32(static_cast<int32_t>(CoreCommandError::DUPLICATE_ID),
        network.lampRadio.sent.back().message.value);
}

void test_distinct_message_ids_execute_on_and_off_independently() {
    Network network;
    network.submit();
    network.pump();
    network.submit(false, FIRST_COMMAND + 1);
    network.pump();
    network.assertExecuted(FIRST_COMMAND + 1);
    TEST_ASSERT_FALSE(findLamp(network.lampLamps, LAMP_LOCAL)->state.power);
    TEST_ASSERT_EQUAL_UINT32(2, Serial.powerExecutionCount);
}

void test_core_refuses_offline_lamp_or_main_without_transmitting() {
    Network network;
    uint32_t id = 123;
    findDeviceById(network.registry, network.lampId)->status = DeviceStatus::OFFLINE;
    TEST_ASSERT_EQUAL_INT(static_cast<int>(CoreCommandError::OFFLINE),
        static_cast<int>(network.core.submitPower(network.lampId, true, id)));
    TEST_ASSERT_EQUAL_UINT32(0, id);
    findDeviceById(network.registry, network.lampId)->status = DeviceStatus::ONLINE;
    findDeviceById(network.registry, network.mainId)->status = DeviceStatus::OFFLINE;
    TEST_ASSERT_EQUAL_INT(static_cast<int>(CoreCommandError::OFFLINE),
        static_cast<int>(network.core.submitPower(network.lampId, true, id)));
    TEST_ASSERT_EQUAL_UINT(0, network.wifiUart.sent.size());
    TEST_ASSERT_EQUAL_UINT8(0, network.coreTracker.count);
}

void test_main_offline_lamp_returns_failure_without_receipt_or_execution() {
    Network network;
    findLamp(network.mainLamps, LAMP_LOCAL)->device.status = DeviceStatus::OFFLINE;
    network.submit();
    network.pump();
    network.assertFailure(CoreCommandError::OFFLINE);
    TEST_ASSERT_FALSE(network.pending()->accepted);
    TEST_ASSERT_EQUAL_UINT32(0, Serial.powerExecutionCount);
    TEST_ASSERT_EQUAL_UINT(0, network.lampRadio.incoming.size());
}

void test_lamp_offline_returns_failure_after_main_acceptance() {
    Network network;
    findLamp(network.lampLamps, LAMP_LOCAL)->device.status = DeviceStatus::OFFLINE;
    network.submit();
    network.pump();
    network.assertFailure(CoreCommandError::OFFLINE);
    TEST_ASSERT_TRUE(network.pending()->accepted);
    TEST_ASSERT_EQUAL_UINT32(0, Serial.powerExecutionCount);
}

void test_unknown_destination_is_clean_local_error_and_main_disappearance_is_reported() {
    Network network;
    uint32_t id = 123;
    TEST_ASSERT_EQUAL_INT(static_cast<int>(CoreCommandError::UNKNOWN_DESTINATION),
        static_cast<int>(network.core.submitPower(0xABCDEF12, true, id)));
    TEST_ASSERT_EQUAL_UINT32(0, id);
    TEST_ASSERT_EQUAL_UINT(0, network.wifiUart.sent.size());
    TEST_ASSERT_TRUE(removeLamp(network.mainLamps, LAMP_LOCAL));
    network.submit();
    network.pump();
    network.assertFailure(CoreCommandError::UNKNOWN_DESTINATION);
    TEST_ASSERT_EQUAL_UINT32(0, Serial.powerExecutionCount);
}

void test_invalid_registry_parent_and_bridge_main_mapping_are_rejected() {
    Network network;
    uint32_t id = 123;
    findDeviceById(network.registry, network.lampId)->parentId = 55;
    TEST_ASSERT_EQUAL_INT(static_cast<int>(CoreCommandError::INVALID_ROUTE),
        static_cast<int>(network.core.submitPower(network.lampId, true, id)));
    findDeviceById(network.registry, network.lampId)->parentId = network.mainId;
    network.bridge.setMain(MAIN_LOCAL + 1, network.mainId + 1);
    network.submit();
    network.pump();
    network.assertFailure(CoreCommandError::INVALID_ROUTE);
    TEST_ASSERT_EQUAL_UINT(0, network.bridgeRadio.sent.size());
    TEST_ASSERT_EQUAL_UINT32(0, Serial.powerExecutionCount);
}

void test_main_rejects_lamp_paired_to_different_main() {
    Network network;
    findLamp(network.mainLamps, LAMP_LOCAL)->identity.parentMainId = MAIN_LOCAL + 1;
    network.submit();
    network.pump();
    network.assertFailure(CoreCommandError::INVALID_ROUTE);
    TEST_ASSERT_FALSE(network.pending()->accepted);
    TEST_ASSERT_EQUAL_UINT32(0, Serial.powerExecutionCount);
}

void test_wrong_ack_provenance_target_correlation_and_result_are_ignored() {
    Network network;
    network.submit();
    const Message command = network.wifiUart.sent[0].message;
    Message ack = makeCoreCommandAck(command, network.mainId, ExecutionStatus::EXECUTED);
    network.deliverReply(ack); // MAIN cannot claim LAMP execution.
    ack = makeCoreCommandAck(command, network.lampId, ExecutionStatus::EXECUTED);
    ++ack.provisioningDeviceId;
    network.deliverReply(ack);
    ack = makeCoreCommandAck(command, network.lampId, ExecutionStatus::EXECUTED);
    ++ack.value2;
    network.deliverReply(ack);
    ack = makeCoreCommandAck(command, network.lampId, ExecutionStatus::EXECUTED);
    ++ack.commandType;
    network.deliverReply(ack);
    ack = makeCoreCommandAck(command, network.lampId, ExecutionStatus::EXECUTED);
    ack.value = static_cast<int32_t>(ExecutionStatus::FAILED);
    network.deliverReply(ack);
    TEST_ASSERT_FALSE(network.pending()->accepted);
    TEST_ASSERT_TRUE(network.pending()->waitingForAck);
    TEST_ASSERT_FALSE(network.pending()->completed);
    TEST_ASSERT_EQUAL_UINT(5, network.rejectedReplies);
    network.pump();
    network.assertExecuted();
}

void test_partial_ack_cannot_complete_indivisible_power_command() {
    Network network;
    network.submit();
    const Message reply = makeCoreCommandAck(network.wifiUart.sent[0].message,
        network.lampId, ExecutionStatus::PARTIAL);
    network.deliverReply(reply);
    TEST_ASSERT_TRUE(network.pending()->waitingForAck);
    TEST_ASSERT_FALSE(network.pending()->completed);
    network.pump();
    network.assertExecuted();
}

void test_command_loss_expires_after_initial_send_and_two_retries() {
    Network network;
    network.dropAllCommands = true;
    network.submit();
    network.pump();
    for (unsigned i = 0; i < MAX_MESSAGE_RETRIES + 1; ++i) network.retry();
    TEST_ASSERT_EQUAL_UINT(1 + MAX_MESSAGE_RETRIES, network.wifiUart.sent.size());
    TEST_ASSERT_EQUAL_UINT8(MAX_MESSAGE_RETRIES, network.pending()->retryCount);
    TEST_ASSERT_TRUE(network.pending()->timedOut);
    TEST_ASSERT_TRUE(network.pending()->completed);
    TEST_ASSERT_FALSE(network.pending()->waitingForAck);
    TEST_ASSERT_FALSE(network.pending()->accepted);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(MessageStatus::FAILED),
                          static_cast<int>(network.pending()->message.status));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ExecutionStatus::NOT_EXECUTED),
                          static_cast<int>(network.pending()->message.executionStatus));
    TEST_ASSERT_EQUAL_UINT32(0, Serial.powerExecutionCount);
}

void test_late_ack_does_not_rewrite_expired_command_or_claim_success() {
    Network network;
    network.dropAllFinalReplies = true;
    network.submit();
    network.pump();
    for (unsigned i = 0; i < MAX_MESSAGE_RETRIES + 1; ++i) network.retry();
    TEST_ASSERT_TRUE(network.pending()->accepted);
    TEST_ASSERT_TRUE(network.pending()->timedOut);
    TEST_ASSERT_EQUAL_UINT32(1, Serial.powerExecutionCount);
    const uint32_t completedAt = network.pending()->completedAt;
    network.dropAllFinalReplies = false;
    network.deliverReply(network.lampRadio.sent[0].message);
    TEST_ASSERT_TRUE(network.pending()->timedOut);
    TEST_ASSERT_EQUAL_UINT32(completedAt, network.pending()->completedAt);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ExecutionStatus::NOT_EXECUTED),
                          static_cast<int>(network.pending()->message.executionStatus));
}

void test_uart_and_radio_transport_failures_never_report_execution() {
    Network network;
    network.wifiUart.failNext = true;
    uint32_t id = 0;
    TEST_ASSERT_EQUAL_INT(static_cast<int>(CoreCommandError::TRANSPORT_FAILURE),
        static_cast<int>(network.core.submitPower(network.lampId, true, id, FIRST_COMMAND)));
    network.assertFailure(CoreCommandError::TRANSPORT_FAILURE);
    network.bridgeRadio.failNext = true;
    network.submit(true, FIRST_COMMAND + 1);
    network.pump();
    network.assertFailure(CoreCommandError::TRANSPORT_FAILURE, FIRST_COMMAND + 1);
    TEST_ASSERT_EQUAL_UINT32(0, Serial.powerExecutionCount);
}

void test_tracker_capacity_refuses_new_command_without_sending_it() {
    Network network;
    for (unsigned i = 0; i < MAX_PENDING_MESSAGES; ++i) {
        network.submit(true, FIRST_COMMAND + i);
    }
    uint32_t id = 123;
    TEST_ASSERT_EQUAL_INT(static_cast<int>(CoreCommandError::TRACKER_FULL),
        static_cast<int>(network.core.submitPower(network.lampId, false,
            id, FIRST_COMMAND + MAX_PENDING_MESSAGES)));
    TEST_ASSERT_EQUAL_UINT32(0, id);
    TEST_ASSERT_EQUAL_UINT(MAX_PENDING_MESSAGES, network.wifiUart.sent.size());
}

void test_full_dedup_cache_protects_retry_and_refuses_execution_until_expiry() {
    Network network;
    for (unsigned i = 0; i < MAX_PROCESSED_MESSAGES; ++i) {
        network.submit(true, FIRST_COMMAND + i);
        network.pump(i == 0); // Preserve one waiting command among completed ones.
    }
    TEST_ASSERT_EQUAL_UINT32(MAX_PROCESSED_MESSAGES, Serial.powerExecutionCount);
    TEST_ASSERT_NOT_NULL(findProcessedMessage(network.lampDedup,
                                              CORE_LOGICAL_ID, FIRST_COMMAND));
    const uint32_t excessId = FIRST_COMMAND + MAX_PROCESSED_MESSAGES;
    network.submit(false, excessId);
    network.pump();
    network.assertFailure(CoreCommandError::DEDUP_FULL, excessId);
    TEST_ASSERT_EQUAL_UINT32(MAX_PROCESSED_MESSAGES, Serial.powerExecutionCount);
    network.retry();
    network.assertExecuted();
    TEST_ASSERT_EQUAL_UINT32(MAX_PROCESSED_MESSAGES, Serial.powerExecutionCount);
    fakeNow = 61001;
    network.submit(false, excessId + 1);
    network.pump();
    network.assertExecuted(excessId + 1);
    TEST_ASSERT_EQUAL_UINT32(MAX_PROCESSED_MESSAGES + 1, Serial.powerExecutionCount);
    TEST_ASSERT_FALSE(findLamp(network.lampLamps, LAMP_LOCAL)->state.power);
}

void test_dedup_protection_survives_millis_wraparound() {
    Network network;
    fakeNow = UINT32_MAX - 1000;
    for (unsigned i = 0; i < MAX_PROCESSED_MESSAGES; ++i) {
        network.submit(true, FIRST_COMMAND + i);
        network.pump(i == 0);
    }
    fakeNow += MESSAGE_TIMEOUT; // Wrap the uint32_t clock, not the retry identity.
    network.core.poll();
    network.pump();
    network.assertExecuted();
    TEST_ASSERT_EQUAL_UINT32(MAX_PROCESSED_MESSAGES, Serial.powerExecutionCount);
    network.submit(false, FIRST_COMMAND + MAX_PROCESSED_MESSAGES);
    network.pump();
    network.assertFailure(CoreCommandError::DEDUP_FULL,
                          FIRST_COMMAND + MAX_PROCESSED_MESSAGES);
    TEST_ASSERT_EQUAL_UINT32(MAX_PROCESSED_MESSAGES, Serial.powerExecutionCount);
}

void test_core_one_and_lamp_local_one_route_to_distinct_physical_peers_with_retry() {
    Network network(1, true);
    network.dropFinalMainToCore = 1;
    network.submit();
    network.pump();
    TEST_ASSERT_TRUE(network.pending()->accepted);
    TEST_ASSERT_FALSE(network.pending()->completed);
    TEST_ASSERT_EQUAL_UINT32(1, Serial.powerExecutionCount);
    uint16_t address = 0;
    TEST_ASSERT_TRUE(network.mainRadio.routes.find(1, CommunicationRouteScope::CORE, address));
    TEST_ASSERT_EQUAL_UINT16(CORE_PHYSICAL, address);
    TEST_ASSERT_TRUE(network.mainRadio.routes.find(1,
        CommunicationRouteScope::LOCAL_DEVICE, address));
    TEST_ASSERT_EQUAL_UINT16(LAMP_PHYSICAL, address);
    network.refreshLampAnnouncement();
    network.retry();
    network.assertExecuted();
    TEST_ASSERT_EQUAL_UINT32(1, Serial.powerExecutionCount);
    TEST_ASSERT_TRUE(findLamp(network.lampLamps, 1)->state.power);
    assertSameCommand(network.wifiUart.sent[0].message, network.wifiUart.sent[1].message);
    for (const Envelope& envelope : network.mainRadio.sent) {
        // Both hops numerically equal 1. Only the physical route scope differs.
        TEST_ASSERT_EQUAL_UINT32(1, envelope.hop);
        if (envelope.message.type == MessageType::COMMAND) {
            TEST_ASSERT_EQUAL_UINT16(LAMP_PHYSICAL, envelope.physicalDestination);
            TEST_ASSERT_EQUAL_INT(static_cast<int>(CommunicationRouteScope::LOCAL_DEVICE),
                                  static_cast<int>(envelope.scope));
        } else {
            TEST_ASSERT_EQUAL_UINT16(CORE_PHYSICAL, envelope.physicalDestination);
            TEST_ASSERT_EQUAL_INT(static_cast<int>(CommunicationRouteScope::CORE),
                                  static_cast<int>(envelope.scope));
        }
    }
}

void test_missing_core_scope_never_falls_back_to_lamp_with_same_local_id() {
    Network network(1, true);
    network.submit();
    Message ack = makeCoreCommandAck(network.wifiUart.sent[0].message,
        network.mainId, ExecutionStatus::NOT_EXECUTED);
    WireTransport radio;
    radio.enforceRoutes = true;
    Message lamp = {};
    lamp.type = MessageType::DEVICE_ANNOUNCE;
    lamp.sourceId = 1;
    lamp.provisioningDeviceId = 1;
    lamp.deviceRole = static_cast<uint8_t>(DeviceRole::LAMP);
    lamp.pairingState = static_cast<uint8_t>(PairingState::PAIRED);
    radio.routes.remember(lamp, LAMP_PHYSICAL);
    TEST_ASSERT_FALSE(radio.sendVia(ack, 1, CommunicationRouteScope::CORE));
    TEST_ASSERT_FALSE(radio.send(ack)); // Inferred scope for an ordinary ACK.
    TEST_ASSERT_EQUAL_UINT(0, radio.sent.size());
    TEST_ASSERT_TRUE(radio.sendVia(network.wifiUart.sent[0].message, 1,
                                   CommunicationRouteScope::LOCAL_DEVICE));
    TEST_ASSERT_EQUAL_UINT16(LAMP_PHYSICAL, radio.sent.back().physicalDestination);
}

void test_pairing_hardware_route_survives_assignment_of_local_id_one() {
    WireTransport radio;
    radio.enforceRoutes = true;
    Message announcement = {};
    announcement.type = MessageType::DEVICE_ANNOUNCE;
    announcement.deviceRole = static_cast<uint8_t>(DeviceRole::LAMP);
    strcpy(announcement.hardwareId, "C6-PAIRING-LAMP");
    radio.routes.remember(announcement, LAMP_PHYSICAL);
    Message pairing = {};
    pairing.id = 123;
    pairing.type = MessageType::PAIR_ACCEPT;
    pairing.sourceId = MAIN_LOCAL;
    strcpy(pairing.hardwareId, announcement.hardwareId);
    TEST_ASSERT_TRUE(radio.send(pairing));
    TEST_ASSERT_EQUAL_UINT16(LAMP_PHYSICAL, radio.sent.back().physicalDestination);
    announcement.sourceId = 1;
    announcement.provisioningDeviceId = 1;
    announcement.pairingState = static_cast<uint8_t>(PairingState::PAIRED);
    radio.routes.remember(announcement, LAMP_PHYSICAL + 1);
    TEST_ASSERT_TRUE(radio.sendVia(pairing, 1, CommunicationRouteScope::LOCAL_DEVICE));
    TEST_ASSERT_EQUAL_UINT16(LAMP_PHYSICAL + 1, radio.sent.back().physicalDestination);
    TEST_ASSERT_TRUE(radio.send(pairing));
    TEST_ASSERT_EQUAL_UINT16(LAMP_PHYSICAL + 1, radio.sent.back().physicalDestination);
}

void test_full_physical_route_cache_preserves_core_and_lamp_collision_routes() {
    Network network(1, true);
    // MAIN already stores CORE 1 and LAMP local 1 in two separate slots.
    for (unsigned i = 0; i < ZIGBEE_ROUTE_TABLE_CAPACITY + 1; ++i) {
        Message announcement = {};
        announcement.type = MessageType::DEVICE_ANNOUNCE;
        announcement.sourceId = 100 + i;
        announcement.deviceRole = static_cast<uint8_t>(DeviceRole::LAMP);
        network.mainRadio.routes.remember(announcement, static_cast<uint16_t>(0x4400 + i));
    }
    network.submit();
    network.pump();
    network.assertExecuted();
    TEST_ASSERT_EQUAL_UINT32(1, Serial.powerExecutionCount);
    uint16_t address = 0;
    TEST_ASSERT_FALSE(network.mainRadio.routes.find(100 + ZIGBEE_ROUTE_TABLE_CAPACITY,
        CommunicationRouteScope::LOCAL_DEVICE, address));
}

void test_core_and_v5_routes_are_learned_in_distinct_scopes_from_messages() {
    for (unsigned order = 0; order < 2; ++order) {
        ZigbeeRouteTable routes;
        Message lamp = {};
        lamp.type = MessageType::DEVICE_ANNOUNCE;
        lamp.sourceId = 1;
        lamp.provisioningDeviceId = 1;
        lamp.deviceRole = static_cast<uint8_t>(DeviceRole::LAMP);
        lamp.pairingState = static_cast<uint8_t>(PairingState::PAIRED);
        Message command = {};
        command.type = MessageType::COMMAND;
        command.sourceId = CORE_LOGICAL_ID;
        command.destinationId = 1234;
        command.parentMainId = 5678;
        if (order == 0) {
            routes.remember(lamp, LAMP_PHYSICAL);
            routes.remember(command, CORE_PHYSICAL);
        } else {
            routes.remember(command, CORE_PHYSICAL);
            routes.remember(lamp, LAMP_PHYSICAL);
        }
        uint16_t address = 0;
        TEST_ASSERT_TRUE(routes.find(1, CommunicationRouteScope::CORE, address));
        TEST_ASSERT_EQUAL_UINT16(CORE_PHYSICAL, address);
        TEST_ASSERT_TRUE(routes.find(1, CommunicationRouteScope::LOCAL_DEVICE, address));
        TEST_ASSERT_EQUAL_UINT16(LAMP_PHYSICAL, address);

        Message assignment = {};
        TEST_ASSERT_TRUE(makeCoreIdAssignedMessage(5678, 9876, 1, assignment));
        routes.clear();
        routes.remember(assignment, CORE_PHYSICAL);
        TEST_ASSERT_TRUE(routes.find(1, CommunicationRouteScope::CORE, address));
        TEST_ASSERT_EQUAL_UINT16(CORE_PHYSICAL, address);

        Message coreTopologyAck = {};
        TEST_ASSERT_TRUE(makeCoreIdAckMessage(5678, 9876, true, coreTopologyAck));
        TEST_ASSERT_EQUAL_INT(static_cast<int>(CommunicationRouteScope::CORE),
            static_cast<int>(ZigbeeRouteTable::destinationScope(coreTopologyAck)));

        routes.remember(lamp, LAMP_PHYSICAL);
        Message legacyAck = {};
        legacyAck.type = MessageType::ACK;
        legacyAck.sourceId = 1;
        legacyAck.destinationId = 1;
        TEST_ASSERT_EQUAL_INT(static_cast<int>(CommunicationRouteScope::LOCAL_DEVICE),
            static_cast<int>(ZigbeeRouteTable::destinationScope(legacyAck)));
        TEST_ASSERT_TRUE(routes.find(legacyAck, address));
        TEST_ASSERT_EQUAL_UINT16(LAMP_PHYSICAL, address);
    }
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_set_power_traverses_uart_main_lamp_and_execution_ack_returns);
    RUN_TEST(test_receipt_ack_is_non_terminal_and_does_not_postpone_retry);
    RUN_TEST(test_lost_command_retries_full_uint32_id_and_executes_once);
    RUN_TEST(test_lost_lamp_ack_retries_without_second_power_execution);
    RUN_TEST(test_lost_main_to_core_ack_replays_cached_execution_result);
    RUN_TEST(test_lost_uart_execution_ack_retries_entire_path_with_same_identity);
    RUN_TEST(test_duplicate_message_id_rejects_resubmit_and_wire_replay_never_reexecutes);
    RUN_TEST(test_distinct_message_ids_execute_on_and_off_independently);
    RUN_TEST(test_core_refuses_offline_lamp_or_main_without_transmitting);
    RUN_TEST(test_main_offline_lamp_returns_failure_without_receipt_or_execution);
    RUN_TEST(test_lamp_offline_returns_failure_after_main_acceptance);
    RUN_TEST(test_unknown_destination_is_clean_local_error_and_main_disappearance_is_reported);
    RUN_TEST(test_invalid_registry_parent_and_bridge_main_mapping_are_rejected);
    RUN_TEST(test_main_rejects_lamp_paired_to_different_main);
    RUN_TEST(test_wrong_ack_provenance_target_correlation_and_result_are_ignored);
    RUN_TEST(test_partial_ack_cannot_complete_indivisible_power_command);
    RUN_TEST(test_command_loss_expires_after_initial_send_and_two_retries);
    RUN_TEST(test_late_ack_does_not_rewrite_expired_command_or_claim_success);
    RUN_TEST(test_uart_and_radio_transport_failures_never_report_execution);
    RUN_TEST(test_tracker_capacity_refuses_new_command_without_sending_it);
    RUN_TEST(test_full_dedup_cache_protects_retry_and_refuses_execution_until_expiry);
    RUN_TEST(test_dedup_protection_survives_millis_wraparound);
    RUN_TEST(test_lost_lamp_to_main_ack_retries_without_second_execution);
    RUN_TEST(test_console_ack_loss_diagnostic_is_consumed_only_by_core_execution_ack);
    RUN_TEST(test_long_core_pause_expires_without_retry_after_dedup_window);
    RUN_TEST(test_message_id_seed_preserves_32_bits_and_skips_zero_on_wrap);
    RUN_TEST(test_ack_arriving_after_deadline_before_poll_cannot_claim_execution);
    RUN_TEST(test_legacy_insertions_cannot_evict_protected_core_results);
    RUN_TEST(test_core_one_and_lamp_local_one_route_to_distinct_physical_peers_with_retry);
    RUN_TEST(test_missing_core_scope_never_falls_back_to_lamp_with_same_local_id);
    RUN_TEST(test_pairing_hardware_route_survives_assignment_of_local_id_one);
    RUN_TEST(test_full_physical_route_cache_preserves_core_and_lamp_collision_routes);
    RUN_TEST(test_core_and_v5_routes_are_learned_in_distinct_scopes_from_messages);
    return UNITY_END();
}
