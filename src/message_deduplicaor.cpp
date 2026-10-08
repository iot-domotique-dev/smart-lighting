#include <Arduino.h>

#include "message_deduplicator.h"
#include "message_manager.h"

namespace {
ProcessedMessage* allocateResultSlot(MessageDeduplicator& deduplicator,
                                     uint32_t now) {
    if (deduplicator.count < MAX_PROCESSED_MESSAGES) {
        return &deduplicator.messages[deduplicator.count++];
    }
    for (uint8_t offset = 0; offset < MAX_PROCESSED_MESSAGES; ++offset) {
        const uint8_t index = static_cast<uint8_t>(
            (deduplicator.nextIndex + offset) % MAX_PROCESSED_MESSAGES);
        ProcessedMessage& entry = deduplicator.messages[index];
        if (entry.valid && entry.protectedUntil != 0 &&
            static_cast<int32_t>(now - entry.protectedUntil) < 0) {
            continue;
        }
        deduplicator.nextIndex = static_cast<uint8_t>(
            (index + 1) % MAX_PROCESSED_MESSAGES);
        return &entry;
    }
    return nullptr;
}

void populateResult(ProcessedMessage& entry, uint32_t sourceId,
                    uint32_t messageId, ExecutionStatus executionStatus,
                    uint32_t now, uint32_t protectedUntil) {
    entry.sourceId = sourceId;
    entry.messageId = messageId;
    entry.executionStatus = executionStatus;
    entry.processedAt = now;
    entry.protectedUntil = protectedUntil;
    entry.destinationId = 0;
    entry.parentMainId = 0;
    entry.provisioningDeviceId = 0;
    entry.commandType = 0;
    entry.value = 0;
    entry.hasCommandIdentity = false;
    entry.valid = true;
}
}  // namespace

void initMessageDeduplicator(
    MessageDeduplicator& deduplicator
) {
    deduplicator.count = 0;
    deduplicator.nextIndex = 0;

    for (
        uint8_t i = 0;
        i < MAX_PROCESSED_MESSAGES;
        i++
    ) {
        deduplicator.messages[i].messageId = 0;
        deduplicator.messages[i].sourceId = 0;

        deduplicator.messages[i].executionStatus =
            ExecutionStatus::NOT_EXECUTED;

        deduplicator.messages[i].processedAt = 0;
        deduplicator.messages[i].protectedUntil = 0;
        deduplicator.messages[i].destinationId = 0;
        deduplicator.messages[i].parentMainId = 0;
        deduplicator.messages[i].provisioningDeviceId = 0;
        deduplicator.messages[i].commandType = 0;
        deduplicator.messages[i].value = 0;
        deduplicator.messages[i].hasCommandIdentity = false;

        deduplicator.messages[i].valid = false;
    }
}

ProcessedMessage* findProcessedMessage(
    MessageDeduplicator& deduplicator,
    uint32_t sourceId,
    uint32_t messageId
) {
    for (
        uint8_t i = 0;
        i < MAX_PROCESSED_MESSAGES;
        i++
    ) {
        ProcessedMessage& message =
            deduplicator.messages[i];

        if (
            message.valid &&
            message.sourceId == sourceId &&
            message.messageId == messageId
        ) {
            return &message;
        }
    }

    return nullptr;
}

bool isMessageProcessed(
    MessageDeduplicator& deduplicator,
    uint32_t sourceId,
    uint32_t messageId
) {
    return findProcessedMessage(
        deduplicator,
        sourceId,
        messageId
    ) != nullptr;
}

bool registerProcessedMessage(
    MessageDeduplicator& deduplicator,
    uint32_t sourceId,
    uint32_t messageId,
    ExecutionStatus executionStatus
) {
    /*
     * Le message existe déjà.
     */
    ProcessedMessage* existing =
        findProcessedMessage(
            deduplicator,
            sourceId,
            messageId
        );

    if (existing != nullptr) {

        existing->executionStatus =
            executionStatus;

        existing->processedAt =
            millis();

        return false;
    }

    const uint32_t now = millis();
    ProcessedMessage* entry = allocateResultSlot(deduplicator, now);
    if (entry == nullptr) return false;
    populateResult(*entry, sourceId, messageId, executionStatus, now, 0);
    return true;
}

bool reserveProcessedMessage(
    MessageDeduplicator& deduplicator,
    uint32_t sourceId,
    uint32_t messageId,
    uint32_t protectionMs
) {
    if (protectionMs == 0 || protectionMs > INT32_MAX) return false;
    if (findProcessedMessage(deduplicator, sourceId, messageId) != nullptr) {
        return true;
    }
    const uint32_t now = millis();
    ProcessedMessage* entry = allocateResultSlot(deduplicator, now);
    if (entry == nullptr) return false;
    uint32_t deadline = now + protectionMs;
    if (deadline == 0) deadline = 1;
    populateResult(*entry, sourceId, messageId, ExecutionStatus::NOT_EXECUTED,
                   now, deadline);
    return true;
}

void printMessageDeduplicator(
    const MessageDeduplicator& deduplicator
) {
    Serial.println();

    Serial.println(
        "===== MESSAGE DEDUPLICATOR ====="
    );

    Serial.print(
        "Messages memorises : "
    );

    Serial.println(
        deduplicator.count
    );

    for (
        uint8_t i = 0;
        i < deduplicator.count;
        i++
    ) {
        const ProcessedMessage& message =
            deduplicator.messages[i];

        if (!message.valid) {
            continue;
        }

        Serial.print(
            "Source : "
        );

        Serial.print(
            message.sourceId
        );

        Serial.print(" | ID : ");
        Serial.print(message.messageId);

        Serial.print(
            " | Execution : "
        );

        Serial.print(
            executionStatusToString(
                message.executionStatus
            )
        );

        Serial.print(
            " | Timestamp : "
        );

        Serial.println(
            message.processedAt
        );
    }

    Serial.println(
        "================================"
    );
}
