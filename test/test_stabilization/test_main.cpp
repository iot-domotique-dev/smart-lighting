#include <unity.h>

#include "action_executor.h"
#include "automation_context.h"
#include "automation_engine.h"
#include "automation_manager.h"
#include "communication.h"
#include "communication_transport.h"
#include "core_module_service.h"
#include "device_manager.h"
#include "device_registry.h"
#include "event_bus.h"
#include "group_manager.h"
#include "lamp_controller.h"
#include "lamp_hardware.h"
#include "lamp_pwm_mapping.h"
#include "message_deduplicator.h"
#include "message_router.h"
#include "message_tracker.h"
#include "provisioning_store.h"
#include "scene_manager.h"

extern "C" void setUp(void) { configureLampHardwareSimulation(false); }
extern "C" void tearDown(void) { configureLampHardwareSimulation(false); }

FakeHardwareSerial Serial;

static uint32_t fakeNow = 0;
static Message incomingMessages[10];
static uint8_t incomingCount = 0;
static uint8_t incomingIndex = 0;
static uint8_t sentCount = 0;
static bool sendSucceeds = true;
static Message lastSentMessage = {};

class TransitionCaptureTransport : public CommunicationTransportInterface {
public:
    Message sent[8] = {};
    uint32_t hops[8] = {};
    CommunicationRouteScope scopes[8] = {};
    uint8_t count = 0;
    bool ready = true;

    bool begin() override { return true; }
    bool send(const Message& message) override {
        if (count >= 8) return false;
        sent[count] = message;
        hops[count] = 0;
        scopes[count] = CommunicationRouteScope::LOCAL_DEVICE;
        ++count;
        return true;
    }
    bool sendVia(const Message& message, uint32_t hop,
                 CommunicationRouteScope scope) override {
        if (count >= 8) return false;
        sent[count] = message;
        hops[count] = hop;
        scopes[count] = scope;
        ++count;
        return true;
    }
    bool receive(Message&) override { return false; }
    bool isReady() const override { return ready; }
    const char* name() const override { return "transition-test"; }
};

uint32_t millis() {
    return fakeNow;
}

bool receiveMessage(Communication&, Message& message) {
    if (incomingIndex >= incomingCount) {
        return false;
    }

    message = incomingMessages[incomingIndex++];
    return true;
}

bool sendMessage(Communication&, Message message) {
    sentCount++;
    lastSentMessage = message;
    return sendSucceeds;
}

static void resetMessages() {
    incomingCount = 0;
    incomingIndex = 0;
    sentCount = 0;
    sendSucceeds = true;
    lastSentMessage = {};
}

static void queueMessage(const Message& message) {
    incomingMessages[incomingCount++] = message;
}

static Message makeMessage(
    uint32_t id,
    uint32_t sourceId,
    uint32_t destinationId,
    MessageType type = MessageType::COMMAND
) {
    Message message = {};
    message.id = id;
    message.sourceId = sourceId;
    message.destinationId = destinationId;
    message.type = type;
    message.timestamp = fakeNow;
    message.status = MessageStatus::PENDING;
    message.executionStatus = ExecutionStatus::NOT_EXECUTED;
    return message;
}

static Message makeAck(uint32_t originalId) {
    Message ack = makeMessage(9000, 2, 1, MessageType::ACK);
    ack.value2 = static_cast<int32_t>(originalId);
    ack.executionStatus = ExecutionStatus::EXECUTED;
    return ack;
}

static Lamp makeLamp(
    uint32_t id,
    DeviceStatus status = DeviceStatus::ONLINE
) {
    Lamp lamp = {};
    lamp.device.id = id;
    lamp.device.name = "TEST_LAMP";
    lamp.device.role = DeviceRole::LAMP;
    lamp.device.status = status;
    lamp.device.lastSeen = fakeNow;
    return lamp;
}

static Message makeCoreLampCommand(uint32_t id, int32_t commandType,
                                   int32_t value) {
    constexpr uint32_t mainLocalId = 0x12345678;
    const uint32_t coreMainId = makeCoreModuleId(CORE_LOGICAL_ID, mainLocalId);
    Message command = makeMessage(
        id, CORE_LOGICAL_ID, makeCoreModuleId(coreMainId, 1));
    command.commandType = commandType;
    command.value = value;
    command.parentMainId = coreMainId;
    command.provisioningDeviceId = 1;
    return command;
}

static void prepareCoreLamp(LampRegistry& lamps) {
    initLampRegistry(lamps);
    Lamp lamp = makeLamp(1);
    lamp.device.capabilities = DEVICE_CAP_POWER | DEVICE_CAP_BRIGHTNESS;
    lamp.identity.parentMainId = 0x12345678;
    lamp.identity.pairingState = PairingState::PAIRED;
    TEST_ASSERT_TRUE(addLamp(lamps, lamp));
}

static void prepareTransitionRegistries(LampRegistry& lamps,
                                        MessageTracker& tracker,
                                        MessageDeduplicator& deduplicator,
                                        GroupRegistry& groups,
                                        SceneRegistry& scenes) {
    initLampRegistry(lamps);
    initMessageTracker(tracker);
    initMessageDeduplicator(deduplicator);
    initGroupRegistry(groups);
    initSceneRegistry(scenes);
    TEST_ASSERT_TRUE(addLamp(lamps, makeLamp(1)));
    resetMessages();
}

static Automation makePresenceAutomation(
    uint32_t id,
    AutomationTriggerMode mode
) {
    Automation automation = {};
    automation.id = id;
    automation.name = "presence";
    automation.enabled = true;
    automation.conditionCount = 1;
    automation.conditions[0] = {
        AutomationConditionType::PRESENCE,
        AutomationOperator::EQUAL,
        1
    };
    automation.logic = AutomationLogic::AND;
    automation.triggerMode = mode;
    return automation;
}

void test_lamp_group_scene_registry_capacities() {
    LampRegistry lamps;
    initLampRegistry(lamps);
    for (uint32_t i = 0; i < MAX_LAMPS; i++) {
        TEST_ASSERT_TRUE(addLamp(lamps, makeLamp(i + 1)));
    }
    TEST_ASSERT_FALSE(addLamp(lamps, makeLamp(MAX_LAMPS + 1)));
    TEST_ASSERT_EQUAL_UINT8(MAX_LAMPS, lamps.count);

    GroupRegistry groups;
    initGroupRegistry(groups);
    for (uint32_t i = 0; i < MAX_GROUPS; i++) {
        LampGroup group = {};
        group.id = i + 1;
        group.name = "group";
        TEST_ASSERT_TRUE(addGroup(groups, group));
    }
    LampGroup extraGroup = {};
    extraGroup.id = MAX_GROUPS + 1;
    extraGroup.name = "extra";
    TEST_ASSERT_FALSE(addGroup(groups, extraGroup));
    TEST_ASSERT_EQUAL_UINT8(MAX_GROUPS, groups.count);

    SceneRegistry scenes;
    initSceneRegistry(scenes);
    for (uint32_t i = 0; i < MAX_SCENES; i++) {
        Scene scene = {};
        scene.id = i + 1;
        scene.name = "scene";
        TEST_ASSERT_TRUE(addScene(scenes, scene));
    }
    Scene extraScene = {};
    extraScene.id = MAX_SCENES + 1;
    extraScene.name = "extra";
    TEST_ASSERT_FALSE(addScene(scenes, extraScene));
    TEST_ASSERT_EQUAL_UINT8(MAX_SCENES, scenes.count);
}

void test_pwm_simulation_brightness_values_and_restore_after_power_off() {
    Lamp lamp = makeLamp(1);
    configureLampHardwareSimulation(true);

    const uint8_t values[] = {0, 1, 50, 100};
    for (uint8_t value : values) {
        TEST_ASSERT_TRUE(setLampBrightness(lamp, value));
        TEST_ASSERT_EQUAL_UINT8(value, lamp.state.brightness);
        TEST_ASSERT_EQUAL_UINT8(value, simulatedLampHardwareOutput());
        TEST_ASSERT_EQUAL(lamp.state.power, value != 0);
    }

    TEST_ASSERT_EQUAL_UINT8(100, lamp.state.lastNonzeroBrightness);
    TEST_ASSERT_TRUE(setLampPower(lamp, false));
    TEST_ASSERT_EQUAL_UINT8(0, simulatedLampHardwareOutput());
    TEST_ASSERT_EQUAL_UINT8(100, lamp.state.lastNonzeroBrightness);
    TEST_ASSERT_TRUE(setLampPower(lamp, true));
    TEST_ASSERT_EQUAL_UINT8(100, simulatedLampHardwareOutput());

    TEST_ASSERT_TRUE(setLampBrightness(lamp, 65));
    TEST_ASSERT_TRUE(setLampBrightness(lamp, 0));
    TEST_ASSERT_EQUAL_UINT8(0, simulatedLampHardwareOutput());
    TEST_ASSERT_EQUAL_UINT8(65, lamp.state.lastNonzeroBrightness);
    TEST_ASSERT_TRUE(setLampPower(lamp, true));
    TEST_ASSERT_EQUAL_UINT8(65, simulatedLampHardwareOutput());

    const uint8_t oldLevel = lamp.state.brightness;
    TEST_ASSERT_FALSE(setLampBrightness(lamp, -1));
    TEST_ASSERT_FALSE(setLampBrightness(lamp, 101));
    TEST_ASSERT_EQUAL_UINT8(oldLevel, lamp.state.brightness);
}

