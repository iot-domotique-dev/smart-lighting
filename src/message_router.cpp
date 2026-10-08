#include <Arduino.h>

#include "message_router.h"
#include "message_manager.h"
#include "action_executor.h"
#include "message_id_generator.h"
#include "device_manager.h"
#include "event_bus.h"
#include "core_command_protocol.h"

namespace {
constexpr uint8_t MAX_APPLICATION_MESSAGE_HANDLERS = 4;
struct ApplicationMessageHandlerSlot {
    ApplicationMessageHandler handler;
    void* context;
};
ApplicationMessageHandlerSlot applicationMessageHandlers[
    MAX_APPLICATION_MESSAGE_HANDLERS
] = {};
}

void registerApplicationMessageHandler(
    ApplicationMessageHandler handler,
    void* context
) {
    if (handler == nullptr) return;

    for (uint8_t i = 0; i < MAX_APPLICATION_MESSAGE_HANDLERS; ++i) {
        if (applicationMessageHandlers[i].handler == handler &&
            applicationMessageHandlers[i].context == context) {
            return;
        }
    }
    for (uint8_t i = 0; i < MAX_APPLICATION_MESSAGE_HANDLERS; ++i) {
        if (applicationMessageHandlers[i].handler == nullptr) {
            applicationMessageHandlers[i] = {handler, context};
            return;
        }
    }
    Serial.println("[MESSAGE] application handler capacity reached");
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

static void sendCoreExecutionAck(
    Communication& communication,
    const Message& command,
    uint32_t replyHop,
    ExecutionStatus result,
    CoreCommandError error,
    bool dropAck
) {
    const bool diagnosticDrop = consumeDropNextCoreExecutionAck();
    if (dropAck || diagnosticDrop) {
        Serial.print("ACK volontairement perdu pour commande CORE : ");
        Serial.println(command.id);
        return;
    }
    const Message ack = makeCoreCommandAck(command, command.destinationId,
                                           result, error);
    if (!sendMessageVia(communication, ack, replyHop)) {
        Serial.println("[V7.2] unable to return execution ACK through MAIN");
    }
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

        bool applicationMessageHandled = false;
        for (uint8_t i = 0; i < MAX_APPLICATION_MESSAGE_HANDLERS; ++i) {
            const ApplicationMessageHandlerSlot& slot =
                applicationMessageHandlers[i];
            if (slot.handler != nullptr &&
                slot.handler(communication, message, slot.context)) {
                applicationMessageHandled = true;
                break;
            }
        }
        if (applicationMessageHandled) {
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

        if (isCoreCommandMessage(message)) {
            uint32_t targetId = 0;
            uint32_t replyHop = 0;
            const CoreCommandError routing = resolveCoreLampCommand(
                lamps, message, targetId, replyHop);
            if (routing != CoreCommandError::NONE &&
                routing != CoreCommandError::OFFLINE) {
                sendCoreExecutionAck(communication, message, replyHop,
                                     ExecutionStatus::FAILED, routing, dropNextAck);
                dropNextAck = false;
                continue;
            }

            ProcessedMessage* processed = findProcessedMessage(
                deduplicator, message.sourceId, message.id);
            if (processed != nullptr) {
                const bool sameCommand = processed->hasCommandIdentity &&
                    processed->destinationId == message.destinationId &&
                    processed->parentMainId == message.parentMainId &&
                    processed->provisioningDeviceId == message.provisioningDeviceId &&
                    processed->commandType == message.commandType &&
                    processed->value == message.value;
                if (!sameCommand) {
                    sendCoreExecutionAck(communication, message, replyHop,
                                         ExecutionStatus::FAILED,
                                         CoreCommandError::DUPLICATE_ID, dropNextAck);
                    dropNextAck = false;
                    continue;
                }
                Serial.print("[V7.2] duplicate command, no execution : ");
                Serial.println(message.id);
                if (processed->executionStatus != ExecutionStatus::NOT_EXECUTED) {
                    sendCoreExecutionAck(communication, message, replyHop,
                                         processed->executionStatus,
                                         CoreCommandError::NONE, false);
                }
                continue;
            }
            if (routing == CoreCommandError::OFFLINE) {
                sendCoreExecutionAck(communication, message, replyHop,
                                     ExecutionStatus::FAILED, routing, dropNextAck);
                dropNextAck = false;
                continue;
            }

            constexpr uint32_t RETRY_PROTECTION_MS = 60000;
            if (!reserveProcessedMessage(deduplicator, message.sourceId,
                                         message.id, RETRY_PROTECTION_MS)) {
                sendCoreExecutionAck(communication, message, replyHop,
                                     ExecutionStatus::FAILED,
                                     CoreCommandError::DEDUP_FULL, dropNextAck);
                dropNextAck = false;
                continue;
            }

            ProcessedMessage* reserved = findProcessedMessage(
                deduplicator, message.sourceId, message.id);
            reserved->destinationId = message.destinationId;
            reserved->parentMainId = message.parentMainId;
            reserved->provisioningDeviceId = message.provisioningDeviceId;
            reserved->commandType = message.commandType;
            reserved->value = message.value;
            reserved->hasCommandIdentity = true;

            const Action action = {
                static_cast<ActionType>(message.commandType), targetId, message.value
            };
            const ExecutionStatus result = executeAction(action, scenes, groups, lamps);
            // The reserved entry cannot be evicted while its execution is pending.
            (void)registerProcessedMessage(deduplicator, message.sourceId,
                                           message.id, result);
            sendCoreExecutionAck(communication, message, replyHop, result,
                                 CoreCommandError::NONE, dropNextAck);
            dropNextAck = false;
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
