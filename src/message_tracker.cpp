#include <Arduino.h>

#include "message_tracker.h"
#include "message_manager.h"
#include "core_command_protocol.h"

namespace {
bool isCoreTrackedCommand(const Message& message) {
    return isCoreCommandMessage(message);
}

bool validCoreAck(const Message& command, const Message& ack) {
    if (static_cast<uint32_t>(ack.value2) != command.id || command.id == 0 ||
        ack.destinationId != command.sourceId ||
        ack.commandType != command.commandType ||
        ack.parentMainId != command.parentMainId ||
        ack.provisioningDeviceId != command.provisioningDeviceId) {
        return false;
    }
    switch (ack.executionStatus) {
        case ExecutionStatus::NOT_EXECUTED:
            return ack.sourceId == command.parentMainId && ack.value == 0;
        case ExecutionStatus::EXECUTED:
            return ack.sourceId == command.destinationId &&
                   ack.value == static_cast<int32_t>(ack.executionStatus);
        case ExecutionStatus::PARTIAL:
            // V7.2 SET_POWER is indivisible; only an explicit final result applies.
            return false;
        case ExecutionStatus::FAILED:
            return (ack.sourceId == command.destinationId ||
                    ack.sourceId == command.parentMainId ||
                    ack.sourceId == command.sourceId) &&
                   (ack.value == static_cast<int32_t>(ExecutionStatus::FAILED) ||
                    (ack.value >= static_cast<int32_t>(CoreCommandError::UNKNOWN_DESTINATION) &&
                     ack.value <= static_cast<int32_t>(CoreCommandError::DUPLICATE_ID)));
    }
    return false;
}
}  // namespace


void initMessageTracker(
    MessageTracker& tracker
) {
    tracker.count = 0;

    for (
        uint8_t i = 0;
        i < MAX_PENDING_MESSAGES;
        i++
    ) {

        tracker.messages[i].waitingForAck =
            false;

        tracker.messages[i].accepted = false;

        tracker.messages[i].completed =
            false;

        tracker.messages[i].timedOut =
            false;

        tracker.messages[i].executionOrderAmbiguous = false;

        tracker.messages[i].retryCount =
            0;

        tracker.messages[i].sentAt =
            0;

        tracker.messages[i].firstSentAt = 0;

        tracker.messages[i].completedAt =
            0;
    }
}


/*
 * ============================================================
 * TRACK
 * ============================================================
 */

bool trackMessage(
    MessageTracker& tracker,
    const Message& message
) {
    /*
     * Vérification doublon.
     */

    if (
        findPendingMessage(
            tracker,
            message.id
        ) != nullptr
    ) {

        Serial.print(
            "Message deja suivi : "
        );

        Serial.println(
            message.id
        );

        return false;
    }


    uint8_t slot = tracker.count;

    if (tracker.count >= MAX_PENDING_MESSAGES) {
        slot = MAX_PENDING_MESSAGES;

        for (uint8_t i = 0; i < tracker.count; i++) {
            if (!tracker.messages[i].waitingForAck) {
                slot = i;
                break;
            }
        }

        if (slot >= MAX_PENDING_MESSAGES) {
            Serial.println("Tracker plein : tous les messages attendent un ACK");
            return false;
        }
    }


    PendingMessage& pending = tracker.messages[slot];


    pending.message =
        message;

    pending.waitingForAck =
        true;

    pending.accepted = false;

    pending.completed =
        false;

    pending.timedOut =
        false;

    pending.executionOrderAmbiguous = false;

    pending.retryCount =
        0;

    pending.sentAt =
        millis();

    pending.firstSentAt = pending.sentAt;

    pending.completedAt =
        0;


    if (tracker.count < MAX_PENDING_MESSAGES) {
        tracker.count++;
    }


    Serial.print(
        "Message suivi : "
    );

    Serial.println(
        message.id
    );


    return true;
}


bool untrackMessage(
    MessageTracker& tracker,
    uint32_t messageId
) {
    for (uint8_t i = 0; i < tracker.count; i++) {
        if (tracker.messages[i].message.id != messageId) {
            continue;
        }

        for (uint8_t j = i; j + 1 < tracker.count; j++) {
            tracker.messages[j] = tracker.messages[j + 1];
        }

        tracker.count--;
        tracker.messages[tracker.count] = {};
        return true;
    }

    return false;
}


/*
 * ============================================================
 * FIND
 * ============================================================
 */

PendingMessage* findPendingMessage(
    MessageTracker& tracker,
    uint32_t messageId
) {
    for (
        uint8_t i = 0;
        i < tracker.count;
        i++
    ) {

        if (
            tracker.messages[i]
                .message.id ==
            messageId
        ) {

            return
                &tracker.messages[i];
        }
    }


    return nullptr;
}


/*
 * ============================================================
 * ACK
 * ============================================================
 */

