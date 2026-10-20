#include <Arduino.h>

#include "message_router.h"
#include "message_manager.h"
#include "action_executor.h"
#include "message_id_generator.h"
#include "device_manager.h"
#include "event_bus.h"
#include "core_command_protocol.h"
#include "lamp_controller.h"
#include "lamp_hardware.h"

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

namespace {
bool isLampOutputAction(ActionType type) {
    return type == ActionType::SET_LAMP_POWER ||
           type == ActionType::SET_LAMP_BRIGHTNESS;
}

void bindProcessedCommand(ProcessedMessage& entry, const Message& command) {
    entry.destinationId = command.destinationId;
    entry.parentMainId = command.parentMainId;
    entry.provisioningDeviceId = command.provisioningDeviceId;
    entry.commandType = command.commandType;
    entry.value = command.value;
    entry.hasCommandIdentity = true;
}

Message restoreTransitionCommand(const LampOutputTransition& transition,
                                const ProcessedMessage& entry) {
    Message command = {};
    command.id = transition.messageId;
    command.sourceId = transition.sourceId;
    command.destinationId = entry.destinationId;
    command.type = MessageType::COMMAND;
    command.timestamp = entry.processedAt;
    command.commandType = entry.commandType;
    command.value = entry.value;
    command.parentMainId = entry.parentMainId;
    command.provisioningDeviceId = entry.provisioningDeviceId;
    command.executionStatus = ExecutionStatus::NOT_EXECUTED;
    return command;
}

void finishInterruptedTransition(Communication& communication,
                                 MessageDeduplicator& deduplicator,
                                 Lamp& lamp,
                                 const LampOutputTransition& transition,
                                 ExecutionStatus result) {
    ProcessedMessage* entry = findProcessedMessage(
        deduplicator, transition.sourceId, transition.messageId);
    if (entry == nullptr || !entry->hasCommandIdentity) {
        lamp.outputTransition.faulted = true;
        Serial.println("[LAMP] interrupted fade has no deduplication identity; ACK withheld");
        return;
    }
    (void)registerProcessedMessage(deduplicator, transition.sourceId,
                                   transition.messageId, result);
    const Message command = restoreTransitionCommand(transition, *entry);
    if (transition.coreCommand) {
        sendCoreExecutionAck(communication, command,
            lamp.identity.parentMainId, result, CoreCommandError::NONE,
            transition.dropAck);
    } else {
        sendAck(communication, command, result, transition.dropAck);
    }
}

bool installSupersededCommandTombstone(MessageDeduplicator& deduplicator,
                                      uint32_t sourceId,
                                      uint32_t supersededMessageId) {
    if (supersededMessageId == 0) return true;
    constexpr uint32_t RETRY_PROTECTION_MS = 60000;
    ProcessedMessage* entry = findProcessedMessage(
        deduplicator, sourceId, supersededMessageId);
    if (entry == nullptr) {
        if (!reserveProcessedMessage(deduplicator, sourceId,
                                     supersededMessageId,
                                     RETRY_PROTECTION_MS)) {
            return false;
        }
        entry = findProcessedMessage(deduplicator, sourceId,
                                     supersededMessageId);
    }
    if (entry == nullptr) return false;

    // The prior command may already be in the LAMP fade, may be waiting in
    // the radio path, or may have completed. Preserve a final cached result;
    // convert a pending/empty reservation into a non-executable tombstone.
    if (entry->executionStatus == ExecutionStatus::NOT_EXECUTED) {
        (void)registerProcessedMessage(deduplicator, sourceId,
                                       supersededMessageId,
                                       ExecutionStatus::FAILED);
    }
    entry = findProcessedMessage(deduplicator, sourceId,
                                 supersededMessageId);
    return entry != nullptr &&
           entry->executionStatus != ExecutionStatus::NOT_EXECUTED;
}

bool startTransitionForMessage(
    Message& command,
    Lamp& lamp,
    MessageDeduplicator& deduplicator,
    bool coreCommand,
    bool dropAck,
    LampOutputTransitionResult& result
) {
    constexpr uint32_t RETRY_PROTECTION_MS = 60000;
    if (!reserveProcessedMessage(deduplicator, command.sourceId, command.id,
                                 RETRY_PROTECTION_MS)) {
        return false;
    }
    ProcessedMessage* entry = findProcessedMessage(
        deduplicator, command.sourceId, command.id);
    if (entry == nullptr) return false;
    bindProcessedCommand(*entry, command);
    result = beginLampOutputTransition(
        lamp, static_cast<ActionType>(command.commandType), command.value,
        command.sourceId, command.id, coreCommand, dropAck);
    return true;
}
}  // namespace


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
                    if (processed->executionStatus ==
                        ExecutionStatus::NOT_EXECUTED) {
                        Serial.println("[LAMP] conflicting duplicate ignored while original output is pending");
                        continue;
                    }
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

            const ActionType commandType =
                static_cast<ActionType>(message.commandType);
            Lamp* outputLamp = findLamp(lamps, targetId);

            if (outputLamp != nullptr &&
                commandType == ActionType::SET_LAMP_POWER &&
                message.value == 0 &&
                (message.value2 != 0 || outputLamp->outputTransition.active ||
                 outputLamp->outputTransition.faulted)) {
                const uint32_t supersededId =
                    static_cast<uint32_t>(message.value2);
                const bool supersededFenced =
                    installSupersededCommandTombstone(
                        deduplicator, message.sourceId, supersededId);
                LampOutputInterruption interruption = {};
                const LampOutputOffResult offResult = forceLampOutputOff(
                    *outputLamp, millis(), interruption);
                if (interruption.hadTransition &&
                    (interruption.transitionCompleted ||
                     interruption.transitionStopped)) {
                    finishInterruptedTransition(communication, deduplicator,
                        *outputLamp, interruption.transition,
                        interruption.transitionCompleted
                            ? ExecutionStatus::EXECUTED
                            : ExecutionStatus::FAILED);
                }
                const bool offApplied = offResult == LampOutputOffResult::APPLIED;
                const ExecutionStatus result = offApplied && supersededFenced
                    ? ExecutionStatus::EXECUTED : ExecutionStatus::FAILED;
                (void)registerProcessedMessage(deduplicator, message.sourceId,
                                               message.id, result);
                sendCoreExecutionAck(communication, message, replyHop, result,
                    !supersededFenced ? CoreCommandError::DEDUP_FULL
                                      : CoreCommandError::NONE,
                    dropNextAck);
                dropNextAck = false;
                continue;
            }

            if (outputLamp != nullptr && lampHardwareSupportsBrightness() &&
                isLampOutputAction(commandType)) {
                const LampOutputTransitionResult transitionResult =
                    beginLampOutputTransition(
                        *outputLamp, commandType, message.value,
                        message.sourceId, message.id, true, dropNextAck);
                if (transitionResult == LampOutputTransitionResult::STARTED) {
                    dropNextAck = false;
                    continue;
                }
                if (transitionResult == LampOutputTransitionResult::UNCERTAIN) {
                    dropNextAck = false;
                    continue;
                }
                if (transitionResult == LampOutputTransitionResult::BUSY) {
                    (void)registerProcessedMessage(deduplicator,
                        message.sourceId, message.id, ExecutionStatus::FAILED);
                    sendCoreExecutionAck(communication, message, replyHop,
                        ExecutionStatus::FAILED,
                        CoreCommandError::COMMAND_IN_PROGRESS, dropNextAck);
                    dropNextAck = false;
                    continue;
                }
                const ExecutionStatus result = transitionResult ==
                    LampOutputTransitionResult::COMPLETED
                    ? ExecutionStatus::EXECUTED : ExecutionStatus::FAILED;
                (void)registerProcessedMessage(deduplicator, message.sourceId,
                                               message.id, result);
                sendCoreExecutionAck(communication, message, replyHop, result,
                                     CoreCommandError::NONE, dropNextAck);
                dropNextAck = false;
                continue;
            }

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

            if (processed->hasCommandIdentity &&
                processed->executionStatus == ExecutionStatus::NOT_EXECUTED) {
                const bool sameCommand =
                    processed->destinationId == message.destinationId &&
                    processed->commandType == message.commandType &&
                    processed->value == message.value;
                if (!sameCommand) {
                    Serial.println("[LAMP] conflicting duplicate ignored while original output is pending");
                    continue;
                }
                // The original output transition is still running. Do not
                // restart it and do not turn a retry into a terminal ACK.
                continue;
            }

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

        const ActionType commandType = action.type;
        Lamp* outputLamp = findLamp(lamps, action.targetId);
        if (outputLamp != nullptr &&
            commandType == ActionType::SET_LAMP_POWER && message.value == 0 &&
            (outputLamp->outputTransition.active ||
             outputLamp->outputTransition.faulted)) {
            constexpr uint32_t RETRY_PROTECTION_MS = 60000;
            if (!reserveProcessedMessage(deduplicator, message.sourceId,
                                         message.id, RETRY_PROTECTION_MS)) {
                sendAck(communication, message, ExecutionStatus::FAILED,
                        dropNextAck);
                dropNextAck = false;
                continue;
            }
            ProcessedMessage* reserved = findProcessedMessage(
                deduplicator, message.sourceId, message.id);
            if (reserved == nullptr) {
                sendAck(communication, message, ExecutionStatus::FAILED,
                        dropNextAck);
                dropNextAck = false;
                continue;
            }
            bindProcessedCommand(*reserved, message);
            LampOutputInterruption interruption = {};
            const LampOutputOffResult offResult = forceLampOutputOff(
                *outputLamp, millis(), interruption);
            if (interruption.hadTransition &&
                (interruption.transitionCompleted ||
                 interruption.transitionStopped)) {
                finishInterruptedTransition(communication, deduplicator,
                    *outputLamp, interruption.transition,
                    interruption.transitionCompleted
                        ? ExecutionStatus::EXECUTED
                        : ExecutionStatus::FAILED);
            }
            const ExecutionStatus result = offResult ==
                LampOutputOffResult::APPLIED
                ? ExecutionStatus::EXECUTED : ExecutionStatus::FAILED;
            (void)registerProcessedMessage(deduplicator, message.sourceId,
                                           message.id, result);
            sendAck(communication, message, result, dropNextAck);
            dropNextAck = false;
            continue;
        }
        if (outputLamp != nullptr && lampHardwareSupportsBrightness() &&
            isLampOutputAction(commandType)) {
            LampOutputTransitionResult transitionResult =
                LampOutputTransitionResult::FAILED;
            if (outputLamp->device.status != DeviceStatus::ONLINE) {
                transitionResult = LampOutputTransitionResult::FAILED;
            } else if (!startTransitionForMessage(message, *outputLamp,
                                                  deduplicator, false,
                                                  dropNextAck,
                                                  transitionResult)) {
                sendAck(communication, message, ExecutionStatus::FAILED,
                        dropNextAck);
                dropNextAck = false;
                continue;
            }

            if (transitionResult == LampOutputTransitionResult::STARTED) {
                dropNextAck = false;
                continue;
            }
            if (transitionResult == LampOutputTransitionResult::UNCERTAIN) {
                dropNextAck = false;
                continue;
            }
            const ExecutionStatus result = transitionResult ==
                LampOutputTransitionResult::COMPLETED
                ? ExecutionStatus::EXECUTED : ExecutionStatus::FAILED;
            (void)registerProcessedMessage(deduplicator, message.sourceId,
                                           message.id, result);
            sendAck(communication, message, result, dropNextAck);
            dropNextAck = false;
            continue;
        }


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

