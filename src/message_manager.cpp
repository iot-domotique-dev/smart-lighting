#include <Arduino.h>

#include "message_manager.h"


const char* messageTypeToString(
    MessageType type
) {

    switch (type) {

        case MessageType::COMMAND:
            return "COMMAND";

        case MessageType::EVENT:
            return "EVENT";

        case MessageType::STATE:
            return "STATE";

        case MessageType::HEARTBEAT:
            return "HEARTBEAT";

        case MessageType::ACK:
            return "ACK";

        case MessageType::DEVICE_ANNOUNCE:
            return "DEVICE_ANNOUNCE";

        case MessageType::PAIR_REQUEST:
            return "PAIR_REQUEST";

        case MessageType::PAIR_ACCEPT:
            return "PAIR_ACCEPT";

        case MessageType::PAIR_CONFIRM:
            return "PAIR_CONFIRM";

        case MessageType::PAIR_REJECT:
            return "PAIR_REJECT";
    }

    return "UNKNOWN";
}


const char* messageStatusToString(
    MessageStatus status
) {

    switch (status) {

        case MessageStatus::PENDING:
            return "PENDING";

        case MessageStatus::SENT:
            return "SENT";

        case MessageStatus::DELIVERED:
            return "DELIVERED";

        case MessageStatus::FAILED:
            return "FAILED";
    }

    return "UNKNOWN";
}


const char* executionStatusToString(
    ExecutionStatus status
) {

    switch (status) {

        case ExecutionStatus::NOT_EXECUTED:
            return "NOT_EXECUTED";

        case ExecutionStatus::EXECUTED:
            return "EXECUTED";

        case ExecutionStatus::PARTIAL:
            return "PARTIAL";

        case ExecutionStatus::FAILED:
            return "FAILED";
    }

    return "UNKNOWN";
}


void printMessage(
    const Message& message
) {

    Serial.println("===== MESSAGE =====");

    Serial.print("ID : ");
    Serial.println(message.id);

    Serial.print("Source : ");
    Serial.println(message.sourceId);

    Serial.print("Destination : ");
    Serial.println(message.destinationId);

    Serial.print("Type : ");
    Serial.println(
        messageTypeToString(
            message.type
        )
    );

    Serial.print("Timestamp : ");
    Serial.println(message.timestamp);

    Serial.print("Command : ");
    Serial.println(message.commandType);

    Serial.print("Value : ");
    Serial.println(message.value);

    Serial.print("Value 2 : ");
    Serial.println(message.value2);

    Serial.print("Status : ");
    Serial.println(
        messageStatusToString(
            message.status
        )
    );

    Serial.print("Execution : ");
    Serial.println(
        executionStatusToString(
            message.executionStatus
        )
    );

    Serial.println("===================");
}
