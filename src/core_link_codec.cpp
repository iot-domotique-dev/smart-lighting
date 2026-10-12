#include <string.h>

#include "core_link_codec.h"

namespace {
constexpr uint8_t MAGIC_0 = 0x53;
constexpr uint8_t MAGIC_1 = 0x43;
constexpr size_t FRAME_FIXED_SIZE = CORE_LINK_HEADER_SIZE + CORE_LINK_CRC_SIZE;
constexpr size_t COMMAND_PAYLOAD_SIZE = 39;
static_assert(COMMAND_PAYLOAD_SIZE <= CORE_LINK_MAX_PAYLOAD_SIZE,
              "Command messages must fit the existing UART payload");

void writeU16(uint8_t* output, size_t& offset, uint16_t value) {
    output[offset++] = static_cast<uint8_t>(value);
    output[offset++] = static_cast<uint8_t>(value >> 8);
}

void writeU32(uint8_t* output, size_t& offset, uint32_t value) {
    output[offset++] = static_cast<uint8_t>(value);
    output[offset++] = static_cast<uint8_t>(value >> 8);
    output[offset++] = static_cast<uint8_t>(value >> 16);
    output[offset++] = static_cast<uint8_t>(value >> 24);
}

bool readU16(const uint8_t* input, size_t length, size_t& offset, uint16_t& value) {
    if (offset + 2 > length) {
        return false;
    }
    value = static_cast<uint16_t>(input[offset]) |
            static_cast<uint16_t>(input[offset + 1] << 8);
    offset += 2;
    return true;
}

bool readU32(const uint8_t* input, size_t length, size_t& offset, uint32_t& value) {
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

uint16_t crc16Ccitt(const uint8_t* data, size_t length) {
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < length; ++i) {
        crc ^= static_cast<uint16_t>(data[i]) << 8;
        for (uint8_t bit = 0; bit < 8; ++bit) {
            crc = (crc & 0x8000) != 0
                ? static_cast<uint16_t>((crc << 1) ^ 0x1021)
                : static_cast<uint16_t>(crc << 1);
        }
    }
    return crc;
}

bool isValidType(uint8_t value) {
    return value >= static_cast<uint8_t>(CoreLinkMessageType::HELLO) &&
           value <= static_cast<uint8_t>(CoreLinkMessageType::PAIRING);
}

bool isValidPairingResult(uint8_t value) {
    return value > static_cast<uint8_t>(CorePairingResultCode::NONE) &&
           value <= static_cast<uint8_t>(CorePairingResultCode::ROUTE_UNAVAILABLE);
}

bool validPairingPacket(const CoreLinkPacket& packet) {
    if (packet.type != CoreLinkMessageType::PAIRING || packet.localId == 0 ||
        packet.pairingAction == CoreLinkPairingAction::NONE) return false;
    switch (packet.pairingAction) {
        case CoreLinkPairingAction::OPEN_SESSION:
            return packet.sessionId != 0 && packet.pairingDurationSeconds >= 15 &&
                   packet.pairingDurationSeconds <= 180;
        case CoreLinkPairingAction::CLOSE_SESSION:
            return packet.sessionId != 0;
        case CoreLinkPairingAction::CANDIDATE:
            return packet.sessionId != 0 && packet.hardwareId[0] != '\0';
        case CoreLinkPairingAction::PAIR_CANDIDATE:
            return packet.sessionId != 0 && packet.operationId != 0 &&
                   packet.hardwareId[0] != '\0' && packet.pairingName[0] != '\0';
        case CoreLinkPairingAction::SESSION_RESULT:
            return packet.sessionId != 0 &&
                   packet.pairingResult > CorePairingResultCode::NONE &&
                   packet.pairingResult <= CorePairingResultCode::ROUTE_UNAVAILABLE;
        case CoreLinkPairingAction::OPERATION_RESULT:
            return packet.sessionId != 0 && packet.operationId != 0 &&
                   packet.pairingResult > CorePairingResultCode::NONE &&
                   packet.pairingResult <= CorePairingResultCode::ROUTE_UNAVAILABLE;
        case CoreLinkPairingAction::CANDIDATE_LIMIT:
            return packet.sessionId != 0;
        case CoreLinkPairingAction::NONE:
            return false;
    }
    return false;
}

bool isValidCommandMessage(const Message& message, CoreLinkMessageType type) {
    const MessageType expected = type == CoreLinkMessageType::COMMAND
        ? MessageType::COMMAND : MessageType::ACK;
    return message.type == expected && message.id != 0 &&
        message.sourceId != 0 && message.destinationId != 0 &&
        static_cast<unsigned>(message.status) <=
            static_cast<unsigned>(MessageStatus::FAILED) &&
        static_cast<unsigned>(message.executionStatus) <=
            static_cast<unsigned>(ExecutionStatus::FAILED) &&
        (expected != MessageType::ACK || message.value2 != 0);
}

bool appendCommandMessage(
    const Message& message,
    CoreLinkMessageType type,
    uint8_t* output,
    size_t capacity,
    size_t& offset
) {
    if (!isValidCommandMessage(message, type) ||
        offset + COMMAND_PAYLOAD_SIZE > capacity) {
        return false;
    }
    writeU32(output, offset, message.id);
    writeU32(output, offset, message.sourceId);
    writeU32(output, offset, message.destinationId);
    writeU32(output, offset, message.timestamp);
    writeU32(output, offset, static_cast<uint32_t>(message.commandType));
    writeU32(output, offset, static_cast<uint32_t>(message.value));
    writeU32(output, offset, static_cast<uint32_t>(message.value2));
    writeU32(output, offset, message.parentMainId);
    writeU32(output, offset, message.provisioningDeviceId);
    output[offset++] = static_cast<uint8_t>(message.type);
    output[offset++] = static_cast<uint8_t>(message.status);
    output[offset++] = static_cast<uint8_t>(message.executionStatus);
    return true;
}

bool readCommandMessage(
    const uint8_t* input,
    size_t length,
    size_t& offset,
    CoreLinkMessageType type,
    Message& message
) {
    uint32_t commandType = 0;
    uint32_t value = 0;
    uint32_t value2 = 0;
    if (!readU32(input, length, offset, message.id) ||
        !readU32(input, length, offset, message.sourceId) ||
        !readU32(input, length, offset, message.destinationId) ||
        !readU32(input, length, offset, message.timestamp) ||
        !readU32(input, length, offset, commandType) ||
        !readU32(input, length, offset, value) ||
        !readU32(input, length, offset, value2) ||
        !readU32(input, length, offset, message.parentMainId) ||
        !readU32(input, length, offset, message.provisioningDeviceId) ||
        offset + 3 > length) {
        return false;
    }
    message.commandType = static_cast<int32_t>(commandType);
    message.value = static_cast<int32_t>(value);
    message.value2 = static_cast<int32_t>(value2);
    message.type = static_cast<MessageType>(input[offset++]);
    message.status = static_cast<MessageStatus>(input[offset++]);
    message.executionStatus = static_cast<ExecutionStatus>(input[offset++]);
    return isValidCommandMessage(message, type);
}

bool appendName(const char* name, uint8_t* output, size_t capacity, size_t& offset) {
    size_t length = 0;
    while (length <= CORE_LINK_MAX_NAME_LENGTH && name[length] != '\0') {
        ++length;
    }
    if (length > CORE_LINK_MAX_NAME_LENGTH || offset + 1 + length > capacity) {
        return false;
    }
    output[offset++] = static_cast<uint8_t>(length);
    if (length != 0) {
        memcpy(output + offset, name, length);
        offset += length;
    }
    return true;
}

bool readName(const uint8_t* input, size_t length, size_t& offset,
              char* name, size_t capacity);

bool appendPairingPacket(const CoreLinkPacket& packet, uint8_t* output,
                         size_t capacity, size_t& offset) {
    if (!validPairingPacket(packet) || offset > capacity ||
        capacity - offset < 5) return false;
    size_t fixedBytes = 0;
    switch (packet.pairingAction) {
        case CoreLinkPairingAction::OPEN_SESSION: fixedBytes = 6; break;
        case CoreLinkPairingAction::CLOSE_SESSION: fixedBytes = 4; break;
        case CoreLinkPairingAction::CANDIDATE: fixedBytes = 8; break;
        case CoreLinkPairingAction::PAIR_CANDIDATE: fixedBytes = 8; break;
        case CoreLinkPairingAction::SESSION_RESULT: fixedBytes = 5; break;
        case CoreLinkPairingAction::OPERATION_RESULT: fixedBytes = 13; break;
        case CoreLinkPairingAction::CANDIDATE_LIMIT: fixedBytes = 4; break;
        case CoreLinkPairingAction::NONE: return false;
    }
    if (capacity - offset - 5 < fixedBytes) return false;
    output[offset++] = static_cast<uint8_t>(packet.pairingAction);
    writeU32(output, offset, packet.localId);
    switch (packet.pairingAction) {
        case CoreLinkPairingAction::OPEN_SESSION:
            writeU32(output, offset, packet.sessionId);
            writeU16(output, offset, packet.pairingDurationSeconds);
            return offset <= capacity;
        case CoreLinkPairingAction::CLOSE_SESSION:
            writeU32(output, offset, packet.sessionId);
            return offset <= capacity;
        case CoreLinkPairingAction::CANDIDATE:
            writeU32(output, offset, packet.sessionId);
            writeU32(output, offset, packet.capabilities);
            return appendName(packet.hardwareId, output, capacity, offset);
        case CoreLinkPairingAction::PAIR_CANDIDATE:
            writeU32(output, offset, packet.sessionId);
            writeU32(output, offset, packet.operationId);
            return appendName(packet.hardwareId, output, capacity, offset) &&
                   appendName(packet.pairingName, output, capacity, offset);
        case CoreLinkPairingAction::SESSION_RESULT:
            writeU32(output, offset, packet.sessionId);
            output[offset++] = static_cast<uint8_t>(packet.pairingResult);
            return offset <= capacity;
        case CoreLinkPairingAction::OPERATION_RESULT:
            writeU32(output, offset, packet.sessionId);
            writeU32(output, offset, packet.operationId);
            output[offset++] = static_cast<uint8_t>(packet.pairingResult);
            writeU32(output, offset, packet.deviceId);
            return appendName(packet.hardwareId, output, capacity, offset);
        case CoreLinkPairingAction::CANDIDATE_LIMIT:
            writeU32(output, offset, packet.sessionId);
            return offset <= capacity;
        case CoreLinkPairingAction::NONE:
            return false;
    }
    return false;
}

bool readPairingPacket(const uint8_t* input, size_t length, size_t& offset,
                       CoreLinkPacket& packet) {
    if (offset >= length) return false;
    const uint8_t action = input[offset++];
    if (action == 0 || action > static_cast<uint8_t>(CoreLinkPairingAction::CANDIDATE_LIMIT) ||
        !readU32(input, length, offset, packet.localId)) return false;
    packet.type = CoreLinkMessageType::PAIRING;
    packet.pairingAction = static_cast<CoreLinkPairingAction>(action);
    switch (packet.pairingAction) {
        case CoreLinkPairingAction::OPEN_SESSION:
            return readU32(input, length, offset, packet.sessionId) &&
                   readU16(input, length, offset, packet.pairingDurationSeconds);
        case CoreLinkPairingAction::CLOSE_SESSION:
            return readU32(input, length, offset, packet.sessionId);
        case CoreLinkPairingAction::CANDIDATE:
            return readU32(input, length, offset, packet.sessionId) &&
                   readU32(input, length, offset, packet.capabilities) &&
                   readName(input, length, offset, packet.hardwareId,
                            sizeof(packet.hardwareId));
        case CoreLinkPairingAction::PAIR_CANDIDATE:
            return readU32(input, length, offset, packet.sessionId) &&
                   readU32(input, length, offset, packet.operationId) &&
                   readName(input, length, offset, packet.hardwareId,
                            sizeof(packet.hardwareId)) &&
                   readName(input, length, offset, packet.pairingName,
                            sizeof(packet.pairingName));
        case CoreLinkPairingAction::SESSION_RESULT:
            if (!readU32(input, length, offset, packet.sessionId) || offset >= length)
                return false;
            if (!isValidPairingResult(input[offset])) return false;
            packet.pairingResult = static_cast<CorePairingResultCode>(input[offset++]);
            return true;
        case CoreLinkPairingAction::OPERATION_RESULT:
            if (!readU32(input, length, offset, packet.sessionId) ||
                !readU32(input, length, offset, packet.operationId) ||
                offset >= length || !isValidPairingResult(input[offset])) return false;
            packet.pairingResult = static_cast<CorePairingResultCode>(input[offset++]);
            return readU32(input, length, offset, packet.deviceId) &&
                   readName(input, length, offset, packet.hardwareId,
                            sizeof(packet.hardwareId));
        case CoreLinkPairingAction::CANDIDATE_LIMIT:
            return readU32(input, length, offset, packet.sessionId);
        case CoreLinkPairingAction::NONE:
            return false;
    }
    return false;
}

bool readName(
    const uint8_t* input,
    size_t length,
    size_t& offset,
    char* name,
    size_t capacity
) {
    if (offset >= length) {
        return false;
    }
    const size_t nameLength = input[offset++];
    if (nameLength >= capacity || offset + nameLength > length) {
        return false;
    }
    if (nameLength != 0) {
        memcpy(name, input + offset, nameLength);
    }
    name[nameLength] = '\0';
    offset += nameLength;
    return true;
}

}  // namespace