void pollLampTransitions(Communication& communication,
                         MessageDeduplicator& deduplicator,
                         LampRegistry& lamps) {
    for (uint8_t index = 0; index < lamps.count; ++index) {
        Lamp& lamp = lamps.lamps[index];
        if (!lamp.outputTransition.active) continue;
        const LampOutputTransition transition = lamp.outputTransition;
        const LampOutputTransitionPollResult result =
            pollLampOutputTransition(lamp, millis());
        if (result == LampOutputTransitionPollResult::NONE ||
            result == LampOutputTransitionPollResult::UNCERTAIN) {
            continue;
        }

        ProcessedMessage* entry = findProcessedMessage(
            deduplicator, transition.sourceId, transition.messageId);
        if (entry == nullptr || !entry->hasCommandIdentity) {
            lamp.outputTransition.faulted = true;
            Serial.println("[LAMP] transition result has no deduplication identity; final ACK withheld");
            continue;
        }
        const ExecutionStatus execution = result ==
            LampOutputTransitionPollResult::EXECUTED
            ? ExecutionStatus::EXECUTED : ExecutionStatus::FAILED;
        (void)registerProcessedMessage(deduplicator, transition.sourceId,
                                       transition.messageId, execution);
        const Message command = restoreTransitionCommand(transition, *entry);
        if (transition.coreCommand) {
            sendCoreExecutionAck(communication, command,
                lamp.identity.parentMainId, execution,
                CoreCommandError::NONE, transition.dropAck);
        } else {
            sendAck(communication, command, execution, transition.dropAck);
        }
    }
}

void finishLampOutputTransition(Communication& communication,
                                MessageDeduplicator& deduplicator,
                                Lamp& lamp,
                                const LampOutputTransition& transition,
                                ExecutionStatus result) {
    finishInterruptedTransition(communication, deduplicator, lamp,
                                transition, result);
}