void test_pwm_zero_off_and_full_scale_reaches_configured_allowed_ceiling() {
    TEST_ASSERT_EQUAL_INT(0, lampPwmOffLevel(false));
    TEST_ASSERT_EQUAL_INT(1, lampPwmOffLevel(true));
    TEST_ASSERT_EQUAL_INT(1, lampPwmPowerLevel(false, true));
    TEST_ASSERT_EQUAL_INT(0, lampPwmPowerLevel(false, false));
    TEST_ASSERT_EQUAL_INT(0, lampPwmPowerLevel(true, true));
    TEST_ASSERT_EQUAL_INT(1, lampPwmPowerLevel(true, false));
    TEST_ASSERT_FALSE(lampPwmOutputInvert(false));
    TEST_ASSERT_TRUE(lampPwmOutputInvert(true));
    TEST_ASSERT_EQUAL_UINT32(0, lampPwmDutyFromPercent(1023, 25, 0));
    TEST_ASSERT_EQUAL_UINT32(128, lampPwmDutyFromPercent(1023, 25, 50));
    TEST_ASSERT_EQUAL_UINT32(256, lampPwmDutyFromPercent(1023, 25, 100));
    TEST_ASSERT_EQUAL_UINT32(1, lampPwmDutyFromPercent(1023, 1, 1));

    configureLampHardwareSimulation(true, 25);
    Lamp lamp = makeLamp(1);
    TEST_ASSERT_TRUE(setLampBrightness(lamp, 0));
    TEST_ASSERT_EQUAL_UINT8(0, simulatedLampHardwareOutput());
    TEST_ASSERT_TRUE(setLampBrightness(lamp, 50));
    TEST_ASSERT_EQUAL_UINT8(13, simulatedLampHardwareOutput());
    TEST_ASSERT_EQUAL_UINT8(50, lamp.state.brightness);
    TEST_ASSERT_TRUE(setLampBrightness(lamp, 100));
    TEST_ASSERT_EQUAL_UINT8(25, simulatedLampHardwareOutput());
    TEST_ASSERT_EQUAL_UINT8(100, lamp.state.brightness);
    TEST_ASSERT_TRUE(setLampPower(lamp, false));
    TEST_ASSERT_EQUAL_UINT8(0, simulatedLampHardwareOutput());
    TEST_ASSERT_TRUE(setLampPower(lamp, true));
    TEST_ASSERT_EQUAL_UINT8(25, simulatedLampHardwareOutput());
}

void test_brightness_refused_without_pwm_and_after_hardware_failure() {
    Lamp lamp = makeLamp(1);
    configureLampHardwareSimulation(false);
    TEST_ASSERT_FALSE(lampHardwareSupportsBrightness());
    TEST_ASSERT_FALSE(setLampBrightness(lamp, 50));
    TEST_ASSERT_EQUAL_UINT8(0, lamp.state.brightness);

    LampRegistry lamps;
    initLampRegistry(lamps);
    TEST_ASSERT_TRUE(addLamp(lamps, lamp));
    GroupRegistry groups;
    initGroupRegistry(groups);
    SceneRegistry scenes;
    initSceneRegistry(scenes);
    Action brightnessAction = {ActionType::SET_LAMP_BRIGHTNESS, 1, 50};
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ExecutionStatus::FAILED),
        static_cast<int>(executeAction(brightnessAction, scenes, groups, lamps)));

    configureLampHardwareSimulation(true);
    MemoryProvisioningStore settingsStore;
    Lamp* simulatedLamp = findLamp(lamps, 1);
    TEST_ASSERT_NOT_NULL(simulatedLamp);
    restoreLampSettings(*simulatedLamp, settingsStore);
    failNextLampHardwareWrite();
    TEST_ASSERT_FALSE(setLampBrightness(*simulatedLamp, 50));
    TEST_ASSERT_EQUAL_UINT8(0, simulatedLamp->state.brightness);
    TEST_ASSERT_EQUAL_UINT8(0, simulatedLampHardwareOutput());

    brightnessAction.value = 300;
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ExecutionStatus::FAILED),
        static_cast<int>(executeAction(brightnessAction, scenes, groups, lamps)));
}

void test_pwm_brightness_persists_and_restores_after_cold_reboot() {
    configureLampHardwareSimulation(true);
    MemoryProvisioningStore settingsStore;
    Lamp beforeReboot = makeLamp(1);
    restoreLampSettings(beforeReboot, settingsStore);
    TEST_ASSERT_EQUAL_UINT8(100, beforeReboot.state.lastNonzeroBrightness);
    TEST_ASSERT_EQUAL_UINT8(0, simulatedLampHardwareOutput());

    TEST_ASSERT_TRUE(setLampBrightness(beforeReboot, 35));
    TEST_ASSERT_EQUAL_UINT8(35, simulatedLampHardwareOutput());
    TEST_ASSERT_EQUAL_UINT32(1, settingsStore.getBrightnessWriteAttemptCount());
    TEST_ASSERT_TRUE(setLampPower(beforeReboot, false));
    TEST_ASSERT_EQUAL_UINT8(0, simulatedLampHardwareOutput());
    TEST_ASSERT_TRUE(setLampPower(beforeReboot, true));
    TEST_ASSERT_EQUAL_UINT8(35, simulatedLampHardwareOutput());
    TEST_ASSERT_EQUAL_UINT32(1, settingsStore.getBrightnessWriteAttemptCount());
    TEST_ASSERT_TRUE(setLampBrightness(beforeReboot, 0));
    TEST_ASSERT_EQUAL_UINT8(0, simulatedLampHardwareOutput());
    TEST_ASSERT_EQUAL_UINT8(35, beforeReboot.state.lastNonzeroBrightness);
    TEST_ASSERT_EQUAL_UINT32(1, settingsStore.getBrightnessWriteAttemptCount());
    TEST_ASSERT_TRUE(setLampBrightness(beforeReboot, 35));
    TEST_ASSERT_EQUAL_UINT32(1, settingsStore.getBrightnessWriteAttemptCount());

    initializeLampHardware();
    TEST_ASSERT_EQUAL_UINT8(0, simulatedLampHardwareOutput());
    Lamp afterReboot = makeLamp(1);
    restoreLampSettings(afterReboot, settingsStore);
    TEST_ASSERT_FALSE(afterReboot.state.power);
    TEST_ASSERT_EQUAL_UINT8(0, afterReboot.state.brightness);
    TEST_ASSERT_EQUAL_UINT8(35, afterReboot.state.lastNonzeroBrightness);
    TEST_ASSERT_EQUAL_UINT8(0, simulatedLampHardwareOutput());
    TEST_ASSERT_TRUE(setLampPower(afterReboot, true));
    TEST_ASSERT_EQUAL_UINT8(35, simulatedLampHardwareOutput());
    TEST_ASSERT_EQUAL_UINT32(1, settingsStore.getBrightnessWriteAttemptCount());
}

void test_pwm_missing_invalid_and_unreadable_settings_use_safe_ram_default() {
    configureLampHardwareSimulation(true);
    MemoryProvisioningStore settingsStore;
    Lamp lamp = makeLamp(1);

    restoreLampSettings(lamp, settingsStore);
    TEST_ASSERT_EQUAL_UINT8(100, lamp.state.lastNonzeroBrightness);
    TEST_ASSERT_EQUAL_UINT8(0, simulatedLampHardwareOutput());

    settingsStore.setStoredBrightnessForTest(0);
    Lamp invalidZero = makeLamp(2);
    restoreLampSettings(invalidZero, settingsStore);
    TEST_ASSERT_EQUAL_UINT8(100, invalidZero.state.lastNonzeroBrightness);
    TEST_ASSERT_EQUAL_UINT8(0, simulatedLampHardwareOutput());

    settingsStore.setStoredBrightnessForTest(101);
    Lamp invalidHigh = makeLamp(3);
    restoreLampSettings(invalidHigh, settingsStore);
    TEST_ASSERT_EQUAL_UINT8(100, invalidHigh.state.lastNonzeroBrightness);
    TEST_ASSERT_EQUAL_UINT8(0, simulatedLampHardwareOutput());

    settingsStore.setFailBrightnessReads(true);
    Lamp unreadable = makeLamp(4);
    restoreLampSettings(unreadable, settingsStore);
    TEST_ASSERT_EQUAL_UINT8(100, unreadable.state.lastNonzeroBrightness);
    TEST_ASSERT_EQUAL_UINT8(0, simulatedLampHardwareOutput());
    TEST_ASSERT_TRUE(setLampBrightness(unreadable, 50));
    TEST_ASSERT_EQUAL_UINT8(50, simulatedLampHardwareOutput());
    TEST_ASSERT_EQUAL_UINT32(0, settingsStore.getBrightnessWriteAttemptCount());

    settingsStore.setFailBrightnessReads(false);
    TEST_ASSERT_TRUE(setLampBrightness(unreadable, 50));
    TEST_ASSERT_EQUAL_UINT32(1, settingsStore.getBrightnessWriteAttemptCount());
}

