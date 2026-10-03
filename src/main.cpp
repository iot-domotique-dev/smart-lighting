#include <Arduino.h>

#include "device_registry.h"
#include "core_module_service.h"
#include "roles.h"
#include "group_manager.h"
#include "scene_manager.h"
#include "automation_manager.h"
#include "event_bus.h"
#include "event_processor.h"
#include "automation_context.h"
#include "communication.h"
#include "message_tracker.h"
#include "message_router.h"
#include "device_manager.h"
#include "message_deduplicator.h"
#include "v5_provisioning_runtime.h"
#include "lamp_hardware.h"
#include "v5_console.h"

#if defined(SMART_LIGHTING_ZIGBEE)
#include "nvs_provisioning_store.h"
#endif

namespace {
const char* roleName(DeviceRole role) {
    switch (role) {
        case DeviceRole::CORE: return "CORE";
        case DeviceRole::MAIN: return "MAIN";
        case DeviceRole::LAMP: return "LAMP";
        case DeviceRole::RELAY: return "RELAY";
        case DeviceRole::SENSOR: return "SENSOR";
        case DeviceRole::CAMERA: return "CAMERA";
    }
    return "UNKNOWN";
}
}  // namespace


/*
 * ============================================================
 * REGISTRES
 * ============================================================
 */

LampRegistry lampRegistry;

#if defined(DEVICE_ROLE_CORE)
DeviceRegistry coreDeviceRegistry;
#endif

GroupRegistry groupRegistry;

SceneRegistry sceneRegistry;

AutomationRegistry automationRegistry;

EventBus eventBus;

AutomationContext automationContext = {};


/*
 * ============================================================
 * COMMUNICATION
 * ============================================================
 */

Communication communication;

MessageTracker messageTracker;

MessageDeduplicator messageDeduplicator;

V5ProvisioningRuntime v5ProvisioningRuntime(
    communication,
    lampRegistry,
    messageTracker
);

bool commissionLampV5(const char* hardwareId, const char* requestedName) {
    return v5ProvisioningRuntime.commissionLamp(hardwareId, requestedName);
}

bool setLampPowerV5(uint32_t deviceId, bool enabled) {
    return v5ProvisioningRuntime.setPower(deviceId, enabled);
}


/*
 * ============================================================
 * SETUP
 * ============================================================
 */

void setup() {

    Serial.begin(
        115200
    );

    initializeLampHardware();


    delay(
        1000
    );


    Serial.println();

    Serial.println(
        "================================"
    );

    Serial.println(
        "       SMART LIGHTING"
    );

    Serial.print("Role : ");
    Serial.println(roleName(DEVICE_ROLE));

    Serial.println(
        "================================"
    );


    /*
     * ========================================================
     * INITIALISATION
     * ========================================================
     */

    initLampRegistry(
        lampRegistry
    );

#if defined(DEVICE_ROLE_CORE)
    initDeviceRegistry(coreDeviceRegistry);
    const Device coreDevice = {
        CORE_LOGICAL_ID,
        "CORE",
        DeviceRole::CORE,
        DeviceStatus::ONLINE,
        millis(),
        0,
        0,
        CORE_LOGICAL_ID
    };
    if (!registerDevice(coreDeviceRegistry, coreDevice)) {
        Serial.println("[CORE] failed to register its root device");
    }
#endif


    initGroupRegistry(
        groupRegistry
    );


    initSceneRegistry(
        sceneRegistry
    );


    initAutomationRegistry(
        automationRegistry
    );


    initEventBus(
        eventBus
    );


    /*
     * IMPORTANT :
     *
     * Le reste du système ne connaît désormais
     * que Communication.
     */

    initCommunication(
        communication,
#if defined(SMART_LIGHTING_ZIGBEE)
        CommunicationTransportType::ZIGBEE,
#else
        CommunicationTransportType::SIMULATION,
#endif
#if defined(DEVICE_ROLE_CORE)
        CORE_LOGICAL_ID
#else
        0
#endif
    );


    initMessageTracker(
        messageTracker
    );


    initMessageDeduplicator(
        messageDeduplicator
    );

#if defined(SMART_LIGHTING_ZIGBEE)
    static NvsProvisioningStore lampProvisioningStore;
    if (DEVICE_ROLE == DeviceRole::LAMP) {
        (void)v5ProvisioningRuntime.begin(DEVICE_ROLE, &lampProvisioningStore);
    } else {
        (void)v5ProvisioningRuntime.begin(DEVICE_ROLE);
    }
    startV5Console();
#endif


    printCommunicationStatus(
        communication
    );



}


/*
 * ============================================================
 * LOOP
 * ============================================================
 */

void loop() {
    v5ProvisioningRuntime.poll();

    processMessages(
        communication,
        messageTracker,
        messageDeduplicator,
        lampRegistry,
        groupRegistry,
        sceneRegistry,
        false,
        &eventBus
    );

    updateMessageTimeouts(
        messageTracker,
        communication
    );

    automationContext.timestamp = millis();

    updateDeviceStatus(
        lampRegistry,
        &eventBus
    );

#if defined(DEVICE_ROLE_CORE)
    Device* core = findDeviceById(coreDeviceRegistry, CORE_LOGICAL_ID);
    if (core != nullptr) {
        updateDeviceSeen(*core, &eventBus);
    }
    updateDeviceStatus(coreDeviceRegistry, &eventBus);
#endif

    processEvents(
        eventBus,
        automationRegistry,
        sceneRegistry,
        groupRegistry,
        lampRegistry,
        automationContext
    );

    delay(100);
}
