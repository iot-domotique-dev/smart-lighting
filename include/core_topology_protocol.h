#pragma once

#include <stdint.h>

#include "core_link_codec.h"
#include "message.h"

/* V7 topology messages reuse existing wire fields and STATE message type. */
constexpr int32_t CORE_TOPOLOGY_MAIN_ANNOUNCEMENT = 0x43520001;
constexpr int32_t CORE_TOPOLOGY_ID_ASSIGNED = 0x43520002;
constexpr int32_t CORE_TOPOLOGY_ID_ACK = 0x43520003;

bool makeCoreMainAnnouncement(
    uint32_t localId,
    const char* name,
    uint32_t capabilities,
    Message& message
);

bool coreAnnouncementFromZigbeeMessage(
    const Message& message,
    uint32_t mainCoreId,
    uint32_t mainLocalId,
    CoreLinkPacket& packet
);

bool makeCoreIdAssignedMessage(
    uint32_t localId,
    uint32_t coreId,
    uint8_t resultCode,
    Message& message
);

bool readCoreIdAssignedMessage(
    const Message& message,
    uint32_t expectedLocalId,
    uint32_t& coreId,
    uint8_t& resultCode
);

bool makeCoreIdAckMessage(
    uint32_t localId,
    uint32_t coreId,
    bool accepted,
    Message& message
);

bool readCoreIdAckMessage(
    const Message& message,
    uint32_t expectedLocalId,
    uint32_t expectedCoreId,
    bool& accepted
);
