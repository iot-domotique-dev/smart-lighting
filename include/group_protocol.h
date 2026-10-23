#pragma once

#include <stdint.h>
#include <string.h>

#include "group.h"

/* V7.10.3 additive, explicitly versioned CORE ↔ MAIN group configuration. */
constexpr uint8_t GROUP_PROTOCOL_VERSION = 1;
constexpr uint8_t GROUP_POWER_PROTOCOL_VERSION = 2;
constexpr uint8_t GROUP_PROTOCOL_MAX_GROUPS = 10;
constexpr uint8_t GROUP_ZIGBEE_MEMBER_CHUNK = 8;
constexpr uint8_t GROUP_UART_MEMBER_CHUNK = 4;

enum class GroupProtocolAction : uint8_t {
    NONE = 0,
    LIST = 1,
    GET = 2,
    CREATE = 3,
    RENAME = 4,
    ADD_MEMBER = 5,
    REMOVE_MEMBER = 6,
    DELETE_GROUP = 7,
    RESULT = 8,
    LIST_BEGIN = 9,
    GROUP_META = 10,
    MEMBER_CHUNK = 11,
    LIST_END = 12,
    SET_POWER = 13,
    MEMBER_RESULT = 14,
    QUERY_OPERATION = 15
};

enum class GroupProtocolResult : uint8_t {
    NONE = 0,
    ACCEPTED = 1,
    PERSISTED = 2,
    COMPLETED = 3,
    NOT_FOUND = 4,
    REVISION_CONFLICT = 5,
    INVALID = 6,
    STORAGE_UNAVAILABLE = 7,
    BUSY = 8,
    FAILED = 9,
    UNCERTAIN = 10,
    PARTIAL = 11,
    EMPTY_GROUP = 12,
    OFFLINE = 13,
    UNSUPPORTED = 14,
    EXECUTED = 15,
    IN_PROGRESS = 16,
    SUPERSEDED = 17
};

/*
 * A group record is deliberately streamed as bounded metadata/member chunks.
 * No ten-member snapshot is assumed to fit in one UART or APS frame.
 */
struct GroupProtocolPayload {
    uint8_t version;
    GroupProtocolAction action;
    GroupProtocolResult result;
    uint32_t operationId;
    uint32_t mainLocalId;
    uint32_t mainCoreId;
    uint32_t groupId;
    uint32_t expectedRevision;
    uint32_t memberLocalId;
    uint32_t childCommandId;
    uint8_t groupCount;
    uint8_t memberCount;
    uint8_t memberOffset;
    uint8_t power;
    uint32_t memberIds[GROUP_ZIGBEE_MEMBER_CHUNK];
    char name[MAX_GROUP_NAME_LENGTH];
};

inline bool isValidGroupProtocolAction(GroupProtocolAction action) {
    return action >= GroupProtocolAction::LIST &&
           action <= GroupProtocolAction::QUERY_OPERATION;
}

inline bool isValidGroupProtocolResult(GroupProtocolResult result) {
    return result <= GroupProtocolResult::SUPERSEDED;
}

inline bool isGroupProtocolRequest(GroupProtocolAction action) {
    return action >= GroupProtocolAction::LIST &&
           (action <= GroupProtocolAction::DELETE_GROUP ||
            action == GroupProtocolAction::SET_POWER ||
            action == GroupProtocolAction::QUERY_OPERATION);
}

inline bool isGroupProtocolResponse(GroupProtocolAction action) {
    return action >= GroupProtocolAction::RESULT &&
           action <= GroupProtocolAction::MEMBER_RESULT;
}