bool processAck(
    MessageTracker& tracker,
    const Message& ack
) {
    if (
        ack.type !=
        MessageType::ACK
    ) {
        return false;
    }


    /*
     * value2 contient l'ID
     * du message original.
     */

    uint32_t originalMessageId =
        static_cast<uint32_t>(
            ack.value2
        );


    PendingMessage* pending =
        findPendingMessage(
            tracker,
            originalMessageId
        );


    if (
        pending == nullptr
    ) {

        Serial.print(
            "ACK sans message correspondant : "
        );

        Serial.println(
            originalMessageId
        );

        return false;
    }


    if (isCoreTrackedCommand(pending->message)) {
        if (!pending->waitingForAck || pending->completed || pending->timedOut ||
            !validCoreAck(pending->message, ack)) {
            return false;
        }
        if (ack.executionStatus == ExecutionStatus::NOT_EXECUTED) {
            pending->accepted = true;
            pending->message.status = MessageStatus::DELIVERED;
            Serial.print("Commande acceptee par MAIN : ");
            Serial.println(originalMessageId);
            return true;
        }
        if (ack.sourceId == pending->message.destinationId ||
            ack.executionStatus != ExecutionStatus::FAILED) {
            pending->accepted = true;
        }
        if (ack.executionStatus == ExecutionStatus::FAILED) {
            // The request's value remains intact; value2 stores terminal cause.
            pending->message.value2 = ack.value;
        }
    }

    pending->waitingForAck =
        false;

    pending->completed =
        true;

    pending->timedOut =
        false;

    pending->completedAt =
        millis();


    pending->message.status =
        MessageStatus::DELIVERED;


    pending->message.executionStatus =
        ack.executionStatus;


    Serial.print(
        "ACK traite pour message : "
    );

    Serial.println(
        originalMessageId
    );


    return true;
}


/*
 * ============================================================
 * TIMEOUT + RETRY
 * ============================================================
 */

bool updateMessageTimeouts(
    MessageTracker& tracker,
    Communication& communication
) {
    bool changed = false;


    for (
        uint8_t i = 0;
        i < tracker.count;
        i++
    ) {

        PendingMessage& pending =
            tracker.messages[i];


        if (
            !pending.waitingForAck
        ) {
            continue;
        }


        const uint32_t now = millis();
        const uint32_t elapsed = now - pending.sentAt;
        const bool coreDeadlineExpired = isCoreTrackedCommand(pending.message) &&
            now - pending.firstSentAt >=
                MESSAGE_TIMEOUT * (static_cast<uint32_t>(MAX_MESSAGE_RETRIES) + 1);


        if (
            !coreDeadlineExpired && elapsed <
            MESSAGE_TIMEOUT
        ) {
            continue;
        }


        changed = true;


        Serial.println();

        Serial.print(
            "TIMEOUT message : "
        );

        Serial.println(
            pending.message.id
        );


        /*
         * RETRY
         */

        if (
            !coreDeadlineExpired && pending.retryCount <
            MAX_MESSAGE_RETRIES
        ) {

            pending.retryCount++;


            pending.sentAt =
                millis();


            pending.message.status =
                MessageStatus::PENDING;


            pending.message.executionStatus =
                ExecutionStatus::NOT_EXECUTED;


            Serial.print(
                "RETRY #"
            );

            Serial.println(
                pending.retryCount
            );


            /*
             * On réutilise exactement
             * le même Message ID.
             *
             * Cela permet au destinataire
             * d'utiliser le deduplicator.
             */

            sendMessage(
                communication,
                pending.message
            );


            continue;
        }


        /*
         * ECHEC DEFINITIF
         */

        pending.waitingForAck =
            false;

        pending.completed =
            true;

        pending.timedOut =
            true;

        pending.completedAt =
            millis();


        pending.message.status =
            MessageStatus::FAILED;


        pending.message.executionStatus =
            isCoreTrackedCommand(pending.message)
                ? ExecutionStatus::NOT_EXECUTED
                : ExecutionStatus::FAILED;


        Serial.println(
            "ECHEC DEFINITIF"
        );
    }


    return changed;
}


/*
 * ============================================================
 * PRINT PENDING
 * ============================================================
 */

void printPendingMessage(
    const PendingMessage& pending
) {
    Serial.println();

    Serial.println(
        "===== PENDING MESSAGE ====="
    );


    Serial.print(
        "ID : "
    );

    Serial.println(
        pending.message.id
    );


    Serial.print(
        "Destination : "
    );

    Serial.println(
        pending.message.destinationId
    );


    Serial.print(
        "Waiting ACK : "
    );

    Serial.println(
        pending.waitingForAck
            ? "OUI"
            : "NON"
    );

    Serial.print("Accepted : ");
    Serial.println(pending.accepted ? "OUI" : "NON");


    Serial.print(
        "Completed : "
    );

    Serial.println(
        pending.completed
            ? "OUI"
            : "NON"
    );


    Serial.print(
        "Timed out : "
    );

    Serial.println(
        pending.timedOut
            ? "OUI"
            : "NON"
    );


    Serial.print(
        "Retries : "
    );

    Serial.println(
        pending.retryCount
    );


    Serial.print(
        "Status : "
    );

    Serial.println(
        messageStatusToString(
            pending.message.status
        )
    );


    Serial.print(
        "Execution : "
    );

    Serial.println(
        executionStatusToString(
            pending.message.executionStatus
        )
    );


    Serial.println(
        "==========================="
    );
}


/*
 * ============================================================
 * PRINT TRACKER
 * ============================================================
 */

void printMessageTracker(
    const MessageTracker& tracker
) {
    Serial.println();

    Serial.println(
        "===== MESSAGE TRACKER ====="
    );


    Serial.print(
        "Messages suivis : "
    );

    Serial.println(
        tracker.count
    );


    for (
        uint8_t i = 0;
        i < tracker.count;
        i++
    ) {

        printPendingMessage(
            tracker.messages[i]
        );
    }


    Serial.println(
        "============================"
    );
}
