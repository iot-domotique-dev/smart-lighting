#pragma once

#include <Arduino.h>

#include "message.h"
#include "communication.h"

constexpr uint8_t MAX_PENDING_MESSAGES = 20;

constexpr uint32_t MESSAGE_TIMEOUT = 5000;

constexpr uint8_t MAX_MESSAGE_RETRIES = 2;


struct PendingMessage {

    Message message;

    bool waitingForAck;

    // MAIN receipt is separate from the final LAMP execution result.
    bool accepted;

    bool completed;

    bool timedOut;

    uint8_t retryCount;

    uint32_t sentAt;

    uint32_t firstSentAt;

    uint32_t completedAt;
};


struct MessageTracker {

    PendingMessage messages[
        MAX_PENDING_MESSAGES
    ];

    uint8_t count;
};


void initMessageTracker(
    MessageTracker& tracker
);


bool trackMessage(
    MessageTracker& tracker,
    const Message& message
);


bool untrackMessage(
    MessageTracker& tracker,
    uint32_t messageId
);


PendingMessage* findPendingMessage(
    MessageTracker& tracker,
    uint32_t messageId
);


bool processAck(
    MessageTracker& tracker,
    const Message& ack
);


bool updateMessageTimeouts(
    MessageTracker& tracker,
    Communication& communication
);


void printPendingMessage(
    const PendingMessage& message
);


void printMessageTracker(
    const MessageTracker& tracker
);
