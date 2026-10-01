#include <string.h>

#include "zigbee_message_codec.h"

namespace {
constexpr uint8_t MAGIC_0 = 'S';
constexpr uint8_t MAGIC_1 = 'L';
constexpr uint8_t WIRE_VERSION = 1;

void putU32(uint8_t* output, size_t& offset, uint32_t value) {
    output[offset++] = static_cast<uint8_t>(value);
    output[offset++] = static_cast<uint8_t>(value >> 8);
    output[offset++] = static_cast<uint8_t>(value >> 16);
    output[offset++] = static_cast<uint8_t>(value >> 24);
}

bool getU32(const uint8_t* input, size_t length, size_t& offset, uint32_t& value) {
    if (offset + 4 > length) {
        return false;
    }
    value = static_cast<uint32_t>(input[offset]) |
            (static_cast<uint32_t>(input[offset + 1]) << 8) |
            (static_cast<uint32_t>(input[offset + 2]) << 16) |
            (static_cast<uint32_t>(input[offset + 3]) << 24);
    offset += 4;
    return true;
}

bool appendText(
    const char* text,
    size_t maxLength,
    uint8_t* output,
    size_t outputCapacity,
    size_t& offset
) {
    size_t length = 0;
    while (length < maxLength && text[length] != '\0') {
        ++length;
    }
    if (offset + 1 + length > outputCapacity) {
        return false;
    }
    output[offset++] = static_cast<uint8_t>(length);
    if (length != 0) {
        memcpy(output + offset, text, length);
        offset += length;
    }
    return true;
}

bool readText(
    const uint8_t* input,
    size_t inputLength,
    size_t& offset,
    char* output,
    size_t outputCapacity
) {
    if (offset >= inputLength) {
        return false;
    }
    const size_t length = input[offset++];
    if (length >= outputCapacity || offset + length > inputLength) {
        return false;
    }
    if (length != 0) {
        memcpy(output, input + offset, length);
    }
    output[length] = '\0';
    offset += length;
    return true;
}

bool isValidType(uint8_t value) {
    return value <= static_cast<uint8_t>(MessageType::PAIR_REJECT);
}

bool isValidStatus(uint8_t value) {
    return value <= static_cast<uint8_t>(MessageStatus::FAILED);
}

bool isValidExecutionStatus(uint8_t value) {
    return value <= static_cast<uint8_t>(ExecutionStatus::FAILED);
}
}  // namespace

bool encodeZigbeeMessage(
    const Message& message,
    uint8_t* output,
    size_t outputCapacity,
    size_t& outputLength
) {
    outputLength = 0;
    if (output == nullptr || outputCapacity < 51 ||
        !isValidType(static_cast<uint8_t>(message.type)) ||
        !isValidStatus(static_cast<uint8_t>(message.status)) ||
        !isValidExecutionStatus(static_cast<uint8_t>(message.executionStatus))) {
        return false;
    }

    size_t offset = 0;
    output[offset++] = MAGIC_0;
    output[offset++] = MAGIC_1;
    output[offset++] = WIRE_VERSION;
    putU32(output, offset, message.id);
    putU32(output, offset, message.sourceId);
    putU32(output, offset, message.destinationId);
    putU32(output, offset, message.timestamp);
    output[offset++] = static_cast<uint8_t>(message.type);
    output[offset++] = static_cast<uint8_t>(message.status);
    output[offset++] = static_cast<uint8_t>(message.executionStatus);
    putU32(output, offset, static_cast<uint32_t>(message.commandType));
    putU32(output, offset, static_cast<uint32_t>(message.value));
    putU32(output, offset, static_cast<uint32_t>(message.value2));
    putU32(output, offset, message.parentMainId);
    putU32(output, offset, message.capabilities);
    putU32(output, offset, message.provisioningDeviceId);
    output[offset++] = message.deviceRole;
    output[offset++] = message.pairingState;

    if (!appendText(message.hardwareId, sizeof(message.hardwareId), output,
                    outputCapacity, offset) ||
        !appendText(message.name, sizeof(message.name), output,
                    outputCapacity, offset) ||
        !appendText(message.firmwareVersion, sizeof(message.firmwareVersion), output,
                    outputCapacity, offset)) {
        return false;
    }

    outputLength = offset;
    return true;
}

bool decodeZigbeeMessage(
    const uint8_t* input,
    size_t inputLength,
    Message& message
) {
    if (input == nullptr || inputLength < 3 || input[0] != MAGIC_0 ||
        input[1] != MAGIC_1 || input[2] != WIRE_VERSION) {
        return false;
    }

    Message decoded = {};
    size_t offset = 3;
    uint32_t raw = 0;
    if (!getU32(input, inputLength, offset, decoded.id) ||
        !getU32(input, inputLength, offset, decoded.sourceId) ||
        !getU32(input, inputLength, offset, decoded.destinationId) ||
        !getU32(input, inputLength, offset, decoded.timestamp) ||
        offset + 3 > inputLength) {
        return false;
    }

    const uint8_t type = input[offset++];
    const uint8_t status = input[offset++];
    const uint8_t executionStatus = input[offset++];
    if (!isValidType(type) || !isValidStatus(status) ||
        !isValidExecutionStatus(executionStatus)) {
        return false;
    }
    decoded.type = static_cast<MessageType>(type);
    decoded.status = static_cast<MessageStatus>(status);
    decoded.executionStatus = static_cast<ExecutionStatus>(executionStatus);

    if (!getU32(input, inputLength, offset, raw)) {
        return false;
    }
    decoded.commandType = static_cast<int32_t>(raw);
    if (!getU32(input, inputLength, offset, raw)) {
        return false;
    }
    decoded.value = static_cast<int32_t>(raw);
    if (!getU32(input, inputLength, offset, raw)) {
        return false;
    }
    decoded.value2 = static_cast<int32_t>(raw);
    if (!getU32(input, inputLength, offset, decoded.parentMainId) ||
        !getU32(input, inputLength, offset, decoded.capabilities) ||
        !getU32(input, inputLength, offset, decoded.provisioningDeviceId) ||
        offset + 2 > inputLength) {
        return false;
    }
    decoded.deviceRole = input[offset++];
    decoded.pairingState = input[offset++];

    if (!readText(input, inputLength, offset, decoded.hardwareId,
                  sizeof(decoded.hardwareId)) ||
        !readText(input, inputLength, offset, decoded.name,
                  sizeof(decoded.name)) ||
        !readText(input, inputLength, offset, decoded.firmwareVersion,
                  sizeof(decoded.firmwareVersion)) ||
        offset != inputLength) {
        return false;
    }

    message = decoded;
    return true;
}
