#pragma once

#include <stddef.h>
#include <stdint.h>

constexpr uint8_t CORE_LINK_PROTOCOL_VERSION = 1;
constexpr size_t CORE_LINK_MAX_NAME_LENGTH = 31;
constexpr size_t CORE_LINK_MAX_PAYLOAD_SIZE = 46;
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
    ERROR = 6
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
