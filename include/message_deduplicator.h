#pragma once

#include <Arduino.h>

#include "message.h"

constexpr uint8_t MAX_PROCESSED_MESSAGES = 20;

struct ProcessedMessage {
    uint32_t sourceId;
    uint32_t messageId;

    ExecutionStatus executionStatus;

    uint32_t processedAt;

    // Entries remain available for retries until this local deadline.
    uint32_t protectedUntil;

    // Bind a CORE result to its original payload, so ID reuse cannot claim
    // execution of a different command.
    uint32_t destinationId;
    uint32_t parentMainId;
    uint32_t provisioningDeviceId;
    int32_t commandType;
    int32_t value;
    bool hasCommandIdentity;

    bool valid;
};

struct MessageDeduplicator {
    ProcessedMessage messages[MAX_PROCESSED_MESSAGES];

    uint8_t count;
    uint8_t nextIndex;
};

void initMessageDeduplicator(
    MessageDeduplicator& deduplicator
);

ProcessedMessage* findProcessedMessage(
    MessageDeduplicator& deduplicator,
    uint32_t sourceId,
    uint32_t messageId
);

bool isMessageProcessed(
    MessageDeduplicator& deduplicator,
    uint32_t sourceId,
    uint32_t messageId
);

bool registerProcessedMessage(
    MessageDeduplicator& deduplicator,
    uint32_t sourceId,
    uint32_t messageId,
    ExecutionStatus executionStatus
);

/* Reserve a result slot before executing. Fails if all slots are protected. */
bool reserveProcessedMessage(
    MessageDeduplicator& deduplicator,
    uint32_t sourceId,
    uint32_t messageId,
    uint32_t protectionMs
);

void printMessageDeduplicator(
    const MessageDeduplicator& deduplicator
);
