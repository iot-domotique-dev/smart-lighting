#include <Arduino.h>

#include "device_registry.h"
#include "device_identity.h"
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
#include "message_id_generator.h"
#include "device_manager.h"
#include "message_deduplicator.h"
#include "v5_provisioning_runtime.h"
#include "lamp_hardware.h"
#include "v5_console.h"

#if defined(SMART_LIGHTING_ZIGBEE)
#include "nvs_provisioning_store.h"
#endif


/*
 * ============================================================
 * REGISTRES
 * ============================================================
 */

LampRegistry lampRegistry;

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
 * TESTS
 * ============================================================
 */

uint32_t timeoutMessageId = 0;

uint32_t duplicateMessageId = 0;

bool simulateCommunicationLoss = false;


/*
 * ============================================================
 * SEND COMMAND
 * ============================================================
 */

[[maybe_unused]] static uint32_t sendCommand(
    uint32_t destinationId,
    ActionType actionType,
    int32_t value
) {
    Message command{};
    command.id = generateMessageId();
    command.sourceId = communication.localDeviceId;
    command.destinationId = destinationId;
    command.type = MessageType::COMMAND;
    command.timestamp = millis();
    command.commandType = static_cast<int32_t>(actionType);
    command.value = value;
    command.value2 = 0;
    command.status = MessageStatus::PENDING;
    command.executionStatus = ExecutionStatus::NOT_EXECUTED;


    if (
        !trackMessage(
            messageTracker,
            command
        )
    ) {

        Serial.println(
            "Erreur : impossible de suivre le message."
        );

        return 0;
    }


    if (
        !sendMessage(
            communication,
            command
        )
    ) {

        untrackMessage(
            messageTracker,
            command.id
        );

        Serial.println(
            "Erreur : impossible d'envoyer le message."
        );

        return 0;
    }


    return command.id;
}


/*
 * ============================================================
 * SIMULATION PERTE
 * ============================================================
 */

[[maybe_unused]] static void simulateLostMessages() {

    Message lostMessage;


    while (
        receiveMessage(
            communication,
            lostMessage
        )
    ) {

        Serial.print(
            "Message volontairement perdu : "
        );


        Serial.println(
            lostMessage.id
        );
    }
}


/*
 * ============================================================
 * PROCESS NORMAL
 * ============================================================
 */

static void processNormalCommunication() {

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
        "       SMART LIGHTING V3.9"
    );

    Serial.print("Instance : ");
    Serial.println(DEVICE_LOCAL_NAME);

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
        DEVICE_LOCAL_ID
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


#if defined(SMART_LIGHTING_DEMO)
    /*
     * ========================================================
     * LAMPES
     * ========================================================
     */

    Lamp lamp1 = {

        {
            1,
            "LAMP_01",
            DeviceRole::LAMP,
            DeviceStatus::ONLINE,
            millis()
        },

        {
            false,
            0,
            false
        }
    };


    Lamp lamp2 = {

        {
            2,
            "LAMP_02",
            DeviceRole::LAMP,
            DeviceStatus::ONLINE,
            millis()
        },

        {
            false,
            0,
            false
        }
    };


    Lamp lamp3 = {

        {
            3,
            "LAMP_03",
            DeviceRole::LAMP,
            DeviceStatus::OFFLINE,
            millis()
        },

        {
            false,
            0,
            false
        }
    };


    addLamp(
        lampRegistry,
        lamp1
    );


    addLamp(
        lampRegistry,
        lamp2
    );


    addLamp(
        lampRegistry,
        lamp3
    );


    /*
     * ========================================================
     * GROUPE
     * ========================================================
     */

    LampGroup entrance = {

        1,

        "ENTREE",

        {},

        0
    };


    addGroup(
        groupRegistry,
        entrance
    );


    addLampToGroup(
        groupRegistry,
        lampRegistry,
        1,
        1
    );


    addLampToGroup(
        groupRegistry,
        lampRegistry,
        1,
        2
    );


    addLampToGroup(
        groupRegistry,
        lampRegistry,
        1,
        3
    );


    /*
     * ========================================================
     * SCENE
     * ========================================================
     */

    Scene evening = {

        1,

        "SOIR",

        {},

        0
    };


    addScene(
        sceneRegistry,
        evening
    );


    SceneAction powerAction = {

        1,

        CommandType::SET_GROUP_POWER,

        1
    };


    addActionToScene(

        sceneRegistry,

        groupRegistry,

        1,

        powerAction
    );


    /*
     * ========================================================
     * TEST 1
     * ========================================================
     */

    Serial.println();

    Serial.println(
        "================================"
    );

    Serial.println(
        " TEST 1 : COMMAND + ACK"
    );

    Serial.println(
        "================================"
    );


    simulateCommunicationLoss =
        false;


    uint32_t normalMessageId =
        sendCommand(

            1,

            ActionType::SET_LAMP_POWER,

            1
        );


    if (
        normalMessageId != 0
    ) {

        processNormalCommunication();

        processNormalCommunication();
    }


    Serial.println();

    Serial.println(
        "===== TRACKER TEST 1 ====="
    );


    printMessageTracker(
        messageTracker
    );


    /*
     * ========================================================
     * TEST 2
     * ========================================================
     */

    Serial.println();

    Serial.println(
        "================================"
    );

    Serial.println(
        " TEST 2 : TIMEOUT + RETRIES"
    );

    Serial.println(
        "================================"
    );


    simulateCommunicationLoss =
        true;


    timeoutMessageId =
        sendCommand(

            1,

            ActionType::SET_LAMP_POWER,

            0
        );


    Serial.print(
        "Message timeout : "
    );


    Serial.println(
        timeoutMessageId
    );


    simulateLostMessages();
