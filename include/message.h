#pragma once

#include <Arduino.h>


enum class MessageType {

    COMMAND,

    EVENT,

    STATE,

    HEARTBEAT,

    ACK,

    DEVICE_ANNOUNCE,
    PAIR_REQUEST,
    PAIR_ACCEPT,
    PAIR_CONFIRM,
    PAIR_REJECT
};


enum class MessageStatus {

    PENDING,

    SENT,

    DELIVERED,

    FAILED
};


enum class ExecutionStatus {

    NOT_EXECUTED,

    EXECUTED,

    PARTIAL,

    FAILED
};


struct Message {

    uint32_t id;

    uint32_t sourceId;

    uint32_t destinationId;

    MessageType type;

    uint32_t timestamp;

    int32_t commandType;

    int32_t value;

    int32_t value2;

    MessageStatus status;

    ExecutionStatus executionStatus;

    // V4 provisioning payload. Existing V3.9 messages leave these fields zeroed.
    char hardwareId[32];
    char name[32];
    char firmwareVersion[16];
    uint32_t parentMainId;
    uint32_t capabilities;
    uint32_t provisioningDeviceId;
    uint8_t deviceRole;
    uint8_t pairingState;
};