void test_pwm_write_failure_keeps_applied_state_and_old_saved_value() {
    configureLampHardwareSimulation(true);
    MemoryProvisioningStore settingsStore;
    TEST_ASSERT_TRUE(settingsStore.saveLastNonzeroBrightness(40));
    Lamp lamp = makeLamp(1);
    restoreLampSettings(lamp, settingsStore);

    settingsStore.setFailWrites(true);
    TEST_ASSERT_TRUE(setLampBrightness(lamp, 60));
    TEST_ASSERT_EQUAL_UINT8(60, simulatedLampHardwareOutput());
    TEST_ASSERT_EQUAL_UINT8(60, lamp.state.lastNonzeroBrightness);
    uint8_t stored = 0;
    TEST_ASSERT_EQUAL_INT(static_cast<int>(LampBrightnessLoadResult::FOUND),
        static_cast<int>(settingsStore.loadLastNonzeroBrightness(stored)));
    TEST_ASSERT_EQUAL_UINT8(40, stored);
    TEST_ASSERT_EQUAL_UINT32(2, settingsStore.getBrightnessWriteAttemptCount());

    LampRegistry lamps;
    GroupRegistry groups;
    SceneRegistry scenes;
    initLampRegistry(lamps);
    initGroupRegistry(groups);
    initSceneRegistry(scenes);
    TEST_ASSERT_TRUE(addLamp(lamps, lamp));
    const Action brightnessAction = {
        ActionType::SET_LAMP_BRIGHTNESS, 1, 60
    };
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ExecutionStatus::EXECUTED),
        static_cast<int>(executeAction(brightnessAction, scenes, groups, lamps)));
    TEST_ASSERT_EQUAL_UINT32(3, settingsStore.getBrightnessWriteAttemptCount());

    settingsStore.setFailWrites(false);
    TEST_ASSERT_TRUE(setLampBrightness(lamp, 60));
    TEST_ASSERT_EQUAL_UINT8(60, simulatedLampHardwareOutput());
    TEST_ASSERT_EQUAL_UINT8(60, lamp.state.lastNonzeroBrightness);
    TEST_ASSERT_EQUAL_UINT32(4, settingsStore.getBrightnessWriteAttemptCount());
}

void test_message_tracker_ack_retry_timeout_and_reuse() {
    MessageTracker tracker;
    initMessageTracker(tracker);
    Communication communication = {};
    fakeNow = 100;

    Message command = makeMessage(1, 10, 20);
    TEST_ASSERT_TRUE(trackMessage(tracker, command));
    TEST_ASSERT_TRUE(processAck(tracker, makeAck(1)));
    TEST_ASSERT_TRUE(findPendingMessage(tracker, 1)->completed);
    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(MessageStatus::DELIVERED),
        static_cast<int>(findPendingMessage(tracker, 1)->message.status)
    );

    Message timeoutCommand = makeMessage(2, 10, 20);
    TEST_ASSERT_TRUE(trackMessage(tracker, timeoutCommand));
    sentCount = 0;
    fakeNow += MESSAGE_TIMEOUT;
    TEST_ASSERT_TRUE(updateMessageTimeouts(tracker, communication));
    TEST_ASSERT_EQUAL_UINT8(1, findPendingMessage(tracker, 2)->retryCount);
    TEST_ASSERT_EQUAL_UINT8(1, sentCount);

    fakeNow += MESSAGE_TIMEOUT;
    TEST_ASSERT_TRUE(updateMessageTimeouts(tracker, communication));
    TEST_ASSERT_EQUAL_UINT8(2, findPendingMessage(tracker, 2)->retryCount);
    fakeNow += MESSAGE_TIMEOUT;
    TEST_ASSERT_TRUE(updateMessageTimeouts(tracker, communication));
    TEST_ASSERT_TRUE(findPendingMessage(tracker, 2)->timedOut);
    TEST_ASSERT_FALSE(findPendingMessage(tracker, 2)->waitingForAck);

    TEST_ASSERT_TRUE(untrackMessage(tracker, 1));
    TEST_ASSERT_NULL(findPendingMessage(tracker, 1));
    TEST_ASSERT_EQUAL_UINT8(1, tracker.count);

    TEST_ASSERT_TRUE(trackMessage(tracker, makeMessage(3, 10, 20)));
    TEST_ASSERT_TRUE(untrackMessage(tracker, 3));
    TEST_ASSERT_NULL(findPendingMessage(tracker, 3));
}

void test_tracker_reuses_completed_entry_but_respects_pending_capacity() {
    MessageTracker tracker;
    initMessageTracker(tracker);

    for (uint32_t id = 1; id <= MAX_PENDING_MESSAGES; id++) {
        TEST_ASSERT_TRUE(trackMessage(tracker, makeMessage(id, 10, 20)));
    }
    TEST_ASSERT_FALSE(trackMessage(
        tracker,
        makeMessage(MAX_PENDING_MESSAGES + 1, 10, 20)
    ));

    TEST_ASSERT_TRUE(processAck(tracker, makeAck(1)));
    TEST_ASSERT_TRUE(trackMessage(
        tracker,
        makeMessage(MAX_PENDING_MESSAGES + 1, 10, 20)
    ));
    TEST_ASSERT_EQUAL_UINT8(MAX_PENDING_MESSAGES, tracker.count);
    TEST_ASSERT_NULL(findPendingMessage(tracker, 1));
    TEST_ASSERT_NOT_NULL(findPendingMessage(tracker, MAX_PENDING_MESSAGES + 1));
}

void test_deduplicator_uses_source_and_message_id_and_evicts_oldest() {
    MessageDeduplicator deduplicator;
    initMessageDeduplicator(deduplicator);

    TEST_ASSERT_TRUE(registerProcessedMessage(
        deduplicator, 11, 5, ExecutionStatus::EXECUTED
    ));
    TEST_ASSERT_TRUE(registerProcessedMessage(
        deduplicator, 22, 5, ExecutionStatus::FAILED
    ));
    TEST_ASSERT_NOT_NULL(findProcessedMessage(deduplicator, 11, 5));
    TEST_ASSERT_NOT_NULL(findProcessedMessage(deduplicator, 22, 5));
    TEST_ASSERT_FALSE(registerProcessedMessage(
        deduplicator, 11, 5, ExecutionStatus::EXECUTED
    ));

    initMessageDeduplicator(deduplicator);
    for (uint32_t id = 1; id <= MAX_PROCESSED_MESSAGES + 1; id++) {
        TEST_ASSERT_TRUE(registerProcessedMessage(
            deduplicator, 42, id, ExecutionStatus::EXECUTED
        ));
    }
    TEST_ASSERT_NULL(findProcessedMessage(deduplicator, 42, 1));
    TEST_ASSERT_NOT_NULL(findProcessedMessage(deduplicator, 42, 2));
    TEST_ASSERT_NOT_NULL(findProcessedMessage(
        deduplicator, 42, MAX_PROCESSED_MESSAGES + 1
    ));
}

void test_message_router_executes_distinct_sources_and_suppresses_replay() {
    MessageTracker tracker;
    MessageDeduplicator deduplicator;
    LampRegistry lamps;
    GroupRegistry groups;
    SceneRegistry scenes;
    Communication communication = {};
    initMessageTracker(tracker);
    initMessageDeduplicator(deduplicator);
    initLampRegistry(lamps);
    initGroupRegistry(groups);
    initSceneRegistry(scenes);
    TEST_ASSERT_TRUE(addLamp(lamps, makeLamp(1)));
    resetMessages();

    Message first = makeMessage(5, 11, 1);
    first.commandType = static_cast<int32_t>(ActionType::SET_LAMP_POWER);
    first.value = 1;
    queueMessage(first);
    processMessages(communication, tracker, deduplicator, lamps, groups, scenes);
    TEST_ASSERT_TRUE(findLamp(lamps, 1)->state.power);

    Message second = makeMessage(5, 22, 1);
    second.commandType = static_cast<int32_t>(ActionType::SET_LAMP_POWER);
    second.value = 0;
    queueMessage(second);
    processMessages(communication, tracker, deduplicator, lamps, groups, scenes);
    TEST_ASSERT_FALSE(findLamp(lamps, 1)->state.power);
    TEST_ASSERT_NOT_NULL(findProcessedMessage(deduplicator, 11, 5));
    TEST_ASSERT_NOT_NULL(findProcessedMessage(deduplicator, 22, 5));

    first.value = 1;
    queueMessage(first);
    processMessages(communication, tracker, deduplicator, lamps, groups, scenes);
    TEST_ASSERT_FALSE(findLamp(lamps, 1)->state.power);
}

void test_ledc_fade_duration_scales_for_all_requested_ranges() {
    configureLampHardwareSimulation(true);
    Lamp lamp = makeLamp(1);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(LampOutputTransitionResult::STARTED),
        static_cast<int>(beginLampOutputTransition(
            lamp, ActionType::SET_LAMP_BRIGHTNESS, 100, 11, 1, false, false)));
    TEST_ASSERT_EQUAL_UINT16(1000, simulatedLampHardwareFadeDuration());
    simulateLampHardwareFadeCompletion();
    TEST_ASSERT_EQUAL_INT(static_cast<int>(LampOutputTransitionPollResult::EXECUTED),
        static_cast<int>(pollLampOutputTransition(lamp, fakeNow)));
    TEST_ASSERT_EQUAL_UINT8(100, lamp.state.brightness);

    TEST_ASSERT_EQUAL_INT(static_cast<int>(LampOutputTransitionResult::STARTED),
        static_cast<int>(beginLampOutputTransition(
            lamp, ActionType::SET_LAMP_BRIGHTNESS, 0, 11, 2, false, false)));
    TEST_ASSERT_EQUAL_UINT16(1000, simulatedLampHardwareFadeDuration());
    simulateLampHardwareFadeCompletion();
    TEST_ASSERT_EQUAL_INT(static_cast<int>(LampOutputTransitionPollResult::EXECUTED),
        static_cast<int>(pollLampOutputTransition(lamp, fakeNow)));
    TEST_ASSERT_EQUAL_UINT8(0, lamp.state.brightness);

    TEST_ASSERT_TRUE(setLampBrightness(lamp, 20));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(LampOutputTransitionResult::STARTED),
        static_cast<int>(beginLampOutputTransition(
            lamp, ActionType::SET_LAMP_BRIGHTNESS, 80, 11, 3, false, false)));
    TEST_ASSERT_EQUAL_UINT16(600, simulatedLampHardwareFadeDuration());
    simulateLampHardwareFadeCompletion();
    TEST_ASSERT_EQUAL_INT(static_cast<int>(LampOutputTransitionPollResult::EXECUTED),
        static_cast<int>(pollLampOutputTransition(lamp, fakeNow)));

    TEST_ASSERT_EQUAL_INT(static_cast<int>(LampOutputTransitionResult::STARTED),
        static_cast<int>(beginLampOutputTransition(
            lamp, ActionType::SET_LAMP_BRIGHTNESS, 30, 11, 4, false, false)));
    TEST_ASSERT_EQUAL_UINT16(500, simulatedLampHardwareFadeDuration());
    simulateLampHardwareFadeCompletion();
    TEST_ASSERT_EQUAL_INT(static_cast<int>(LampOutputTransitionPollResult::EXECUTED),
        static_cast<int>(pollLampOutputTransition(lamp, fakeNow)));
    TEST_ASSERT_EQUAL_UINT8(30, lamp.state.brightness);
}

