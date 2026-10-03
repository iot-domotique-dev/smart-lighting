#include <Arduino.h>

#include "message_router.h"
#include "message_manager.h"
#include "action_executor.h"
#include "message_id_generator.h"
#include "device_manager.h"
#include "event_bus.h"

namespace {
ApplicationMessageHandler applicationMessageHandler = nullptr;
void* applicationMessageContext = nullptr;
}

void registerApplicationMessageHandler(
    ApplicationMessageHandler handler,
    void* context
) {
    applicationMessageHandler = handler;
    applicationMessageContext = context;
}


static bool isValidCommandType(
    int32_t commandType
) {
    return (
        commandType >=
            static_cast<int32_t>(
                ActionType::EXECUTE_SCENE
            )
        &&
        commandType <=
            static_cast<int32_t>(
                ActionType::SET_GROUP_BRIGHTNESS
            )
    );
}


/*
 * ============================================================
 * ACK
 * ============================================================
 */

static void sendAck(
    Communication& communication,
    const Message& command,
    ExecutionStatus executionStatus,
    bool dropAck
) {
    Message ack{};
    ack.id = generateMessageId();
    ack.sourceId = command.destinationId;
    ack.destinationId = command.sourceId;
    ack.type = MessageType::ACK;
    ack.timestamp = millis();
    ack.commandType = command.commandType;
    ack.value = static_cast<int32_t>(executionStatus);
    ack.value2 = static_cast<int32_t>(command.id);
    ack.status = MessageStatus::PENDING;
    ack.executionStatus = executionStatus;


    if (
        dropAck
    ) {

        Serial.println();

        Serial.print(
            "ACK volontairement perdu : "
        );

        Serial.println(
            ack.id
        );


        Serial.print(
            "ACK correspondant au message : "
        );

        Serial.println(
            command.id
        );

        return;
    }


    sendMessage(
        communication,
        ack
    );


    Serial.println(
        "ACK genere"
    );
}


/*
 * ============================================================
 * MESSAGE PROCESSOR
 * ============================================================
 */

void processMessages(
    Communication& communication,
    MessageTracker& tracker,
    MessageDeduplicator& deduplicator,
    LampRegistry& lamps,
    GroupRegistry& groups,
    SceneRegistry& scenes,
    bool dropNextAck,
    EventBus* eventBus
) {
    Message message;


    while (
        receiveMessage(
            communication,
            message
        )
    ) {

        Lamp* sourceLamp = findLamp(
            lamps,
            message.sourceId
        );

        if (sourceLamp != nullptr) {
            updateDeviceSeen(
                sourceLamp->device,
                eventBus
            );
        }

        Serial.println();

        Serial.println(
            ">>> MESSAGE RECU"
        );


        printMessage(
            message
        );

        if (applicationMessageHandler != nullptr &&
            applicationMessageHandler(
                communication,
                message,
                applicationMessageContext
            )) {
            continue;
        }


        /*
         * ====================================================
         * ACK
         * ====================================================
         */

        if (
            message.type ==
            MessageType::ACK
        ) {

            Serial.println(
                ">>> ACK RECU"
            );


            processAck(
                tracker,
                message
            );


            continue;
        }


        /*
         * ====================================================
         * SEULS LES COMMANDES SONT EXECUTEES
         * ====================================================
         */

        if (
            message.type !=
            MessageType::COMMAND
        ) {
            continue;
        }


        /*
         * ====================================================
         * VALIDATION
         * ====================================================
         */

        if (
            !isValidCommandType(
                message.commandType
            )
        ) {

            Serial.println(
                "CommandType invalide"
            );


            sendAck(
                communication,
                message,
                ExecutionStatus::FAILED,
                dropNextAck
            );


            dropNextAck = false;


            continue;
        }


        /*
         * ====================================================
         * DEDUPLICATION
         * ====================================================
         */

        ProcessedMessage* processed =
            findProcessedMessage(
                deduplicator,
                message.sourceId,
                message.id
            );


        if (
            processed != nullptr
        ) {

            Serial.println();

            Serial.println(
                ">>> DOUBLON DETECTE"
            );


            Serial.print(
                "Message deja execute : "
            );

            Serial.println(
                message.id
            );


            Serial.print(
                "Execution precedente : "
            );


            Serial.println(
                executionStatusToString(
                    processed->executionStatus
                )
            );


            /*
             * On ne réexécute PAS
             * la commande.
             *
             * On renvoie simplement
             * le résultat précédent.
             */

            sendAck(
                communication,
                message,
                processed->executionStatus,
                false
            );


            continue;
        }


        /*
         * ====================================================
         * CREATION ACTION
         * ====================================================
         */

        Action action = {

            static_cast<ActionType>(
                message.commandType
            ),

            message.destinationId,

            message.value
        };


        Serial.println(
            ">>> EXECUTION COMMANDE"
        );


        /*
         * ====================================================
         * EXECUTION METIER
         * ====================================================
         */

        ExecutionStatus result =
            executeAction(
                action,
                scenes,
                groups,
                lamps
            );


        Serial.print(
            "Resultat execution : "
        );


        Serial.println(
            executionStatusToString(
                result
            )
        );


        /*
         * ====================================================
         * DEDUPLICATION
         * ====================================================
         */

        registerProcessedMessage(
            deduplicator,
            message.sourceId,
            message.id,
            result
        );


        /*
         * ====================================================
         * ACK
         * ====================================================
         */

        sendAck(
            communication,
            message,
            result,
            dropNextAck
        );


        dropNextAck = false;
    }
}
