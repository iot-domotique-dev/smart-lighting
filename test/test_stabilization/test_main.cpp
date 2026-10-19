#include <unity.h>

#include "action_executor.h"
#include "automation_context.h"
#include "automation_engine.h"
#include "automation_manager.h"
#include "communication.h"
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

bool sendMessage(Communication&, Message) {
    sentCount++;
    return sendSucceeds;
}

static void resetMessages() {
    incomingCount = 0;
    incomingIndex = 0;
    sentCount = 0;
    sendSucceeds = true;
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
    Lamp* simulatedLamp = findLamp(lamps, 1);
    TEST_ASSERT_NOT_NULL(simulatedLamp);
    failNextLampHardwareWrite();
    TEST_ASSERT_FALSE(setLampBrightness(*simulatedLamp, 50));
    TEST_ASSERT_EQUAL_UINT8(0, simulatedLamp->state.brightness);
    TEST_ASSERT_EQUAL_UINT8(0, simulatedLampHardwareOutput());

    brightnessAction.value = 300;
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ExecutionStatus::FAILED),
        static_cast<int>(executeAction(brightnessAction, scenes, groups, lamps)));
}

void test_pwm_reboot_starts_low_and_uses_ram_default_until_reconfigured() {
    configureLampHardwareSimulation(true);
    Lamp beforeReboot = makeLamp(1);
    TEST_ASSERT_TRUE(setLampBrightness(beforeReboot, 35));
    TEST_ASSERT_EQUAL_UINT8(35, simulatedLampHardwareOutput());

    initializeLampHardware();
    TEST_ASSERT_EQUAL_UINT8(0, simulatedLampHardwareOutput());
    Lamp afterReboot = makeLamp(1);
    TEST_ASSERT_EQUAL_UINT8(100, afterReboot.state.lastNonzeroBrightness);
    TEST_ASSERT_TRUE(setLampPower(afterReboot, true));
    TEST_ASSERT_EQUAL_UINT8(100, simulatedLampHardwareOutput());
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
    RUN_TEST(test_pwm_reboot_starts_low_and_uses_ram_default_until_reconfigured);
    RUN_TEST(test_brightness_refused_without_pwm_and_after_hardware_failure);
    RUN_TEST(test_message_tracker_ack_retry_timeout_and_reuse);
    RUN_TEST(test_tracker_reuses_completed_entry_but_respects_pending_capacity);
    RUN_TEST(test_deduplicator_uses_source_and_message_id_and_evicts_oldest);
    RUN_TEST(test_message_router_executes_distinct_sources_and_suppresses_replay);
    RUN_TEST(test_automation_counts_are_bounded_and_runtime_state_is_reset);
    RUN_TEST(test_automation_once_and_repeat_trigger_modes);
    RUN_TEST(test_device_seen_and_timeout_publish_status_events);
    return UNITY_END();
}