void test_ledc_fade_uses_minimum_duration_for_small_changes() {
    configureLampHardwareSimulation(true);
    Lamp lamp = makeLamp(1);
    TEST_ASSERT_TRUE(setLampBrightness(lamp, 50));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(LampOutputTransitionResult::STARTED),
        static_cast<int>(beginLampOutputTransition(
            lamp, ActionType::SET_LAMP_BRIGHTNESS, 51, 11, 5, false, false)));
    TEST_ASSERT_EQUAL_UINT16(100, simulatedLampHardwareFadeDuration());
}

void test_ledc_power_on_restores_last_positive_level_with_fade() {
    configureLampHardwareSimulation(true);
    MemoryProvisioningStore settingsStore;
    TEST_ASSERT_TRUE(settingsStore.saveLastNonzeroBrightness(35));
    const uint32_t writesBeforeFade =
        settingsStore.getBrightnessWriteAttemptCount();
    Lamp lamp = makeLamp(1);
    restoreLampSettings(lamp, settingsStore);
    TEST_ASSERT_FALSE(lamp.state.power);
    TEST_ASSERT_EQUAL_UINT8(0, simulatedLampHardwareOutput());
    TEST_ASSERT_EQUAL_INT(static_cast<int>(LampOutputTransitionResult::STARTED),
        static_cast<int>(beginLampOutputTransition(
            lamp, ActionType::SET_LAMP_POWER, 1, 11, 6, false, false)));
    TEST_ASSERT_EQUAL_UINT16(350, simulatedLampHardwareFadeDuration());
    TEST_ASSERT_FALSE(lamp.state.power);
    simulateLampHardwareFadeCompletion();
    TEST_ASSERT_EQUAL_INT(static_cast<int>(LampOutputTransitionPollResult::EXECUTED),
        static_cast<int>(pollLampOutputTransition(lamp, fakeNow)));
    TEST_ASSERT_TRUE(lamp.state.power);
    TEST_ASSERT_EQUAL_UINT8(35, lamp.state.brightness);
    TEST_ASSERT_EQUAL_UINT32(writesBeforeFade,
                             settingsStore.getBrightnessWriteAttemptCount());
}

void test_ledc_power_off_is_immediate_after_transition_finishes() {
    configureLampHardwareSimulation(true);
    Lamp lamp = makeLamp(1);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(LampOutputTransitionResult::STARTED),
        static_cast<int>(beginLampOutputTransition(
            lamp, ActionType::SET_LAMP_BRIGHTNESS, 40, 11, 7, false, false)));
    simulateLampHardwareFadeCompletion();
    TEST_ASSERT_EQUAL_INT(static_cast<int>(LampOutputTransitionPollResult::EXECUTED),
        static_cast<int>(pollLampOutputTransition(lamp, fakeNow)));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(LampOutputTransitionResult::COMPLETED),
        static_cast<int>(beginLampOutputTransition(
            lamp, ActionType::SET_LAMP_POWER, 0, 11, 8, false, false)));
    TEST_ASSERT_FALSE(simulatedLampHardwareFadeActive());
    TEST_ASSERT_EQUAL_UINT8(0, simulatedLampHardwareOutput());
    TEST_ASSERT_FALSE(lamp.state.power);
}

void test_generic_main_command_ack_waits_for_fade_completion() {
    configureLampHardwareSimulation(true);
    fakeNow = 1000;
    MessageTracker tracker;
    MessageDeduplicator deduplicator;
    LampRegistry lamps;
    GroupRegistry groups;
    SceneRegistry scenes;
    Communication communication = {};
    prepareTransitionRegistries(lamps, tracker, deduplicator, groups, scenes);
    Message command = makeMessage(501, 22, 1);
    command.commandType = static_cast<int32_t>(ActionType::SET_LAMP_BRIGHTNESS);
    command.value = 70;
    queueMessage(command);
    processMessages(communication, tracker, deduplicator, lamps, groups, scenes);

    TEST_ASSERT_EQUAL_UINT8(0, sentCount);
    TEST_ASSERT_TRUE(findLamp(lamps, 1)->outputTransition.active);
    TEST_ASSERT_EQUAL_UINT8(0, findLamp(lamps, 1)->state.brightness);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ExecutionStatus::NOT_EXECUTED),
        static_cast<int>(findProcessedMessage(deduplicator, 22, 501)->executionStatus));

    simulateLampHardwareFadeCompletion();
    pollLampTransitions(communication, deduplicator, lamps);
    TEST_ASSERT_EQUAL_UINT8(1, sentCount);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ExecutionStatus::EXECUTED),
        static_cast<int>(lastSentMessage.executionStatus));
    TEST_ASSERT_EQUAL_UINT32(501, static_cast<uint32_t>(lastSentMessage.value2));
    TEST_ASSERT_EQUAL_UINT8(70, findLamp(lamps, 1)->state.brightness);
}

void test_pending_duplicate_does_not_restart_fade_or_send_terminal_ack() {
    configureLampHardwareSimulation(true);
    MessageTracker tracker;
    MessageDeduplicator deduplicator;
    LampRegistry lamps;
    GroupRegistry groups;
    SceneRegistry scenes;
    Communication communication = {};
    prepareTransitionRegistries(lamps, tracker, deduplicator, groups, scenes);
    Message command = makeMessage(502, 22, 1);
    command.commandType = static_cast<int32_t>(ActionType::SET_LAMP_BRIGHTNESS);
    command.value = 80;
    queueMessage(command);
    processMessages(communication, tracker, deduplicator, lamps, groups, scenes);
    TEST_ASSERT_TRUE(simulatedLampHardwareFadeActive());

    queueMessage(command);
    processMessages(communication, tracker, deduplicator, lamps, groups, scenes);
    TEST_ASSERT_TRUE(simulatedLampHardwareFadeActive());
    TEST_ASSERT_EQUAL_UINT8(0, sentCount);
    TEST_ASSERT_EQUAL_UINT16(800, simulatedLampHardwareFadeDuration());

    simulateLampHardwareFadeCompletion();
    pollLampTransitions(communication, deduplicator, lamps);
    TEST_ASSERT_EQUAL_UINT8(1, sentCount);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ExecutionStatus::EXECUTED),
        static_cast<int>(findProcessedMessage(deduplicator, 22, 502)->executionStatus));
}

void test_priority_off_interrupts_fade_and_retry_never_restarts_it() {
    configureLampHardwareSimulation(true);
    MessageTracker tracker;
    MessageDeduplicator deduplicator;
    LampRegistry lamps;
    GroupRegistry groups;
    SceneRegistry scenes;
    Communication communication = {};
    prepareTransitionRegistries(lamps, tracker, deduplicator, groups, scenes);
    MemoryProvisioningStore settingsStore;
    Lamp* lamp = findLamp(lamps, 1);
    TEST_ASSERT_NOT_NULL(lamp);
    restoreLampSettings(*lamp, settingsStore);
    const uint32_t writesBeforeFade =
        settingsStore.getBrightnessWriteAttemptCount();
    Message first = makeMessage(503, 22, 1);
    first.commandType = static_cast<int32_t>(ActionType::SET_LAMP_BRIGHTNESS);
    first.value = 60;
    queueMessage(first);
    processMessages(communication, tracker, deduplicator, lamps, groups, scenes);
    const uint32_t oldGeneration = lamp->outputTransition.generation;
    TEST_ASSERT_TRUE(oldGeneration != 0);

    Message second = makeMessage(504, 22, 1);
    second.commandType = static_cast<int32_t>(ActionType::SET_LAMP_POWER);
    second.value = 0;
    queueMessage(second);
    processMessages(communication, tracker, deduplicator, lamps, groups, scenes);
    TEST_ASSERT_FALSE(findLamp(lamps, 1)->outputTransition.active);
    TEST_ASSERT_FALSE(simulatedLampHardwareFadeActive());
    TEST_ASSERT_EQUAL_UINT8(0, simulatedLampHardwareOutput());
    TEST_ASSERT_FALSE(lamp->state.power);
    TEST_ASSERT_EQUAL_UINT32(writesBeforeFade,
                             settingsStore.getBrightnessWriteAttemptCount());
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ExecutionStatus::FAILED),
        static_cast<int>(findProcessedMessage(deduplicator, 22, 503)->executionStatus));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ExecutionStatus::EXECUTED),
        static_cast<int>(findProcessedMessage(deduplicator, 22, 504)->executionStatus));
    TEST_ASSERT_EQUAL_UINT8(2, sentCount);
    TEST_ASSERT_EQUAL_UINT32(504, static_cast<uint32_t>(lastSentMessage.value2));

    queueMessage(first);
    processMessages(communication, tracker, deduplicator, lamps, groups, scenes);
    TEST_ASSERT_EQUAL_UINT8(3, sentCount);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ExecutionStatus::FAILED),
        static_cast<int>(lastSentMessage.executionStatus));
    TEST_ASSERT_FALSE(simulatedLampHardwareFadeActive());
    TEST_ASSERT_EQUAL_UINT8(0, simulatedLampHardwareOutput());

    simulateLampHardwareFadeCallbackForGeneration(60, oldGeneration);
    pollLampTransitions(communication, deduplicator, lamps);
    TEST_ASSERT_EQUAL_UINT8(3, sentCount);
    TEST_ASSERT_EQUAL_UINT8(0, simulatedLampHardwareOutput());

    queueMessage(second);
    processMessages(communication, tracker, deduplicator, lamps, groups, scenes);
    TEST_ASSERT_EQUAL_UINT8(4, sentCount);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ExecutionStatus::EXECUTED),
        static_cast<int>(lastSentMessage.executionStatus));
    TEST_ASSERT_EQUAL_UINT8(0, simulatedLampHardwareOutput());
}