inline bool isValidGroupProtocolPayload(const GroupProtocolPayload& group,
                                        uint8_t memberChunkLimit) {
    if ((group.version != GROUP_PROTOCOL_VERSION &&
         group.version != GROUP_POWER_PROTOCOL_VERSION) ||
        !isValidGroupProtocolAction(group.action) ||
        !isValidGroupProtocolResult(group.result) || group.operationId == 0 ||
        group.mainLocalId == 0 || group.mainCoreId == 0 ||
        memberChunkLimit == 0 ||
        memchr(group.name, '\0', sizeof(group.name)) == nullptr) return false;
    if (group.version == GROUP_PROTOCOL_VERSION &&
        (group.action > GroupProtocolAction::LIST_END ||
         group.childCommandId != 0 || group.power != 0)) return false;
    const bool hasName = group.name[0] != '\0';
    switch (group.action) {
        case GroupProtocolAction::LIST:
            return group.result == GroupProtocolResult::NONE &&
                group.groupId == 0 && group.expectedRevision == 0 &&
                group.memberLocalId == 0 && group.groupCount == 0 &&
                group.memberCount == 0 && group.memberOffset == 0 && !hasName;
        case GroupProtocolAction::GET:
            return group.result == GroupProtocolResult::NONE && group.groupId != 0 &&
                group.expectedRevision == 0 && group.memberLocalId == 0 &&
                group.groupCount == 0 && group.memberCount == 0 &&
                group.memberOffset == 0 && !hasName;
        case GroupProtocolAction::CREATE:
            return group.result == GroupProtocolResult::NONE &&
                group.groupId == group.operationId && group.expectedRevision == 0 &&
                group.memberLocalId == 0 && group.groupCount == 0 &&
                group.memberCount == 0 && group.memberOffset == 0 && hasName;
        case GroupProtocolAction::RENAME:
            return group.result == GroupProtocolResult::NONE && group.groupId != 0 &&
                group.expectedRevision != 0 && group.memberLocalId == 0 &&
                group.groupCount == 0 && group.memberCount == 0 &&
                group.memberOffset == 0 && hasName;
        case GroupProtocolAction::ADD_MEMBER:
        case GroupProtocolAction::REMOVE_MEMBER:
            return group.result == GroupProtocolResult::NONE && group.groupId != 0 &&
                group.expectedRevision != 0 && group.memberLocalId != 0 &&
                group.groupCount == 0 && group.memberCount == 0 &&
                group.memberOffset == 0 && !hasName;
        case GroupProtocolAction::DELETE_GROUP:
            return group.result == GroupProtocolResult::NONE && group.groupId != 0 &&
                group.expectedRevision != 0 && group.memberLocalId == 0 &&
                group.groupCount == 0 && group.memberCount == 0 &&
                group.memberOffset == 0 && !hasName;
        case GroupProtocolAction::RESULT:
            if (group.version == GROUP_POWER_PROTOCOL_VERSION) {
                return group.result != GroupProtocolResult::NONE &&
                    group.groupId != 0 && group.expectedRevision == 0 &&
                    group.memberLocalId == 0 && group.childCommandId == 0 &&
                    group.groupCount <= MAX_GROUP_LAMPS &&
                    group.memberCount <= group.groupCount &&
                    group.memberOffset == 0 && group.power <= 1 && !hasName;
            }
            return group.result != GroupProtocolResult::NONE &&
                group.memberLocalId == 0 && group.groupCount == 0 &&
                group.memberCount == 0 && group.memberOffset == 0 && !hasName;
        case GroupProtocolAction::LIST_BEGIN:
        case GroupProtocolAction::LIST_END:
            return group.result == GroupProtocolResult::NONE && group.groupId == 0 &&
                group.expectedRevision == 0 && group.memberLocalId == 0 &&
                group.groupCount <= GROUP_PROTOCOL_MAX_GROUPS &&
                group.memberCount == 0 && group.memberOffset == 0 && !hasName;
        case GroupProtocolAction::GROUP_META:
            return group.result == GroupProtocolResult::NONE && group.groupId != 0 &&
                group.expectedRevision != 0 && group.memberLocalId == 0 &&
                group.groupCount == 0 && group.memberCount <= MAX_GROUP_LAMPS &&
                group.memberOffset == 0 && hasName;
        case GroupProtocolAction::MEMBER_CHUNK:
            if (group.result != GroupProtocolResult::NONE || group.groupId == 0 ||
                group.groupCount != 0 || group.memberLocalId != 0 ||
                group.memberCount == 0 || group.memberCount > memberChunkLimit ||
                static_cast<uint16_t>(group.memberOffset) + group.memberCount >
                    MAX_GROUP_LAMPS || hasName) return false;
            for (uint8_t index = 0; index < group.memberCount; ++index)
                if (group.memberIds[index] == 0) return false;
            return true;
        case GroupProtocolAction::SET_POWER:
            return group.version == GROUP_POWER_PROTOCOL_VERSION &&
                group.result == GroupProtocolResult::NONE && group.groupId != 0 &&
                group.expectedRevision == 0 && group.memberLocalId == 0 &&
                group.childCommandId == 0 && group.groupCount == 0 &&
                group.memberCount == 0 && group.memberOffset == 0 &&
                group.power <= 1 && !hasName;
        case GroupProtocolAction::QUERY_OPERATION:
            return group.version == GROUP_POWER_PROTOCOL_VERSION &&
                group.result == GroupProtocolResult::NONE && group.groupId != 0 &&
                group.expectedRevision == 0 && group.memberLocalId == 0 &&
                group.childCommandId == 0 && group.groupCount == 0 &&
                group.memberCount == 0 && group.memberOffset == 0 &&
                group.power <= 1 && !hasName;
        case GroupProtocolAction::MEMBER_RESULT:
            return group.version == GROUP_POWER_PROTOCOL_VERSION &&
                (group.result == GroupProtocolResult::IN_PROGRESS ||
                 group.result == GroupProtocolResult::EXECUTED ||
                 group.result == GroupProtocolResult::FAILED ||
                 group.result == GroupProtocolResult::OFFLINE ||
                 group.result == GroupProtocolResult::UNSUPPORTED ||
                 group.result == GroupProtocolResult::UNCERTAIN ||
                 group.result == GroupProtocolResult::SUPERSEDED) &&
                group.groupId != 0 && group.expectedRevision == 0 &&
                group.memberLocalId != 0 && group.childCommandId != 0 &&
                group.groupCount > 0 && group.groupCount <= MAX_GROUP_LAMPS &&
                group.memberOffset < group.groupCount && group.memberCount == 0 &&
                group.power <= 1 && !hasName;
        case GroupProtocolAction::NONE:
            return false;
    }
    return false;
}