bool encodeCoreLinkFrame(
    const CoreLinkPacket& packet,
    uint8_t* output,
    size_t outputCapacity,
    size_t& outputLength
) {
    outputLength = 0;
    if (output == nullptr || outputCapacity < FRAME_FIXED_SIZE) {
        return false;
    }

    size_t offset = CORE_LINK_HEADER_SIZE;
    switch (packet.type) {
        case CoreLinkMessageType::HELLO:
            if (offset + 4 > outputCapacity) return false;
            writeU32(output, offset, packet.coreId);
            break;

        case CoreLinkMessageType::MODULE_ANNOUNCEMENT:
            if (offset + 14 > outputCapacity) return false;
            writeU32(output, offset, packet.localId);
            writeU32(output, offset, packet.parentId);
            writeU32(output, offset, packet.capabilities);
            output[offset++] = packet.role;
            if (!appendName(packet.name, output, outputCapacity, offset)) {
                return false;
            }
            break;

        case CoreLinkMessageType::ID_ASSIGNMENT:
            if (offset + 9 > outputCapacity) return false;
            writeU32(output, offset, packet.localId);
            writeU32(output, offset, packet.coreId);
            output[offset++] = packet.resultCode;
            break;

        case CoreLinkMessageType::ACK:
            if (offset + 1 > outputCapacity) return false;
            output[offset++] = packet.resultCode;
            break;

        case CoreLinkMessageType::STATE:
            if (offset + 9 > outputCapacity) return false;
            writeU32(output, offset, packet.coreId);
            output[offset++] = packet.statusCode;
            writeU32(output, offset, packet.timestamp);
            break;

        case CoreLinkMessageType::ERROR:
            if (offset + 5 > outputCapacity) return false;
            writeU32(output, offset, packet.localId);
            output[offset++] = packet.resultCode;
            break;

        case CoreLinkMessageType::COMMAND:
        case CoreLinkMessageType::COMMAND_RESULT:
            if (!appendCommandMessage(packet.message, packet.type, output,
                                      outputCapacity, offset)) {
                return false;
            }
            break;

        case CoreLinkMessageType::PAIRING:
            if (!appendPairingPacket(packet, output, outputCapacity, offset)) {
                return false;
            }
            break;

        default:
            return false;
    }

    const size_t payloadLength = offset - CORE_LINK_HEADER_SIZE;
    if (payloadLength > CORE_LINK_MAX_PAYLOAD_SIZE ||
        offset + CORE_LINK_CRC_SIZE > outputCapacity) {
        return false;
    }

    output[0] = MAGIC_0;
    output[1] = MAGIC_1;
    output[2] = CORE_LINK_PROTOCOL_VERSION;
    output[3] = static_cast<uint8_t>(packet.type);
    output[4] = static_cast<uint8_t>(packet.sequence);
    output[5] = static_cast<uint8_t>(packet.sequence >> 8);
    output[6] = static_cast<uint8_t>(payloadLength);
    output[7] = static_cast<uint8_t>(payloadLength >> 8);
    const uint16_t crc = crc16Ccitt(output, offset);
    writeU16(output, offset, crc);
    outputLength = offset;
    return true;
}