void test_priority_off_interrupts_fade_one_millisecond_before_deadline() {
    configureLampHardwareSimulation(true);
    MessageTracker tracker;
    MessageDeduplicator deduplicator;
    LampRegistry lamps;
    GroupRegistry groups;
    SceneRegistry scenes;
    Communication communication = {};
    prepareTransitionRegistries(lamps, tracker, deduplicator, groups, scenes);

    Message brightness = makeMessage(505, 22, 1);
    brightness.commandType =
        static_cast<int32_t>(ActionType::SET_LAMP_BRIGHTNESS);
    brightness.value = 100;
    queueMessage(brightness);
    processMessages(communication, tracker, deduplicator, lamps, groups, scenes);
    TEST_ASSERT_EQUAL_UINT16(1000, simulatedLampHardwareFadeDuration());
    TEST_ASSERT_TRUE(simulatedLampHardwareFadeActive());

    fakeNow += 999;
    pollLampTransitions(communication, deduplicator, lamps);
    TEST_ASSERT_TRUE(findLamp(lamps, 1)->outputTransition.active);
    TEST_ASSERT_EQUAL_UINT8(0, sentCount);

    Message off = makeMessage(506, 22, 1);
    off.commandType = static_cast<int32_t>(ActionType::SET_LAMP_POWER);
    off.value = 0;
    queueMessage(off);
    processMessages(communication, tracker, deduplicator, lamps, groups, scenes);

    TEST_ASSERT_FALSE(findLamp(lamps, 1)->outputTransition.active);
    TEST_ASSERT_FALSE(simulatedLampHardwareFadeActive());
    TEST_ASSERT_EQUAL_UINT8(0, simulatedLampHardwareOutput());
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ExecutionStatus::FAILED),
        static_cast<int>(findProcessedMessage(deduplicator, 22, 505)
            ->executionStatus));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ExecutionStatus::EXECUTED),
        static_cast<int>(findProcessedMessage(deduplicator, 22, 506)
            ->executionStatus));
}

void test_priority_off_after_fade_reached_target_reports_both_executed() {
    configureLampHardwareSimulation(true);
    MessageTracker tracker;
    MessageDeduplicator deduplicator;
    LampRegistry lamps;
    GroupRegistry groups;
    SceneRegistry scenes;
    Communication communication = {};
    prepareTransitionRegistries(lamps, tracker, deduplicator, groups, scenes);
    Message first = makeMessage(507, 22, 1);
    first.commandType = static_cast<int32_t>(ActionType::SET_LAMP_BRIGHTNESS);
    first.value = 55;
    queueMessage(first);
    processMessages(communication, tracker, deduplicator, lamps, groups, scenes);
    simulateLampHardwareReachTargetWithoutCallback();

    Message off = makeMessage(508, 22, 1);
    off.commandType = static_cast<int32_t>(ActionType::SET_LAMP_POWER);
    off.value = 0;
    queueMessage(off);
    processMessages(communication, tracker, deduplicator, lamps, groups, scenes);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ExecutionStatus::EXECUTED),
        static_cast<int>(findProcessedMessage(deduplicator, 22, 507)->executionStatus));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ExecutionStatus::EXECUTED),
        static_cast<int>(findProcessedMessage(deduplicator, 22, 508)->executionStatus));
    TEST_ASSERT_EQUAL_UINT8(2, sentCount);
    TEST_ASSERT_EQUAL_UINT8(0, simulatedLampHardwareOutput());
    TEST_ASSERT_FALSE(findLamp(lamps, 1)->state.power);
}

void test_priority_off_stop_failure_does_not_claim_output_is_off() {
    configureLampHardwareSimulation(true);
    MessageTracker tracker;
    MessageDeduplicator deduplicator;
    LampRegistry lamps;
    GroupRegistry groups;
    SceneRegistry scenes;
    Communication communication = {};
    prepareTransitionRegistries(lamps, tracker, deduplicator, groups, scenes);
    TEST_ASSERT_TRUE(setLampBrightness(*findLamp(lamps, 1), 20));
    Message first = makeMessage(509, 22, 1);
    first.commandType = static_cast<int32_t>(ActionType::SET_LAMP_BRIGHTNESS);
    first.value = 80;
    queueMessage(first);
    processMessages(communication, tracker, deduplicator, lamps, groups, scenes);
    failNextLampHardwareFadeStop();

    Message off = makeMessage(510, 22, 1);
    off.commandType = static_cast<int32_t>(ActionType::SET_LAMP_POWER);
    off.value = 0;
    queueMessage(off);
    processMessages(communication, tracker, deduplicator, lamps, groups, scenes);
    TEST_ASSERT_TRUE(findLamp(lamps, 1)->outputTransition.active);
    TEST_ASSERT_TRUE(simulatedLampHardwareFadeActive());
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ExecutionStatus::FAILED),
        static_cast<int>(findProcessedMessage(deduplicator, 22, 510)->executionStatus));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ExecutionStatus::FAILED),
        static_cast<int>(lastSentMessage.executionStatus));

    simulateLampHardwareFadeCompletion();
    pollLampTransitions(communication, deduplicator, lamps);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ExecutionStatus::EXECUTED),
        static_cast<int>(findProcessedMessage(deduplicator, 22, 509)->executionStatus));
    TEST_ASSERT_EQUAL_UINT8(80, simulatedLampHardwareOutput());
}

void test_priority_off_gpio_failure_withholds_success_and_marks_fault() {
    configureLampHardwareSimulation(true);
    MessageTracker tracker;
    MessageDeduplicator deduplicator;
    LampRegistry lamps;
    GroupRegistry groups;
    SceneRegistry scenes;
    Communication communication = {};
    prepareTransitionRegistries(lamps, tracker, deduplicator, groups, scenes);
    Lamp* lamp = findLamp(lamps, 1);
    TEST_ASSERT_NOT_NULL(lamp);
    TEST_ASSERT_TRUE(setLampBrightness(*lamp, 20));
    Message first = makeMessage(511, 22, 1);
    first.commandType = static_cast<int32_t>(ActionType::SET_LAMP_BRIGHTNESS);
    first.value = 75;
    queueMessage(first);
    processMessages(communication, tracker, deduplicator, lamps, groups, scenes);
    failNextLampHardwareWrite();

    Message off = makeMessage(512, 22, 1);
    off.commandType = static_cast<int32_t>(ActionType::SET_LAMP_POWER);
    off.value = 0;
    queueMessage(off);
    processMessages(communication, tracker, deduplicator, lamps, groups, scenes);
    TEST_ASSERT_FALSE(lamp->outputTransition.active);
    TEST_ASSERT_TRUE(lamp->outputTransition.faulted);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ExecutionStatus::FAILED),
        static_cast<int>(findProcessedMessage(deduplicator, 22, 511)->executionStatus));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ExecutionStatus::FAILED),
        static_cast<int>(findProcessedMessage(deduplicator, 22, 512)->executionStatus));
    TEST_ASSERT_EQUAL_UINT8(20, simulatedLampHardwareOutput());
}

void test_secure_removal_shutdown_interrupts_rising_fade_without_nvs_write() {
    configureLampHardwareSimulation(true);
    MessageTracker tracker;
    MessageDeduplicator deduplicator;
    LampRegistry lamps;
    GroupRegistry groups;
    SceneRegistry scenes;
    Communication communication = {};
    prepareTransitionRegistries(lamps, tracker, deduplicator, groups, scenes);
    MemoryProvisioningStore settingsStore;
    Lamp* lamp = findLamp(lamps, 1);
    TEST_ASSERT_NOT_NULL(lamp);
    restoreLampSettings(*lamp, settingsStore);
    const uint32_t writesBefore = settingsStore.getBrightnessWriteAttemptCount();

    Message command = makeMessage(513, 22, 1);
    command.commandType = static_cast<int32_t>(ActionType::SET_LAMP_BRIGHTNESS);
    command.value = 100;
    queueMessage(command);
    processMessages(communication, tracker, deduplicator, lamps, groups, scenes);
    const uint32_t oldGeneration = lamp->outputTransition.generation;
    TEST_ASSERT_TRUE(lamp->outputTransition.active);
    TEST_ASSERT_TRUE(simulatedLampHardwareFadeActive());

    LampOutputInterruption interruption = {};
    TEST_ASSERT_EQUAL_INT(static_cast<int>(LampOutputOffResult::APPLIED),
        static_cast<int>(forceLampOutputOff(*lamp, fakeNow, interruption)));
    TEST_ASSERT_TRUE(interruption.hadTransition);
    TEST_ASSERT_TRUE(interruption.transitionStopped);
    TEST_ASSERT_FALSE(interruption.transitionCompleted);
    TEST_ASSERT_FALSE(lamp->outputTransition.active);
    TEST_ASSERT_TRUE(lamp->outputGeneration != oldGeneration);
    TEST_ASSERT_FALSE(simulatedLampHardwareFadeActive());
    TEST_ASSERT_EQUAL_UINT8(0, simulatedLampHardwareOutput());
    TEST_ASSERT_FALSE(lamp->state.power);
    TEST_ASSERT_EQUAL_UINT8(writesBefore,
        settingsStore.getBrightnessWriteAttemptCount());

    const uint8_t sentBeforeRemoval = sentCount;
    TEST_ASSERT_TRUE(removeLamp(lamps, 1));
    simulateLampHardwareFadeCallbackForGeneration(100, oldGeneration);
    pollLampTransitions(communication, deduplicator, lamps);
    TEST_ASSERT_EQUAL_UINT8(sentBeforeRemoval, sentCount);
    TEST_ASSERT_EQUAL_UINT8(0, simulatedLampHardwareOutput());
}