#endif
}


/*
 * ============================================================
 * LOOP
 * ============================================================
 */

void loop() {
    v5ProvisioningRuntime.poll();
#if defined(SMART_LIGHTING_DEMO)

    /*
     * ========================================================
     * COMMUNICATION
     * ========================================================
     */

    if (
        simulateCommunicationLoss
    ) {

        simulateLostMessages();

    }
    else {

        processNormalCommunication();
    }


    /*
     * ========================================================
     * TIMEOUT / RETRY
     * ========================================================
     */

    updateMessageTimeouts(

        messageTracker,

        communication
    );


    /*
     * ========================================================
     * FIN TEST 2
     * ========================================================
     */

    PendingMessage* timeoutMessage =
        findPendingMessage(

            messageTracker,

            timeoutMessageId
        );


    if (

        timeoutMessage != nullptr &&

        timeoutMessage->completed &&

        simulateCommunicationLoss

    ) {

        Serial.println();

        Serial.println(
            "================================"
        );

        Serial.println(
            " TEST 2 TERMINE"
        );

        Serial.println(
            "================================"
        );


        printPendingMessage(
            *timeoutMessage
        );


        simulateCommunicationLoss =
            false;


        delay(
            1000
        );


        /*
         * ====================================================
         * TEST 3
         * ====================================================
         */

        Serial.println();

        Serial.println(
            "================================"
        );

        Serial.println(
            " TEST 3 : ACK PERDU + DOUBLON"
        );

        Serial.println(
            "================================"
        );


        duplicateMessageId =
            sendCommand(

                1,

                ActionType::SET_LAMP_POWER,

                1
            );


        processMessages(

            communication,

            messageTracker,

            messageDeduplicator,

            lampRegistry,

            groupRegistry,

            sceneRegistry,

            true,

            &eventBus
        );


        Serial.println();

        Serial.println(
            "ACK perdu. Attente du timeout..."
        );
    }


    /*
     * ========================================================
     * TEST 3 RESULT
     * ========================================================
     */

    PendingMessage* duplicateMessage =
        findPendingMessage(

            messageTracker,

            duplicateMessageId
        );


    if (

        duplicateMessage != nullptr &&

        !duplicateMessage->completed &&

        duplicateMessage->retryCount > 0

    ) {

        processNormalCommunication();


        if (
            duplicateMessage->completed
        ) {

            Serial.println();

            Serial.println(
                "================================"
            );

            Serial.println(
                " RESULTAT TEST 3"
            );

            Serial.println(
                "================================"
            );


            printPendingMessage(
                *duplicateMessage
            );


            printMessageDeduplicator(
                messageDeduplicator
            );


            Serial.println();

            Serial.println(
                "===== ETAT FINAL LAMPES ====="
            );


            printLampRegistry(
                lampRegistry
            );


            duplicateMessageId =
                0;
        }
    }


#else
    processNormalCommunication();

    updateMessageTimeouts(
        messageTracker,
        communication
    );
#endif

    automationContext.timestamp = millis();

    updateDeviceStatus(
        lampRegistry,
        &eventBus
    );

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
