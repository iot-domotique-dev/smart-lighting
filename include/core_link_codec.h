#pragma once

#include <stddef.h>
#include <stdint.h>

#include "message.h"

constexpr uint8_t CORE_LINK_PROTOCOL_VERSION = 1;
constexpr size_t CORE_LINK_MAX_NAME_LENGTH = 31;
constexpr size_t CORE_LINK_MAX_PAYLOAD_SIZE = 80;
constexpr size_t CORE_LINK_HEADER_SIZE = 8;
constexpr size_t CORE_LINK_CRC_SIZE = 2;
constexpr size_t CORE_LINK_MAX_FRAME_SIZE =
    CORE_LINK_HEADER_SIZE + CORE_LINK_MAX_PAYLOAD_SIZE + CORE_LINK_CRC_SIZE;

enum class CoreLinkMessageType : uint8_t {
    HELLO = 1,
    MODULE_ANNOUNCEMENT = 2,
    ID_ASSIGNMENT = 3,
    ACK = 4,
    STATE = 5,
    ERROR = 6,
    COMMAND = 7,
    COMMAND_RESULT = 8,
    PAIRING = 9
};

enum class CoreLinkPairingAction : uint8_t {
    NONE = 0,
    OPEN_SESSION = 1,
    CLOSE_SESSION = 2,
    CANDIDATE = 3,
    PAIR_CANDIDATE = 4,
    SESSION_RESULT = 5,
    OPERATION_RESULT = 6,
    CANDIDATE_LIMIT = 7,
    REMOVE_DEVICE = 8,
    QUERY_REMOVAL = 9,
    REMOVAL_RESULT = 10
};

enum class CorePairingResultCode : uint8_t {
    NONE = 0,
    ACTIVE = 1,
    CLOSED = 2,
    EXPIRED = 3,
    FAILED = 4,
    UNCERTAIN = 5,
    BUSY = 6,
    PROCESSING = 7,
    SUCCEEDED = 8,
    REJECTED = 9,
    CAPACITY = 10,
    NOT_FOUND = 11,
    ROUTE_UNAVAILABLE = 12
};

struct CoreLinkPacket {
    uint16_t sequence;
    CoreLinkMessageType type;
    uint32_t localId;
    uint32_t parentId;
    uint32_t coreId;
    uint32_t capabilities;
    uint32_t timestamp;
    uint8_t role;
    uint8_t resultCode;
    uint8_t statusCode;
    char name[CORE_LINK_MAX_NAME_LENGTH + 1];
    /* V7.2 uses the existing Message model inside the UART framing. */
    Message message;
    /* Bounded V7.6.3 pairing control and status fields. */
    CoreLinkPairingAction pairingAction;
    CorePairingResultCode pairingResult;
    uint16_t pairingDurationSeconds;
    uint32_t sessionId;
    uint32_t operationId;
    uint32_t deviceId;
    char hardwareId[32];
    char pairingName[32];
};

/* Encodes explicit fields into a versioned frame; never serializes the struct. */
bool encodeCoreLinkFrame(
    const CoreLinkPacket& packet,
    uint8_t* output,
    size_t outputCapacity,
    size_t& outputLength
);

/* Validates exact frame length, protocol version, message type, and CRC-16. */
bool decodeCoreLinkFrame(
    const uint8_t* input,
    size_t inputLength,
    CoreLinkPacket& packet
);

/* Incremental decoder for UART streams, including partial and concatenated frames. */
class CoreLinkFrameDecoder {
private:
    uint8_t buffer[CORE_LINK_MAX_FRAME_SIZE];
    size_t used;
    uint16_t errorCount;

    void discardFront(size_t count);

public:
    CoreLinkFrameDecoder();

    bool pushByte(uint8_t value, CoreLinkPacket& packet);
    uint16_t errors() const;
    void reset();
};