void test_secure_removal_shutdown_interrupts_descending_fade_without_persisting_zero() {
    configureLampHardwareSimulation(true);
    MessageTracker tracker;
    MessageDeduplicator deduplicator;
    LampRegistry lamps;
    GroupRegistry groups;
    SceneRegistry scenes;
    Communication communication = {};
    prepareTransitionRegistries(lamps, tracker, deduplicator, groups, scenes);
    MemoryProvisioningStore settingsStore;
    Lamp* lamp = findLamp(lamps, 1);
    TEST_ASSERT_NOT_NULL(lamp);
    restoreLampSettings(*lamp, settingsStore);
    TEST_ASSERT_TRUE(setLampBrightness(*lamp, 80));
    const uint32_t writesBeforeFade = settingsStore.getBrightnessWriteAttemptCount();

    Message command = makeMessage(514, 22, 1);
    command.commandType = static_cast<int32_t>(ActionType::SET_LAMP_BRIGHTNESS);
    command.value = 20;
    queueMessage(command);
    processMessages(communication, tracker, deduplicator, lamps, groups, scenes);
    const uint32_t oldGeneration = lamp->outputTransition.generation;
    TEST_ASSERT_TRUE(lamp->outputTransition.active);

    LampOutputInterruption interruption = {};
    TEST_ASSERT_EQUAL_INT(static_cast<int>(LampOutputOffResult::APPLIED),
        static_cast<int>(forceLampOutputOff(*lamp, fakeNow, interruption)));
    TEST_ASSERT_TRUE(interruption.transitionStopped);
    TEST_ASSERT_EQUAL_UINT8(0, simulatedLampHardwareOutput());
    TEST_ASSERT_FALSE(lamp->state.power);
    TEST_ASSERT_EQUAL_UINT8(80, lamp->state.lastNonzeroBrightness);
    TEST_ASSERT_EQUAL_UINT8(writesBeforeFade,
        settingsStore.getBrightnessWriteAttemptCount());

    TEST_ASSERT_TRUE(removeLamp(lamps, 1));
    simulateLampHardwareFadeCallbackForGeneration(20, oldGeneration);
    pollLampTransitions(communication, deduplicator, lamps);
    TEST_ASSERT_EQUAL_UINT8(0, simulatedLampHardwareOutput());
}

void test_secure_removal_shutdown_forces_already_off_output_low() {
    configureLampHardwareSimulation(true);
    MessageTracker tracker;
    MessageDeduplicator deduplicator;
    LampRegistry lamps;
    GroupRegistry groups;
    SceneRegistry scenes;
    Communication communication = {};
    prepareTransitionRegistries(lamps, tracker, deduplicator, groups, scenes);
    MemoryProvisioningStore settingsStore;
    Lamp* lamp = findLamp(lamps, 1);
    TEST_ASSERT_NOT_NULL(lamp);
    restoreLampSettings(*lamp, settingsStore);
    const uint32_t writesBefore = settingsStore.getBrightnessWriteAttemptCount();
    TEST_ASSERT_FALSE(lamp->state.power);

    LampOutputInterruption interruption = {};
    TEST_ASSERT_EQUAL_INT(static_cast<int>(LampOutputOffResult::APPLIED),
        static_cast<int>(forceLampOutputOff(*lamp, fakeNow, interruption)));
    TEST_ASSERT_FALSE(interruption.hadTransition);
    TEST_ASSERT_EQUAL_UINT8(0, simulatedLampHardwareOutput());
    TEST_ASSERT_EQUAL_UINT8(writesBefore,
        settingsStore.getBrightnessWriteAttemptCount());
    TEST_ASSERT_TRUE(removeLamp(lamps, 1));
    TEST_ASSERT_EQUAL_UINT8(0, lamps.count);
}

void test_core_v7_exec_ack_is_sent_only_after_fade_and_replayed() {
    configureLampHardwareSimulation(true);
    MessageTracker tracker;
    MessageDeduplicator deduplicator;
    LampRegistry lamps;
    GroupRegistry groups;
    SceneRegistry scenes;
    TransitionCaptureTransport transport;
    Communication communication = {};
    communication.state = CommunicationState::READY;
    communication.transport = &transport;
    initMessageTracker(tracker);
    initMessageDeduplicator(deduplicator);
    prepareCoreLamp(lamps);
    initGroupRegistry(groups);
    initSceneRegistry(scenes);

    const Message command = makeCoreLampCommand(600,
        static_cast<int32_t>(ActionType::SET_LAMP_BRIGHTNESS), 65);
    queueMessage(command);
    processMessages(communication, tracker, deduplicator, lamps, groups, scenes);
    TEST_ASSERT_EQUAL_UINT8(0, transport.count);
    TEST_ASSERT_TRUE(findLamp(lamps, 1)->outputTransition.active);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ExecutionStatus::NOT_EXECUTED),
        static_cast<int>(findProcessedMessage(deduplicator, CORE_LOGICAL_ID,
                                              600)->executionStatus));

    queueMessage(command);
    processMessages(communication, tracker, deduplicator, lamps, groups, scenes);
    TEST_ASSERT_EQUAL_UINT8(0, transport.count);
    TEST_ASSERT_TRUE(simulatedLampHardwareFadeActive());

    simulateLampHardwareFadeCompletion();
    pollLampTransitions(communication, deduplicator, lamps);
    TEST_ASSERT_EQUAL_UINT8(1, transport.count);
    TEST_ASSERT_EQUAL_UINT32(0x12345678, transport.hops[0]);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ExecutionStatus::EXECUTED),
        static_cast<int>(transport.sent[0].executionStatus));
    TEST_ASSERT_EQUAL_UINT32(command.destinationId, transport.sent[0].sourceId);
    TEST_ASSERT_EQUAL_UINT32(CORE_LOGICAL_ID, transport.sent[0].destinationId);
    TEST_ASSERT_EQUAL_UINT32(command.id, static_cast<uint32_t>(transport.sent[0].value2));

    queueMessage(command);
    processMessages(communication, tracker, deduplicator, lamps, groups, scenes);
    TEST_ASSERT_EQUAL_UINT8(2, transport.count);
    TEST_ASSERT_FALSE(simulatedLampHardwareFadeActive());
}

void test_lost_final_ack_is_replayed_without_repeating_transition() {
    configureLampHardwareSimulation(true);
    MessageTracker tracker;
    MessageDeduplicator deduplicator;
    LampRegistry lamps;
    GroupRegistry groups;
    SceneRegistry scenes;
    Communication communication = {};
    prepareTransitionRegistries(lamps, tracker, deduplicator, groups, scenes);
    Message command = makeMessage(505, 22, 1);
    command.commandType = static_cast<int32_t>(ActionType::SET_LAMP_BRIGHTNESS);
    command.value = 45;
    queueMessage(command);
    processMessages(communication, tracker, deduplicator, lamps, groups, scenes,
                    true);
    simulateLampHardwareFadeCompletion();
    pollLampTransitions(communication, deduplicator, lamps);
    TEST_ASSERT_EQUAL_UINT8(0, sentCount);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ExecutionStatus::EXECUTED),
        static_cast<int>(findProcessedMessage(deduplicator, 22, 505)->executionStatus));

    queueMessage(command);
    processMessages(communication, tracker, deduplicator, lamps, groups, scenes);
    TEST_ASSERT_EQUAL_UINT8(1, sentCount);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ExecutionStatus::EXECUTED),
        static_cast<int>(lastSentMessage.executionStatus));
    TEST_ASSERT_FALSE(simulatedLampHardwareFadeActive());
}

void test_ledc_start_failure_restores_previous_level_and_reports_failed() {
    configureLampHardwareSimulation(true);
    Lamp lamp = makeLamp(1);
    TEST_ASSERT_TRUE(setLampBrightness(lamp, 25));
    failNextLampHardwareFadeStart();
    TEST_ASSERT_EQUAL_INT(static_cast<int>(LampOutputTransitionResult::FAILED),
        static_cast<int>(beginLampOutputTransition(
            lamp, ActionType::SET_LAMP_BRIGHTNESS, 75, 11, 9, false, false)));
    TEST_ASSERT_FALSE(lamp.outputTransition.active);
    TEST_ASSERT_FALSE(lamp.outputTransition.faulted);
    TEST_ASSERT_EQUAL_UINT8(25, simulatedLampHardwareOutput());
    TEST_ASSERT_EQUAL_UINT8(25, lamp.state.brightness);
}