bool decodeCoreLinkFrame(
    const uint8_t* input,
    size_t inputLength,
    CoreLinkPacket& packet
) {
    if (input == nullptr || inputLength < FRAME_FIXED_SIZE ||
        input[0] != MAGIC_0 || input[1] != MAGIC_1 ||
        input[2] != CORE_LINK_PROTOCOL_VERSION || !isValidType(input[3])) {
        return false;
    }

    const size_t payloadLength = static_cast<size_t>(input[6]) |
        (static_cast<size_t>(input[7]) << 8);
    if (payloadLength > CORE_LINK_MAX_PAYLOAD_SIZE ||
        inputLength != CORE_LINK_HEADER_SIZE + payloadLength + CORE_LINK_CRC_SIZE) {
        return false;
    }

    const size_t crcOffset = inputLength - CORE_LINK_CRC_SIZE;
    size_t crcReadOffset = crcOffset;
    uint16_t receivedCrc = 0;
    if (!readU16(input, inputLength, crcReadOffset, receivedCrc) ||
        receivedCrc != crc16Ccitt(input, crcOffset)) {
        return false;
    }

    CoreLinkPacket decoded = {};
    decoded.sequence = static_cast<uint16_t>(input[4]) |
                       static_cast<uint16_t>(input[5] << 8);
    decoded.type = static_cast<CoreLinkMessageType>(input[3]);
    size_t offset = CORE_LINK_HEADER_SIZE;
    const size_t payloadEnd = inputLength - CORE_LINK_CRC_SIZE;
    bool validPayload = false;
    switch (decoded.type) {
        case CoreLinkMessageType::HELLO:
            validPayload = readU32(input, payloadEnd, offset, decoded.coreId);
            break;

        case CoreLinkMessageType::MODULE_ANNOUNCEMENT:
            validPayload = readU32(input, payloadEnd, offset, decoded.localId) &&
                           readU32(input, payloadEnd, offset, decoded.parentId) &&
                           readU32(input, payloadEnd, offset, decoded.capabilities) &&
                           offset < payloadEnd;
            if (validPayload) {
                decoded.role = input[offset++];
                validPayload = readName(input, payloadEnd, offset, decoded.name,
                                        sizeof(decoded.name));
            }
            break;

        case CoreLinkMessageType::ID_ASSIGNMENT:
            validPayload = readU32(input, payloadEnd, offset, decoded.localId) &&
                           readU32(input, payloadEnd, offset, decoded.coreId) &&
                           offset < payloadEnd;
            if (validPayload) decoded.resultCode = input[offset++];
            break;

        case CoreLinkMessageType::ACK:
            validPayload = offset < payloadEnd;
            if (validPayload) decoded.resultCode = input[offset++];
            break;

        case CoreLinkMessageType::STATE:
            validPayload = readU32(input, payloadEnd, offset, decoded.coreId) &&
                           offset < payloadEnd;
            if (validPayload) {
                decoded.statusCode = input[offset++];
                validPayload = readU32(input, payloadEnd, offset, decoded.timestamp);
            }
            break;

        case CoreLinkMessageType::ERROR:
            validPayload = readU32(input, payloadEnd, offset, decoded.localId) &&
                           offset < payloadEnd;
            if (validPayload) decoded.resultCode = input[offset++];
            break;

        case CoreLinkMessageType::COMMAND:
        case CoreLinkMessageType::COMMAND_RESULT:
            validPayload = readCommandMessage(input, payloadEnd, offset,
                                               decoded.type, decoded.message);
            break;

        case CoreLinkMessageType::PAIRING:
            validPayload = readPairingPacket(input, payloadEnd, offset, decoded);
            break;

        default:
            return false;
    }

    if (!validPayload || offset != payloadEnd) {
        return false;
    }
    if (decoded.type == CoreLinkMessageType::PAIRING &&
        !validPairingPacket(decoded)) return false;
    packet = decoded;
    return true;
}

