#pragma once

#include <stddef.h>
#include <stdint.h>

#include "message.h"

/* Versioned, little-endian APS payload; never sends the in-memory struct. */
constexpr size_t ZIGBEE_MESSAGE_MAX_ENCODED_SIZE = 128;

bool encodeZigbeeMessage(
    const Message& message,
    uint8_t* output,
    size_t outputCapacity,
    size_t& outputLength
);

bool decodeZigbeeMessage(
    const uint8_t* input,
    size_t inputLength,
    Message& message
);