void test_stale_fade_callback_cannot_complete_a_newer_target() {
    configureLampHardwareSimulation(true);
    Lamp lamp = makeLamp(1);
    TEST_ASSERT_TRUE(setLampBrightness(lamp, 20));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(LampOutputTransitionResult::STARTED),
        static_cast<int>(beginLampOutputTransition(
            lamp, ActionType::SET_LAMP_BRIGHTNESS, 80, 11, 10, false, false)));
    simulateLampHardwareFadeCallback(20);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(LampOutputTransitionPollResult::NONE),
        static_cast<int>(pollLampOutputTransition(lamp, fakeNow)));
    TEST_ASSERT_TRUE(lamp.outputTransition.active);
    TEST_ASSERT_EQUAL_UINT8(20, lamp.state.brightness);
    simulateLampHardwareFadeCompletion();
    TEST_ASSERT_EQUAL_INT(static_cast<int>(LampOutputTransitionPollResult::EXECUTED),
        static_cast<int>(pollLampOutputTransition(lamp, fakeNow)));
    TEST_ASSERT_EQUAL_UINT8(80, lamp.state.brightness);
}

void test_positive_setpoint_is_persisted_once_only_after_fade_completion() {
    configureLampHardwareSimulation(true);
    MemoryProvisioningStore settingsStore;
    Lamp lamp = makeLamp(1);
    restoreLampSettings(lamp, settingsStore);
    const uint32_t writesBeforeFade =
        settingsStore.getBrightnessWriteAttemptCount();
    TEST_ASSERT_EQUAL_INT(static_cast<int>(LampOutputTransitionResult::STARTED),
        static_cast<int>(beginLampOutputTransition(
            lamp, ActionType::SET_LAMP_BRIGHTNESS, 72, 11, 11, false, false)));
    TEST_ASSERT_EQUAL_UINT32(writesBeforeFade,
                             settingsStore.getBrightnessWriteAttemptCount());
    simulateLampHardwareFadeCompletion();
    TEST_ASSERT_EQUAL_UINT32(writesBeforeFade,
                             settingsStore.getBrightnessWriteAttemptCount());
    TEST_ASSERT_EQUAL_INT(static_cast<int>(LampOutputTransitionPollResult::EXECUTED),
        static_cast<int>(pollLampOutputTransition(lamp, fakeNow)));
    TEST_ASSERT_EQUAL_UINT32(1, settingsStore.getBrightnessWriteAttemptCount());
    TEST_ASSERT_EQUAL_INT(static_cast<int>(LampOutputTransitionResult::COMPLETED),
        static_cast<int>(beginLampOutputTransition(
            lamp, ActionType::SET_LAMP_BRIGHTNESS, 72, 11, 12, false, false)));
    TEST_ASSERT_EQUAL_UINT32(1, settingsStore.getBrightnessWriteAttemptCount());
}

void test_zero_brightness_and_power_off_do_not_write_nvs() {
    configureLampHardwareSimulation(true);
    MemoryProvisioningStore settingsStore;
    Lamp lamp = makeLamp(1);
    restoreLampSettings(lamp, settingsStore);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(LampOutputTransitionResult::STARTED),
        static_cast<int>(beginLampOutputTransition(
            lamp, ActionType::SET_LAMP_BRIGHTNESS, 50, 11, 13, false, false)));
    simulateLampHardwareFadeCompletion();
    TEST_ASSERT_EQUAL_INT(static_cast<int>(LampOutputTransitionPollResult::EXECUTED),
        static_cast<int>(pollLampOutputTransition(lamp, fakeNow)));
    TEST_ASSERT_EQUAL_UINT32(1, settingsStore.getBrightnessWriteAttemptCount());
    TEST_ASSERT_EQUAL_INT(static_cast<int>(LampOutputTransitionResult::STARTED),
        static_cast<int>(beginLampOutputTransition(
            lamp, ActionType::SET_LAMP_BRIGHTNESS, 0, 11, 14, false, false)));
    simulateLampHardwareFadeCompletion();
    TEST_ASSERT_EQUAL_INT(static_cast<int>(LampOutputTransitionPollResult::EXECUTED),
        static_cast<int>(pollLampOutputTransition(lamp, fakeNow)));
    TEST_ASSERT_EQUAL_UINT32(1, settingsStore.getBrightnessWriteAttemptCount());
    TEST_ASSERT_EQUAL_INT(static_cast<int>(LampOutputTransitionResult::COMPLETED),
        static_cast<int>(beginLampOutputTransition(
            lamp, ActionType::SET_LAMP_POWER, 0, 11, 15, false, false)));
    TEST_ASSERT_EQUAL_UINT32(1, settingsStore.getBrightnessWriteAttemptCount());
}

void test_missing_callback_with_target_readback_completes_at_watchdog() {
    configureLampHardwareSimulation(true);
    Lamp lamp = makeLamp(1);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(LampOutputTransitionResult::STARTED),
        static_cast<int>(beginLampOutputTransition(
            lamp, ActionType::SET_LAMP_BRIGHTNESS, 55, 11, 16, false, false)));
    simulateLampHardwareReachTargetWithoutCallback();
    TEST_ASSERT_EQUAL_INT(static_cast<int>(LampOutputTransitionPollResult::NONE),
        static_cast<int>(pollLampOutputTransition(lamp, fakeNow + 1000)));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(LampOutputTransitionPollResult::EXECUTED),
        static_cast<int>(pollLampOutputTransition(lamp, fakeNow + 3500)));
    TEST_ASSERT_EQUAL_UINT8(55, lamp.state.brightness);
}

void test_fade_timeout_restores_previous_output_and_reports_failure() {
    configureLampHardwareSimulation(true);
    Lamp lamp = makeLamp(1);
    TEST_ASSERT_TRUE(setLampBrightness(lamp, 30));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(LampOutputTransitionResult::STARTED),
        static_cast<int>(beginLampOutputTransition(
            lamp, ActionType::SET_LAMP_BRIGHTNESS, 75, 11, 17, false, false)));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(LampOutputTransitionPollResult::FAILED),
        static_cast<int>(pollLampOutputTransition(lamp, fakeNow + 3500)));
    TEST_ASSERT_EQUAL_UINT8(30, simulatedLampHardwareOutput());
    TEST_ASSERT_EQUAL_UINT8(30, lamp.state.brightness);
    TEST_ASSERT_FALSE(lamp.outputTransition.faulted);
}

void test_unrestorable_timeout_withholds_execution_ack() {
    configureLampHardwareSimulation(true);
    MessageTracker tracker;
    MessageDeduplicator deduplicator;
    LampRegistry lamps;
    GroupRegistry groups;
    SceneRegistry scenes;
    Communication communication = {};
    prepareTransitionRegistries(lamps, tracker, deduplicator, groups, scenes);
    Message command = makeMessage(506, 22, 1);
    command.commandType = static_cast<int32_t>(ActionType::SET_LAMP_BRIGHTNESS);
    command.value = 90;
    queueMessage(command);
    processMessages(communication, tracker, deduplicator, lamps, groups, scenes);
    failNextLampHardwareFadeStop();
    pollLampTransitions(communication, deduplicator, lamps);
    fakeNow += 3500;
    pollLampTransitions(communication, deduplicator, lamps);
    TEST_ASSERT_EQUAL_UINT8(0, sentCount);
    TEST_ASSERT_TRUE(findLamp(lamps, 1)->outputTransition.faulted);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ExecutionStatus::NOT_EXECUTED),
        static_cast<int>(findProcessedMessage(deduplicator, 22, 506)->executionStatus));
}

void test_fade_service_unavailable_preserves_immediate_brightness_behavior() {
    configureLampHardwareSimulation(true, 100, false);
    Lamp lamp = makeLamp(1);
    TEST_ASSERT_FALSE(lampHardwareSupportsFade());
    TEST_ASSERT_EQUAL_INT(static_cast<int>(LampOutputTransitionResult::COMPLETED),
        static_cast<int>(beginLampOutputTransition(
            lamp, ActionType::SET_LAMP_BRIGHTNESS, 44, 11, 18, false, false)));
    TEST_ASSERT_EQUAL_UINT8(44, simulatedLampHardwareOutput());
    TEST_ASSERT_EQUAL_UINT8(44, lamp.state.brightness);
    TEST_ASSERT_FALSE(lamp.outputTransition.active);
}

void test_pwm_reboot_during_transition_returns_to_off_and_keeps_old_setpoint() {
    configureLampHardwareSimulation(true);
    MemoryProvisioningStore settingsStore;
    TEST_ASSERT_TRUE(settingsStore.saveLastNonzeroBrightness(35));
    const uint32_t writesBeforeFade =
        settingsStore.getBrightnessWriteAttemptCount();
    Lamp beforeReboot = makeLamp(1);
    restoreLampSettings(beforeReboot, settingsStore);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(LampOutputTransitionResult::STARTED),
        static_cast<int>(beginLampOutputTransition(
            beforeReboot, ActionType::SET_LAMP_BRIGHTNESS, 80,
            11, 19, false, false)));
    TEST_ASSERT_EQUAL_UINT32(writesBeforeFade,
                             settingsStore.getBrightnessWriteAttemptCount());

    initializeLampHardware();
    Lamp afterReboot = makeLamp(1);
    restoreLampSettings(afterReboot, settingsStore);
    TEST_ASSERT_FALSE(afterReboot.outputTransition.active);
    TEST_ASSERT_FALSE(afterReboot.state.power);
    TEST_ASSERT_EQUAL_UINT8(0, afterReboot.state.brightness);
    TEST_ASSERT_EQUAL_UINT8(35, afterReboot.state.lastNonzeroBrightness);
    TEST_ASSERT_EQUAL_UINT8(0, simulatedLampHardwareOutput());
    TEST_ASSERT_EQUAL_UINT32(writesBeforeFade,
                             settingsStore.getBrightnessWriteAttemptCount());
}