CoreLinkFrameDecoder::CoreLinkFrameDecoder() : used(0), errorCount(0) {}

void CoreLinkFrameDecoder::discardFront(size_t count) {
    if (count >= used) {
        used = 0;
        return;
    }
    memmove(buffer, buffer + count, used - count);
    used -= count;
}

bool CoreLinkFrameDecoder::pushByte(uint8_t value, CoreLinkPacket& packet) {
    if (used >= sizeof(buffer)) {
        discardFront(1);
        ++errorCount;
    }
    buffer[used++] = value;

    while (used != 0) {
        if (buffer[0] != MAGIC_0) {
            discardFront(1);
            continue;
        }
        if (used < 2) return false;
        if (buffer[1] != MAGIC_1) {
            discardFront(1);
            continue;
        }
        if (used < 3) return false;
        if (buffer[2] != CORE_LINK_PROTOCOL_VERSION) {
            discardFront(1);
            ++errorCount;
            continue;
        }
        if (used < CORE_LINK_HEADER_SIZE) return false;

        const size_t payloadLength = static_cast<size_t>(buffer[6]) |
            (static_cast<size_t>(buffer[7]) << 8);
        if (payloadLength > CORE_LINK_MAX_PAYLOAD_SIZE) {
            discardFront(1);
            ++errorCount;
            continue;
        }
        const size_t frameLength = CORE_LINK_HEADER_SIZE + payloadLength +
                                   CORE_LINK_CRC_SIZE;
        if (used < frameLength) return false;
        if (decodeCoreLinkFrame(buffer, frameLength, packet)) {
            discardFront(frameLength);
            return true;
        }
        discardFront(1);
        ++errorCount;
    }
    return false;
}

uint16_t CoreLinkFrameDecoder::errors() const {
    return errorCount;
}

void CoreLinkFrameDecoder::reset() {
    used = 0;
    errorCount = 0;
}