void test_main_direct_on_off_remains_compatible_without_pwm() {
    configureLampHardwareSimulation(false);
    MessageTracker tracker;
    MessageDeduplicator deduplicator;
    LampRegistry lamps;
    GroupRegistry groups;
    SceneRegistry scenes;
    Communication communication = {};
    prepareTransitionRegistries(lamps, tracker, deduplicator, groups, scenes);
    Message on = makeMessage(507, 22, 1);
    on.commandType = static_cast<int32_t>(ActionType::SET_LAMP_POWER);
    on.value = 1;
    queueMessage(on);
    processMessages(communication, tracker, deduplicator, lamps, groups, scenes);
    TEST_ASSERT_TRUE(findLamp(lamps, 1)->state.power);
    TEST_ASSERT_EQUAL_UINT8(1, sentCount);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ExecutionStatus::EXECUTED),
        static_cast<int>(lastSentMessage.executionStatus));

    Message off = makeMessage(508, 22, 1);
    off.commandType = static_cast<int32_t>(ActionType::SET_LAMP_POWER);
    off.value = 0;
    queueMessage(off);
    processMessages(communication, tracker, deduplicator, lamps, groups, scenes);
    TEST_ASSERT_FALSE(findLamp(lamps, 1)->state.power);
    TEST_ASSERT_EQUAL_UINT8(2, sentCount);
}

void test_removed_lamp_does_not_receive_late_transition_ack() {
    configureLampHardwareSimulation(true);
    MessageTracker tracker;
    MessageDeduplicator deduplicator;
    LampRegistry lamps;
    GroupRegistry groups;
    SceneRegistry scenes;
    Communication communication = {};
    prepareTransitionRegistries(lamps, tracker, deduplicator, groups, scenes);
    Message command = makeMessage(509, 22, 1);
    command.commandType = static_cast<int32_t>(ActionType::SET_LAMP_BRIGHTNESS);
    command.value = 85;
    queueMessage(command);
    processMessages(communication, tracker, deduplicator, lamps, groups, scenes);
    TEST_ASSERT_TRUE(removeLamp(lamps, 1));
    simulateLampHardwareFadeCompletion();
    pollLampTransitions(communication, deduplicator, lamps);
    TEST_ASSERT_EQUAL_UINT8(0, sentCount);
    TEST_ASSERT_EQUAL_UINT8(0, lamps.count);
}

void test_automation_counts_are_bounded_and_runtime_state_is_reset() {
    AutomationRegistry registry;
    initAutomationRegistry(registry);

    Automation invalidConditions = makePresenceAutomation(
        1, AutomationTriggerMode::ONCE
    );
    invalidConditions.conditionCount = MAX_AUTOMATION_CONDITIONS + 1;
    TEST_ASSERT_FALSE(addAutomation(registry, invalidConditions));

    Automation invalidActions = makePresenceAutomation(
        2, AutomationTriggerMode::ONCE
    );
    invalidActions.actionCount = MAX_AUTOMATION_ACTIONS + 1;
    TEST_ASSERT_FALSE(addAutomation(registry, invalidActions));
    TEST_ASSERT_EQUAL_UINT8(0, registry.count);

    Automation valid = makePresenceAutomation(3, AutomationTriggerMode::ONCE);
    valid.conditionState = true;
    valid.conditionSince = 123;
    valid.lastTriggered = 456;
    TEST_ASSERT_TRUE(addAutomation(registry, valid));
    TEST_ASSERT_FALSE(registry.automations[0].conditionState);
    TEST_ASSERT_EQUAL_UINT32(0, registry.automations[0].conditionSince);
    TEST_ASSERT_EQUAL_UINT32(0, registry.automations[0].lastTriggered);
}

void test_automation_once_and_repeat_trigger_modes() {
    AutomationRegistry registry;
    initAutomationRegistry(registry);
    Automation automation = makePresenceAutomation(1, AutomationTriggerMode::ONCE);
    TEST_ASSERT_TRUE(addAutomation(registry, automation));

    SceneRegistry scenes;
    GroupRegistry groups;
    LampRegistry lamps;
    initSceneRegistry(scenes);
    initGroupRegistry(groups);
    initLampRegistry(lamps);

    AutomationContext context = {};
    context.presenceDetected = true;

    fakeNow = 100;
    processAutomations(registry, scenes, groups, lamps, context);
    fakeNow = 101;
    processAutomations(registry, scenes, groups, lamps, context);
    TEST_ASSERT_EQUAL_UINT32(101, registry.automations[0].lastTriggered);

    fakeNow = 102;
    processAutomations(registry, scenes, groups, lamps, context);
    TEST_ASSERT_EQUAL_UINT32(101, registry.automations[0].lastTriggered);

    registry.automations[0].triggerMode = AutomationTriggerMode::REPEAT;
    registry.automations[0].cooldownMs = 10;
    fakeNow = 105;
    processAutomations(registry, scenes, groups, lamps, context);
    TEST_ASSERT_EQUAL_UINT32(101, registry.automations[0].lastTriggered);

    fakeNow = 112;
    processAutomations(registry, scenes, groups, lamps, context);
    TEST_ASSERT_EQUAL_UINT32(112, registry.automations[0].lastTriggered);
}

void test_device_seen_and_timeout_publish_status_events() {
    LampRegistry lamps;
    EventBus events;
    initLampRegistry(lamps);
    initEventBus(events);
    TEST_ASSERT_TRUE(addLamp(lamps, makeLamp(1)));
    findLamp(lamps, 1)->device.lastSeen = 0;

    fakeNow = DEVICE_TIMEOUT + 1;
    updateDeviceStatus(lamps, &events);
    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(DeviceStatus::OFFLINE),
        static_cast<int>(findLamp(lamps, 1)->device.status)
    );

    Event event = {};
    TEST_ASSERT_TRUE(consumeEvent(events, event));
    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(EventType::LAMP_OFFLINE),
        static_cast<int>(event.type)
    );
    TEST_ASSERT_EQUAL_UINT32(1, event.sourceId);

    fakeNow += 1;
    updateDeviceSeen(findLamp(lamps, 1)->device, &events);
    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(DeviceStatus::ONLINE),
        static_cast<int>(findLamp(lamps, 1)->device.status)
    );
    TEST_ASSERT_TRUE(consumeEvent(events, event));
    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(EventType::LAMP_ONLINE),
        static_cast<int>(event.type)
    );
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_lamp_group_scene_registry_capacities);
    RUN_TEST(test_pwm_simulation_brightness_values_and_restore_after_power_off);
    RUN_TEST(test_pwm_zero_off_and_full_scale_reaches_configured_allowed_ceiling);
    RUN_TEST(test_pwm_brightness_persists_and_restores_after_cold_reboot);
    RUN_TEST(test_pwm_missing_invalid_and_unreadable_settings_use_safe_ram_default);
    RUN_TEST(test_pwm_write_failure_keeps_applied_state_and_old_saved_value);
    RUN_TEST(test_brightness_refused_without_pwm_and_after_hardware_failure);
    RUN_TEST(test_message_tracker_ack_retry_timeout_and_reuse);
    RUN_TEST(test_tracker_reuses_completed_entry_but_respects_pending_capacity);
    RUN_TEST(test_deduplicator_uses_source_and_message_id_and_evicts_oldest);
    RUN_TEST(test_message_router_executes_distinct_sources_and_suppresses_replay);
    RUN_TEST(test_ledc_fade_duration_scales_for_all_requested_ranges);
    RUN_TEST(test_ledc_fade_uses_minimum_duration_for_small_changes);
    RUN_TEST(test_ledc_power_on_restores_last_positive_level_with_fade);
    RUN_TEST(test_ledc_power_off_is_immediate_after_transition_finishes);
    RUN_TEST(test_generic_main_command_ack_waits_for_fade_completion);
    RUN_TEST(test_pending_duplicate_does_not_restart_fade_or_send_terminal_ack);
    RUN_TEST(test_priority_off_interrupts_fade_and_retry_never_restarts_it);
    RUN_TEST(test_priority_off_interrupts_fade_one_millisecond_before_deadline);
    RUN_TEST(test_priority_off_after_fade_reached_target_reports_both_executed);
    RUN_TEST(test_priority_off_stop_failure_does_not_claim_output_is_off);
    RUN_TEST(test_priority_off_gpio_failure_withholds_success_and_marks_fault);
    RUN_TEST(test_secure_removal_shutdown_interrupts_rising_fade_without_nvs_write);
    RUN_TEST(test_secure_removal_shutdown_interrupts_descending_fade_without_persisting_zero);
    RUN_TEST(test_secure_removal_shutdown_forces_already_off_output_low);
    RUN_TEST(test_core_v7_exec_ack_is_sent_only_after_fade_and_replayed);
    RUN_TEST(test_lost_final_ack_is_replayed_without_repeating_transition);
    RUN_TEST(test_ledc_start_failure_restores_previous_level_and_reports_failed);
    RUN_TEST(test_stale_fade_callback_cannot_complete_a_newer_target);
    RUN_TEST(test_positive_setpoint_is_persisted_once_only_after_fade_completion);
    RUN_TEST(test_zero_brightness_and_power_off_do_not_write_nvs);
    RUN_TEST(test_missing_callback_with_target_readback_completes_at_watchdog);
    RUN_TEST(test_fade_timeout_restores_previous_output_and_reports_failure);
    RUN_TEST(test_unrestorable_timeout_withholds_execution_ack);
    RUN_TEST(test_fade_service_unavailable_preserves_immediate_brightness_behavior);
    RUN_TEST(test_pwm_reboot_during_transition_returns_to_off_and_keeps_old_setpoint);
    RUN_TEST(test_main_direct_on_off_remains_compatible_without_pwm);
    RUN_TEST(test_removed_lamp_does_not_receive_late_transition_ack);
    RUN_TEST(test_automation_counts_are_bounded_and_runtime_state_is_reset);
    RUN_TEST(test_automation_once_and_repeat_trigger_modes);
    RUN_TEST(test_device_seen_and_timeout_publish_status_events);
    return UNITY_END();
}
