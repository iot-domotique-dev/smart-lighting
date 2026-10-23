#include "core_http_api.h"

#if defined(SMART_LIGHTING_CORE_WIFI)

#include <Arduino.h>
#include <stdio.h>
#include <string.h>

#include "core_command_protocol.h"
#include "core_http_brightness_parser.h"
#include "core_http_auth.h"
#include "core_module_service.h"
#include "esp_random.h"
#include "esp_app_desc.h"
#include "esp_timer.h"
#include "freertos/task.h"

namespace {
constexpr size_t MAX_COMMAND_BODY_BYTES = 64;
constexpr size_t MAX_PAIRING_BODY_BYTES = 160;
constexpr size_t MAX_REMOVAL_BODY_BYTES = 64;
constexpr size_t MAX_GROUP_BODY_BYTES = 192;
constexpr uint32_t GROUP_OPERATION_TIMEOUT_MS = 8000;
constexpr uint32_t GROUP_POWER_OPERATION_TIMEOUT_MS = 20000;
constexpr uint32_t GROUP_POWER_STATUS_REQUERY_MS = 5000;
constexpr uint32_t GROUP_POWER_STATUS_QUERY_TIMEOUT_MS = 3000;
constexpr uint32_t GROUP_OPERATION_RESULT_RETENTION_MS = 2U * 60U * 1000U;
constexpr size_t MAX_AUTHORIZATION_BYTES = 160;
char responseBuffer[CORE_API_MAX_RESPONSE_BYTES];

const char* statusText(uint16_t status) {
    switch (status) {
        case 200: return "200 OK";
        case 202: return "202 Accepted";
        case 400: return "400 Bad Request";
        case 401: return "401 Unauthorized";
        case 404: return "404 Not Found";
        case 405: return "405 Method Not Allowed";
        case 408: return "408 Request Timeout";
        case 409: return "409 Conflict";
        case 413: return "413 Payload Too Large";
        case 415: return "415 Unsupported Media Type";
        case 500: return "500 Internal Server Error";
        case 503: return "503 Service Unavailable";
        default: return "500 Internal Server Error";
    }
}

bool parseId(const char* first, const char* last, uint32_t& id) {
    if (first == nullptr || last == nullptr || first == last) return false;
    uint32_t parsed = 0;
    for (const char* cursor = first; cursor != last; ++cursor) {
        if (*cursor < '0' || *cursor > '9') return false;
        const uint32_t digit = static_cast<uint32_t>(*cursor - '0');
        if (parsed > (UINT32_MAX - digit) / 10U) return false;
        parsed = parsed * 10U + digit;
    }
    if (parsed == 0) return false;
    id = parsed;
    return true;
}

enum class CommandRouteKind : uint8_t {
    NONE, SUBMIT_POWER, SUBMIT_BRIGHTNESS, STATUS, INVALID
};

enum class PairingRouteKind : uint8_t {
    NONE,
    SESSION,
    CANDIDATES,
    OPERATION_CREATE,
    OPERATION_STATUS,
    INVALID
};

enum class RemovalRouteKind : uint8_t { NONE, CREATE, STATUS, INVALID };

struct RemovalRoute {
    RemovalRouteKind kind;
    uint32_t id;
};

enum class GroupRouteKind : uint8_t {
    NONE, COLLECTION, ITEM, POWER, OPERATION, INVALID
};

struct GroupRoute {
    GroupRouteKind kind;
    uint32_t mainId;
    uint32_t id;
};

struct PairingRoute {
    PairingRouteKind kind;
    uint32_t id;
};

struct CommandRoute {
    CommandRouteKind kind;
    uint32_t id;
};

CommandRoute parseCommandRoute(const char* uri) {
    if (uri == nullptr) return {CommandRouteKind::NONE, 0};
    if (strncmp(uri, "/api/v1/commands/", 17) == 0) {
        const char* idStart = uri + 17;
        const char* idEnd = idStart + strlen(idStart);
        uint32_t id = 0;
        if (strchr(idStart, '/') != nullptr || strchr(idStart, '?') != nullptr ||
            !parseId(idStart, idEnd, id)) {
            return {CommandRouteKind::INVALID, 0};
        }
        return {CommandRouteKind::STATUS, id};
    }

    constexpr char DEVICE_PREFIX[] = "/api/v1/devices/";
    if (strncmp(uri, DEVICE_PREFIX, sizeof(DEVICE_PREFIX) - 1) != 0) {
        return {CommandRouteKind::NONE, 0};
    }
    const char* idStart = uri + sizeof(DEVICE_PREFIX) - 1;
    const char* suffix = strstr(idStart, "/commands/");
    if (suffix == nullptr) return {CommandRouteKind::NONE, 0};
    uint32_t id = 0;
    if (!parseId(idStart, suffix, id) || strchr(uri, '?') != nullptr) {
        return {CommandRouteKind::INVALID, 0};
    }
    if (strcmp(suffix, "/commands/power") == 0)
        return {CommandRouteKind::SUBMIT_POWER, id};
    if (strcmp(suffix, "/commands/brightness") == 0)
        return {CommandRouteKind::SUBMIT_BRIGHTNESS, id};
    return {CommandRouteKind::INVALID, 0};
}

PairingRoute parsePairingRoute(const char* uri) {
    if (uri == nullptr || strncmp(uri, "/api/v1/pairing", 15) != 0)
        return {PairingRouteKind::NONE, 0};
    if (strcmp(uri, "/api/v1/pairing/session") == 0)
        return {PairingRouteKind::SESSION, 0};
    if (strcmp(uri, "/api/v1/pairing/candidates") == 0)
        return {PairingRouteKind::CANDIDATES, 0};
    if (strcmp(uri, "/api/v1/pairing/operations") == 0)
        return {PairingRouteKind::OPERATION_CREATE, 0};
    constexpr char OPERATION_PREFIX[] = "/api/v1/pairing/operations/";
    if (strncmp(uri, OPERATION_PREFIX, sizeof(OPERATION_PREFIX) - 1) == 0) {
        const char* idStart = uri + sizeof(OPERATION_PREFIX) - 1;
        const char* idEnd = idStart + strlen(idStart);
        uint32_t id = 0;
        if (strchr(idStart, '/') != nullptr || strchr(idStart, '?') != nullptr ||
            !parseId(idStart, idEnd, id)) return {PairingRouteKind::INVALID, 0};
        return {PairingRouteKind::OPERATION_STATUS, id};
    }
    return {PairingRouteKind::INVALID, 0};
}

RemovalRoute parseRemovalRoute(const char* uri) {
    if (uri == nullptr) return {RemovalRouteKind::NONE, 0};
    constexpr char DEVICE_PREFIX[] = "/api/v1/devices/";
    constexpr char CREATE_SUFFIX[] = "/removal-operations";
    if (strncmp(uri, DEVICE_PREFIX, sizeof(DEVICE_PREFIX) - 1) == 0) {
        const char* idStart = uri + sizeof(DEVICE_PREFIX) - 1;
        const char* suffix = strstr(idStart, CREATE_SUFFIX);
        if (suffix == nullptr) return {RemovalRouteKind::NONE, 0};
        uint32_t id = 0;
        if (!parseId(idStart, suffix, id) ||
            strcmp(suffix, CREATE_SUFFIX) != 0 || strchr(uri, '?') != nullptr)
            return {RemovalRouteKind::INVALID, 0};
        return {RemovalRouteKind::CREATE, id};
    }
    constexpr char STATUS_PREFIX[] = "/api/v1/removal-operations/";
    if (strncmp(uri, STATUS_PREFIX, sizeof(STATUS_PREFIX) - 1) == 0) {
        const char* idStart = uri + sizeof(STATUS_PREFIX) - 1;
        uint32_t id = 0;
        if (strchr(idStart, '/') != nullptr || strchr(idStart, '?') != nullptr ||
            !parseId(idStart, idStart + strlen(idStart), id))
            return {RemovalRouteKind::INVALID, 0};
        return {RemovalRouteKind::STATUS, id};
    }
    if (strncmp(uri, "/api/v1/removal-operations", 26) == 0)
        return {RemovalRouteKind::INVALID, 0};
    return {RemovalRouteKind::NONE, 0};
}

GroupRoute parseGroupRoute(const char* uri) {
    if (uri == nullptr) return {GroupRouteKind::NONE, 0, 0};
    constexpr char OPERATION_PREFIX[] = "/api/v1/group-operations/";
    if (strncmp(uri, OPERATION_PREFIX, sizeof(OPERATION_PREFIX) - 1) == 0) {
        const char* start = uri + sizeof(OPERATION_PREFIX) - 1;
        uint32_t id = 0;
        if (strchr(start, '/') != nullptr || strchr(start, '?') != nullptr ||
            !parseId(start, start + strlen(start), id))
            return {GroupRouteKind::INVALID, 0, 0};
        return {GroupRouteKind::OPERATION, 0, id};
    }
    constexpr char MODULE_PREFIX[] = "/api/v1/modules/";
    if (strncmp(uri, MODULE_PREFIX, sizeof(MODULE_PREFIX) - 1) != 0)
        return {GroupRouteKind::NONE, 0, 0};
    const char* start = uri + sizeof(MODULE_PREFIX) - 1;
    const char* suffix = strstr(start, "/groups");
    if (suffix == nullptr) return {GroupRouteKind::NONE, 0, 0};
    uint32_t mainId = 0;
    if (!parseId(start, suffix, mainId) || strchr(uri, '?') != nullptr)
        return {GroupRouteKind::INVALID, 0, 0};
    if (strcmp(suffix, "/groups") == 0)
        return {GroupRouteKind::COLLECTION, mainId, 0};
    constexpr char ITEM_PREFIX[] = "/groups/";
    if (strncmp(suffix, ITEM_PREFIX, sizeof(ITEM_PREFIX) - 1) == 0) {
        const char* idStart = suffix + sizeof(ITEM_PREFIX) - 1;
        const char* idEnd = strchr(idStart, '/');
        if (idEnd == nullptr) idEnd = idStart + strlen(idStart);
        uint32_t groupId = 0;
        if (!parseId(idStart, idEnd, groupId))
            return {GroupRouteKind::INVALID, 0, 0};
        if (strcmp(idEnd, "/commands/power") == 0)
            return {GroupRouteKind::POWER, mainId, groupId};
        if (*idEnd != '\0') return {GroupRouteKind::INVALID, 0, 0};
        return {GroupRouteKind::ITEM, mainId, groupId};
    }
    return {GroupRouteKind::INVALID, 0, 0};
}

void skipJsonWhitespace(const char*& cursor, const char* end) {
    while (cursor != end && (*cursor == ' ' || *cursor == '\t' ||
           *cursor == '\r' || *cursor == '\n')) ++cursor;
}

bool parseJsonString(const char*& cursor, const char* end,
                     char* output, size_t capacity) {
    if (cursor == end || *cursor++ != '"' || capacity == 0) return false;
    size_t length = 0;
    while (cursor != end && *cursor != '"') {
        const unsigned char value = static_cast<unsigned char>(*cursor++);
        if (value < 0x20 || value >= 0x7f || value == '\\' ||
            length + 1 >= capacity) return false;
        output[length++] = static_cast<char>(value);
    }
    if (cursor == end || *cursor++ != '"') return false;
    output[length] = '\0';
    return true;
}

bool parseJsonUnsigned(const char*& cursor, const char* end, uint32_t& value) {
    if (cursor == end || *cursor < '0' || *cursor > '9') return false;
    uint32_t parsed = 0;
    do {
        const uint32_t digit = static_cast<uint32_t>(*cursor++ - '0');
        if (parsed > (UINT32_MAX - digit) / 10U) return false;
        parsed = parsed * 10U + digit;
    } while (cursor != end && *cursor >= '0' && *cursor <= '9');
    value = parsed;
    return true;
}

bool isJsonContentType(httpd_req_t* request);

bool parsePairingBody(httpd_req_t* request, char* body, size_t capacity) {
    if (request->content_len == 0 || request->content_len >= capacity ||
        !isJsonContentType(request)) return false;
    size_t received = 0;
    while (received < request->content_len) {
        const int count = httpd_req_recv(request, body + received,
            request->content_len - received);
        if (count <= 0) return false;
        received += static_cast<size_t>(count);
    }
    body[received] = '\0';
    return true;
}

bool parseSessionBody(const char* body, uint16_t& durationSeconds) {
    const char* cursor = body;
    const char* end = body + strlen(body);
    skipJsonWhitespace(cursor, end);
    if (cursor == end || *cursor++ != '{') return false;
    skipJsonWhitespace(cursor, end);
    char key[32] = {};
    if (!parseJsonString(cursor, end, key, sizeof(key)) ||
        strcmp(key, "duration_seconds") != 0) return false;
    skipJsonWhitespace(cursor, end);
    if (cursor == end || *cursor++ != ':') return false;
    skipJsonWhitespace(cursor, end);
    uint32_t duration = 0;
    if (!parseJsonUnsigned(cursor, end, duration)) return false;
    skipJsonWhitespace(cursor, end);
    if (cursor == end || *cursor++ != '}') return false;
    skipJsonWhitespace(cursor, end);
    if (cursor != end || duration < 15 || duration > 180) return false;
    durationSeconds = static_cast<uint16_t>(duration);
    return true;
}

bool parseRemovalBody(const char* body, uint32_t& operationId) {
    const char* cursor = body;
    const char* end = body + strlen(body);
    skipJsonWhitespace(cursor, end);
    if (cursor == end || *cursor++ != '{') return false;
    skipJsonWhitespace(cursor, end);
    char key[32] = {};
    if (!parseJsonString(cursor, end, key, sizeof(key)) ||
        strcmp(key, "operation_id") != 0) return false;
    skipJsonWhitespace(cursor, end);
    if (cursor == end || *cursor++ != ':') return false;
    skipJsonWhitespace(cursor, end);
    if (!parseJsonUnsigned(cursor, end, operationId) || operationId == 0)
        return false;
    skipJsonWhitespace(cursor, end);
    if (cursor == end || *cursor++ != '}') return false;
    skipJsonWhitespace(cursor, end);
    return cursor == end;
}

struct GroupHttpBody {
    uint32_t operationId;
    uint32_t expectedRevision;
    uint32_t lampId;
    char name[MAX_GROUP_NAME_LENGTH];
    char memberAction[8];
    char power[4];
    bool haveOperationId;
    bool haveExpectedRevision;
    bool haveLampId;
    bool haveName;
    bool haveMemberAction;
    bool havePower;
};

bool parseGroupBody(const char* body, GroupHttpBody& parsed) {
    parsed = {};
    const char* cursor = body;
    const char* end = body + strlen(body);
    bool closed = false;
    skipJsonWhitespace(cursor, end);
    if (cursor == end || *cursor++ != '{') return false;
    while (cursor != end) {
        skipJsonWhitespace(cursor, end);
        if (cursor != end && *cursor == '}') {
            ++cursor;
            closed = true;
            break;
        }
        char key[32] = {};
        if (!parseJsonString(cursor, end, key, sizeof(key))) return false;
        skipJsonWhitespace(cursor, end);
        if (cursor == end || *cursor++ != ':') return false;
        skipJsonWhitespace(cursor, end);
        if (strcmp(key, "operation_id") == 0 && !parsed.haveOperationId) {
            if (!parseJsonUnsigned(cursor, end, parsed.operationId) ||
                parsed.operationId == 0) return false;
            parsed.haveOperationId = true;
        } else if (strcmp(key, "expected_revision") == 0 &&
                   !parsed.haveExpectedRevision) {
            if (!parseJsonUnsigned(cursor, end, parsed.expectedRevision) ||
                parsed.expectedRevision == 0) return false;
            parsed.haveExpectedRevision = true;
        } else if (strcmp(key, "lamp_id") == 0 && !parsed.haveLampId) {
            if (!parseJsonUnsigned(cursor, end, parsed.lampId) ||
                parsed.lampId == 0) return false;
            parsed.haveLampId = true;
        } else if (strcmp(key, "name") == 0 && !parsed.haveName) {
            if (!parseJsonString(cursor, end, parsed.name,
                                 sizeof(parsed.name))) return false;
            parsed.haveName = true;
        } else if (strcmp(key, "member_action") == 0 &&
                   !parsed.haveMemberAction) {
            if (!parseJsonString(cursor, end, parsed.memberAction,
                                 sizeof(parsed.memberAction))) return false;
            parsed.haveMemberAction = true;
        } else if (strcmp(key, "power") == 0 && !parsed.havePower) {
            if (!parseJsonString(cursor, end, parsed.power,
                                 sizeof(parsed.power)) ||
                (strcmp(parsed.power, "on") != 0 &&
                 strcmp(parsed.power, "off") != 0)) return false;
            parsed.havePower = true;
        } else {
            return false;
        }
        skipJsonWhitespace(cursor, end);
        if (cursor != end && *cursor == ',') {
            ++cursor;
            skipJsonWhitespace(cursor, end);
            if (cursor == end || *cursor == '}') return false;
            continue;
        }
        if (cursor != end && *cursor == '}') {
            ++cursor;
            closed = true;
            break;
        }
        return false;
    }
    skipJsonWhitespace(cursor, end);
    return closed && cursor == end && parsed.haveOperationId;
}

const char* removalStateName(CorePairingResultCode result) {
    switch (result) {
        case CorePairingResultCode::PROCESSING: return "processing";
        case CorePairingResultCode::SUCCEEDED: return "succeeded";
        case CorePairingResultCode::UNCERTAIN: return "uncertain";
        case CorePairingResultCode::NOT_FOUND: return "not_found";
        case CorePairingResultCode::FAILED:
        case CorePairingResultCode::REJECTED:
        case CorePairingResultCode::BUSY:
        case CorePairingResultCode::ROUTE_UNAVAILABLE:
            return "failed";
        default: return "uncertain";
    }
}

const char* groupOperationStateName(CoreGroupOperationState state) {
    switch (state) {
        case CoreGroupOperationState::SENT: return "sent";
        case CoreGroupOperationState::ACCEPTED: return "accepted";
        case CoreGroupOperationState::PERSISTED: return "persisted";
        case CoreGroupOperationState::COMPLETED: return "completed";
        case CoreGroupOperationState::FAILED: return "failed";
        case CoreGroupOperationState::UNCERTAIN: return "uncertain";
        case CoreGroupOperationState::EMPTY: return "unknown";
    }
    return "unknown";
}

const char* groupResultName(GroupProtocolResult result) {
    switch (result) {
        case GroupProtocolResult::ACCEPTED: return "accepted";
        case GroupProtocolResult::PERSISTED: return "persisted";
        case GroupProtocolResult::COMPLETED: return "completed";
        case GroupProtocolResult::NOT_FOUND: return "not_found";
        case GroupProtocolResult::REVISION_CONFLICT: return "revision_conflict";
        case GroupProtocolResult::INVALID: return "invalid";
        case GroupProtocolResult::STORAGE_UNAVAILABLE: return "storage_unavailable";
        case GroupProtocolResult::BUSY: return "busy";
        case GroupProtocolResult::FAILED: return "failed";
        case GroupProtocolResult::UNCERTAIN: return "uncertain";
        case GroupProtocolResult::PARTIAL: return "partial";
        case GroupProtocolResult::EMPTY_GROUP: return "empty_group";
        case GroupProtocolResult::OFFLINE: return "offline";
        case GroupProtocolResult::UNSUPPORTED: return "unsupported";
        case GroupProtocolResult::EXECUTED: return "executed";
        case GroupProtocolResult::IN_PROGRESS: return "in_progress";
        case GroupProtocolResult::SUPERSEDED: return "superseded";
        case GroupProtocolResult::NONE: return "none";
    }
    return "failed";
}

bool groupOperationTerminal(CoreGroupOperationState state) {
    return state == CoreGroupOperationState::PERSISTED ||
           state == CoreGroupOperationState::COMPLETED ||
           state == CoreGroupOperationState::FAILED ||
           state == CoreGroupOperationState::UNCERTAIN;
}

bool groupOperationReusable(const CoreGroupOperationRecord& operation,
                            uint32_t now) {
    if (operation.state == CoreGroupOperationState::EMPTY) return true;
    // An uncertain outcome can still be resolved by a late MAIN result or ACK.
    // Keep its operation ID and member correlations until it is resolved.
    if (operation.state == CoreGroupOperationState::UNCERTAIN) return false;
    return (operation.state == CoreGroupOperationState::PERSISTED ||
            operation.state == CoreGroupOperationState::COMPLETED ||
            operation.state == CoreGroupOperationState::FAILED) &&
        operation.retainedAt != 0 &&
        now - operation.retainedAt >= GROUP_OPERATION_RESULT_RETENTION_MS;
}

void updateGroupOperationRetention(CoreGroupOperationRecord& operation,
                                   CoreGroupOperationState previousState,
                                   uint32_t now) {
    if (operation.state == previousState) return;
    if (operation.state == CoreGroupOperationState::SENT ||
        operation.state == CoreGroupOperationState::ACCEPTED) {
        operation.retainedAt = 0;
    } else if (operation.state == CoreGroupOperationState::UNCERTAIN ||
               operation.state == CoreGroupOperationState::PERSISTED ||
               operation.state == CoreGroupOperationState::COMPLETED ||
               operation.state == CoreGroupOperationState::FAILED) {
        operation.retainedAt = now;
    }
}

bool sameGroupRequest(const GroupProtocolPayload& left,
                      const GroupProtocolPayload& right) {
    return left.version == right.version && left.action == right.action &&
        left.mainLocalId == right.mainLocalId &&
        left.mainCoreId == right.mainCoreId && left.groupId == right.groupId &&
        left.expectedRevision == right.expectedRevision &&
        left.memberLocalId == right.memberLocalId && left.power == right.power &&
        strcmp(left.name, right.name) == 0;
}

bool isGroupPowerOperation(const CoreGroupOperationRecord& operation) {
    return operation.request.version == GROUP_POWER_PROTOCOL_VERSION &&
        operation.request.action == GroupProtocolAction::SET_POWER;
}

bool isGroupMemberResultTerminal(GroupProtocolResult result) {
    return result == GroupProtocolResult::EXECUTED ||
        result == GroupProtocolResult::FAILED ||
        result == GroupProtocolResult::OFFLINE ||
        result == GroupProtocolResult::UNSUPPORTED ||
        result == GroupProtocolResult::UNCERTAIN ||
        result == GroupProtocolResult::SUPERSEDED;
}

void updateGroupPowerAggregate(CoreGroupOperationRecord& operation) {
    if (!isGroupPowerOperation(operation) || operation.expectedGroupCount == 0 ||
        operation.receivedGroupCount != operation.expectedGroupCount) return;
    bool allTerminal = true;
    bool anyExecuted = false;
    bool anyFailure = false;
    bool anyUncertain = false;
    for (uint8_t index = 0; index < operation.expectedGroupCount; ++index) {
        const CoreGroupMemberResult& member = operation.members[index];
        if (!member.valid || !isGroupMemberResultTerminal(member.result)) {
            allTerminal = false;
            continue;
        }
        if (member.result == GroupProtocolResult::EXECUTED) anyExecuted = true;
        if (member.result == GroupProtocolResult::FAILED ||
            member.result == GroupProtocolResult::OFFLINE ||
            member.result == GroupProtocolResult::UNSUPPORTED) anyFailure = true;
        if (member.result == GroupProtocolResult::UNCERTAIN ||
            member.result == GroupProtocolResult::SUPERSEDED) anyUncertain = true;
    }
    if (!allTerminal) return;
    if (anyUncertain) operation.result = GroupProtocolResult::UNCERTAIN;
    else if (anyFailure && anyExecuted) operation.result = GroupProtocolResult::PARTIAL;
    else if (anyFailure) operation.result = GroupProtocolResult::FAILED;
    else operation.result = GroupProtocolResult::COMPLETED;
    operation.state = operation.result == GroupProtocolResult::UNCERTAIN
        ? CoreGroupOperationState::UNCERTAIN
        : (operation.result == GroupProtocolResult::FAILED
            ? CoreGroupOperationState::FAILED : CoreGroupOperationState::COMPLETED);
}

bool appendJsonString(size_t& used, const char* value) {
    if (value == nullptr || used >= sizeof(responseBuffer) - 1) return false;
    responseBuffer[used++] = '"';
    for (const unsigned char* cursor =
             reinterpret_cast<const unsigned char*>(value);
         *cursor != 0; ++cursor) {
        if (*cursor == '"' || *cursor == '\\') {
            if (used + 2 >= sizeof(responseBuffer)) return false;
            responseBuffer[used++] = '\\';
            responseBuffer[used++] = static_cast<char>(*cursor);
        } else if (*cursor < 0x20) {
            if (used + 6 >= sizeof(responseBuffer)) return false;
            static constexpr char HEX[] = "0123456789abcdef";
            responseBuffer[used++] = '\\';
            responseBuffer[used++] = 'u';
            responseBuffer[used++] = '0';
            responseBuffer[used++] = '0';
            responseBuffer[used++] = HEX[*cursor >> 4];
            responseBuffer[used++] = HEX[*cursor & 0x0f];
        } else {
            if (used + 1 >= sizeof(responseBuffer)) return false;
            responseBuffer[used++] = static_cast<char>(*cursor);
        }
    }
    if (used + 1 >= sizeof(responseBuffer)) return false;
    responseBuffer[used++] = '"';
    responseBuffer[used] = '\0';
    return true;
}

bool appendGroupRecordJson(size_t& used, const GroupRegistry& snapshot,
                           uint8_t groupIndex, uint32_t mainId) {
    if (groupIndex >= snapshot.count || used >= sizeof(responseBuffer)) return false;
    const LampGroup& group = snapshot.groups[groupIndex];
    int length = snprintf(responseBuffer + used, sizeof(responseBuffer) - used,
        "%s{\"id\":%lu,\"main_id\":%lu,\"name\":",
        groupIndex == 0 ? "" : ",", static_cast<unsigned long>(group.id),
        static_cast<unsigned long>(mainId));
    if (length <= 0 || static_cast<size_t>(length) >= sizeof(responseBuffer) - used)
        return false;
    used += static_cast<size_t>(length);
    if (!appendJsonString(used, group.name)) return false;
    length = snprintf(responseBuffer + used, sizeof(responseBuffer) - used,
        ",\"revision\":%lu,\"member_ids\":[",
        static_cast<unsigned long>(group.revision));
    if (length <= 0 || static_cast<size_t>(length) >= sizeof(responseBuffer) - used)
        return false;
    used += static_cast<size_t>(length);
    for (uint8_t member = 0; member < group.lampCount; ++member) {
        const uint32_t lampV7Id = makeCoreModuleId(mainId, group.lampIds[member]);
        length = snprintf(responseBuffer + used, sizeof(responseBuffer) - used,
            "%s%lu", member == 0 ? "" : ",",
            static_cast<unsigned long>(lampV7Id));
        if (length <= 0 || static_cast<size_t>(length) >= sizeof(responseBuffer) - used)
            return false;
        used += static_cast<size_t>(length);
    }
    constexpr char END[] = "]}";
    if (sizeof(END) - 1 >= sizeof(responseBuffer) - used) return false;
    memcpy(responseBuffer + used, END, sizeof(END) - 1);
    used += sizeof(END) - 1;
    responseBuffer[used] = '\0';
    return true;
}

const char* groupMemberStateName(GroupProtocolResult result) {
    switch (result) {
        case GroupProtocolResult::IN_PROGRESS: return "in_progress";
        case GroupProtocolResult::EXECUTED: return "executed";
        case GroupProtocolResult::FAILED: return "failed";
        case GroupProtocolResult::OFFLINE: return "offline";
        case GroupProtocolResult::UNSUPPORTED: return "unsupported";
        case GroupProtocolResult::UNCERTAIN:
        case GroupProtocolResult::SUPERSEDED: return "uncertain";
        default: return "unknown";
    }
}

bool appendGroupOperationJson(const CoreGroupOperationRecord& operation) {
    const int length = snprintf(responseBuffer, sizeof(responseBuffer),
        "{\"operation\":{\"id\":%lu,\"state\":\"%s\",\"result\":\"%s\",\"group_id\":%lu,\"revision\":%lu",
        static_cast<unsigned long>(operation.id),
        groupOperationStateName(operation.state), groupResultName(operation.result),
        static_cast<unsigned long>(operation.request.groupId),
        static_cast<unsigned long>(operation.resultRevision));
    if (length <= 0 || static_cast<size_t>(length) >= sizeof(responseBuffer))
        return false;
    size_t used = static_cast<size_t>(length);
    if (operation.request.version == GROUP_POWER_PROTOCOL_VERSION &&
        operation.request.action == GroupProtocolAction::SET_POWER) {
        constexpr char MEMBERS[] = ",\"members\":[";
        if (sizeof(MEMBERS) - 1 >= sizeof(responseBuffer) - used) return false;
        memcpy(responseBuffer + used, MEMBERS, sizeof(MEMBERS) - 1);
        used += sizeof(MEMBERS) - 1;
        bool first = true;
        for (uint8_t index = 0; index < MAX_GROUP_LAMPS; ++index) {
            const CoreGroupMemberResult& member = operation.members[index];
            if (!member.valid) continue;
            const uint32_t lampId = makeCoreModuleId(
                operation.request.mainCoreId, member.localId);
            const int memberLength = snprintf(responseBuffer + used,
                sizeof(responseBuffer) - used,
                "%s{\"lamp_id\":%lu,\"child_id\":%lu,\"state\":\"%s\",\"result\":\"%s\"}",
                first ? "" : ",", static_cast<unsigned long>(lampId),
                static_cast<unsigned long>(member.childCommandId),
                groupMemberStateName(member.result), groupResultName(member.result));
            if (memberLength <= 0 ||
                static_cast<size_t>(memberLength) >= sizeof(responseBuffer) - used)
                return false;
            used += static_cast<size_t>(memberLength);
            first = false;
        }
        constexpr char CLOSE_MEMBERS[] = "]";
        if (sizeof(CLOSE_MEMBERS) - 1 >= sizeof(responseBuffer) - used) return false;
        memcpy(responseBuffer + used, CLOSE_MEMBERS, sizeof(CLOSE_MEMBERS) - 1);
        used += sizeof(CLOSE_MEMBERS) - 1;
    }
    constexpr char CLOSE[] = "}}";
    if (sizeof(CLOSE) - 1 >= sizeof(responseBuffer) - used) return false;
    memcpy(responseBuffer + used, CLOSE, sizeof(CLOSE) - 1);
    used += sizeof(CLOSE) - 1;
    responseBuffer[used] = '\0';
    return true;
}

bool parsePairOperationBody(const char* body, uint32_t& sessionId,
                            uint32_t& operationId,
                            char* hardwareId, size_t hardwareCapacity,
                            char* name, size_t nameCapacity) {
    const char* cursor = body;
    const char* end = body + strlen(body);
    bool haveSession = false;
    bool haveOperation = false;
    bool haveHardware = false;
    bool haveName = false;
    bool closed = false;
    skipJsonWhitespace(cursor, end);
    if (cursor == end || *cursor++ != '{') return false;
    while (cursor != end) {
        skipJsonWhitespace(cursor, end);
        if (cursor != end && *cursor == '}') {
            ++cursor;
            closed = true;
            break;
        }
        char key[32] = {};
        if (!parseJsonString(cursor, end, key, sizeof(key))) return false;
        skipJsonWhitespace(cursor, end);
        if (cursor == end || *cursor++ != ':') return false;
        skipJsonWhitespace(cursor, end);
        if (strcmp(key, "session_id") == 0 && !haveSession) {
            if (!parseJsonUnsigned(cursor, end, sessionId)) return false;
            haveSession = true;
        } else if (strcmp(key, "operation_id") == 0 && !haveOperation) {
            if (!parseJsonUnsigned(cursor, end, operationId)) return false;
            haveOperation = true;
        } else if (strcmp(key, "hardware_id") == 0 && !haveHardware) {
            if (!parseJsonString(cursor, end, hardwareId, hardwareCapacity)) return false;
            haveHardware = true;
        } else if (strcmp(key, "name") == 0 && !haveName) {
            if (!parseJsonString(cursor, end, name, nameCapacity)) return false;
            haveName = true;
        } else {
            return false;
        }
        skipJsonWhitespace(cursor, end);
        if (cursor != end && *cursor == ',') {
            ++cursor;
            skipJsonWhitespace(cursor, end);
            if (cursor == end || *cursor == '}') return false;
            continue;
        }
        if (cursor != end && *cursor == '}') {
            ++cursor;
            closed = true;
            break;
        }
        return false;
    }
    skipJsonWhitespace(cursor, end);
    return cursor == end && closed && haveSession && sessionId != 0 && haveOperation &&
           operationId != 0 && haveHardware && haveName;
}

uint32_t makePairingId() {
    uint32_t id = esp_random();
    if (id == 0) id = 1;
    return id;
}

bool findPairingMain(const DeviceRegistry& registry, const Device*& main,
                     uint16_t& matches) {
    main = nullptr;
    matches = 0;
    for (uint8_t index = 0; index < registry.count && index < MAX_DEVICES; ++index) {
        if (registry.devices[index].role == DeviceRole::MAIN) {
            if (registry.devices[index].parentId != CORE_LOGICAL_ID) {
                ++matches;
                continue;
            }
            main = &registry.devices[index];
            ++matches;
        }
    }
    return matches == 1 && main != nullptr && main->localId != 0;
}

bool authorized(httpd_req_t* request, const char* token) {
    const size_t headerLength = httpd_req_get_hdr_value_len(request, "Authorization");
    if (headerLength == 0 || headerLength >= MAX_AUTHORIZATION_BYTES) return false;

    char header[MAX_AUTHORIZATION_BYTES] = {};
    if (httpd_req_get_hdr_value_str(request, "Authorization", header,
                                    sizeof(header)) != ESP_OK) {
        return false;
    }
    return coreHttpHasValidBearer(header, headerLength, token);
}

bool isJsonContentType(httpd_req_t* request) {
    const size_t headerLength = httpd_req_get_hdr_value_len(request, "Content-Type");
    if (headerLength == 0 || headerLength >= 64) return false;
    char contentType[64] = {};
    if (httpd_req_get_hdr_value_str(request, "Content-Type", contentType,
                                    sizeof(contentType)) != ESP_OK) {
        return false;
    }
    constexpr char JSON_TYPE[] = "application/json";
    return strncmp(contentType, JSON_TYPE, sizeof(JSON_TYPE) - 1) == 0 &&
        (contentType[sizeof(JSON_TYPE) - 1] == '\0' ||
         contentType[sizeof(JSON_TYPE) - 1] == ';');
}

bool receivePowerState(httpd_req_t* request, bool& power) {
    if (request->content_len == 0 || request->content_len > MAX_COMMAND_BODY_BYTES ||
        !isJsonContentType(request)) {
        return false;
    }
    char body[MAX_COMMAND_BODY_BYTES + 1] = {};
    size_t received = 0;
    while (received < request->content_len) {
        const int count = httpd_req_recv(request, body + received,
            request->content_len - received);
        if (count <= 0) return false;
        received += static_cast<size_t>(count);
    }
    body[received] = '\0';

    const char* cursor = body;
    const char* end = body + received;
    const auto skipWhitespace = [&cursor, end]() {
        while (cursor != end && (*cursor == ' ' || *cursor == '\t' ||
               *cursor == '\r' || *cursor == '\n')) ++cursor;
    };
    skipWhitespace();
    if (cursor == end || *cursor++ != '{') return false;
    skipWhitespace();
    constexpr char STATE_KEY[] = "\"state\"";
    if (static_cast<size_t>(end - cursor) < sizeof(STATE_KEY) - 1 ||
        memcmp(cursor, STATE_KEY, sizeof(STATE_KEY) - 1) != 0) return false;
    cursor += sizeof(STATE_KEY) - 1;
    skipWhitespace();
    if (cursor == end || *cursor++ != ':') return false;
    skipWhitespace();
    if (cursor == end || *cursor++ != '"') return false;
    const char* valueStart = cursor;
    while (cursor != end && *cursor != '"') ++cursor;
    if (cursor == end) return false;
    const size_t valueLength = static_cast<size_t>(cursor - valueStart);
    ++cursor;
    skipWhitespace();
    if (cursor == end || *cursor++ != '}') return false;
    skipWhitespace();
    if (cursor != end) return false;
    if (valueLength == 2 && memcmp(valueStart, "on", 2) == 0) {
        power = true;
        return true;
    }
    if (valueLength == 3 && memcmp(valueStart, "off", 3) == 0) {
        power = false;
        return true;
    }
    return false;
}

bool receiveBrightnessPercent(httpd_req_t* request, int32_t& percent) {
    if (request->content_len == 0 || request->content_len > MAX_COMMAND_BODY_BYTES ||
        !isJsonContentType(request)) {
        return false;
    }
    char body[MAX_COMMAND_BODY_BYTES + 1] = {};
    size_t received = 0;
    while (received < request->content_len) {
        const int count = httpd_req_recv(request, body + received,
            request->content_len - received);
        if (count <= 0) return false;
        received += static_cast<size_t>(count);
    }
    return parseCoreBrightnessRequestBody(body, received, percent);
}

esp_err_t sendJson(httpd_req_t* request, uint16_t status,
                   const char* body, size_t length, const char* allow = nullptr,
                   bool unauthorizedResponse = false) {
    if (status != 200 && httpd_resp_set_status(request, statusText(status)) != ESP_OK)
        return ESP_FAIL;
    if (httpd_resp_set_type(request, "application/json; charset=utf-8") != ESP_OK ||
        httpd_resp_set_hdr(request, "Cache-Control", "no-store") != ESP_OK) {
        return ESP_FAIL;
    }
    if (allow != nullptr && httpd_resp_set_hdr(request, "Allow", allow) != ESP_OK)
        return ESP_FAIL;
    if (unauthorizedResponse &&
        httpd_resp_set_hdr(request, "WWW-Authenticate", "Bearer") != ESP_OK)
        return ESP_FAIL;
    return httpd_resp_send(request, body, static_cast<ssize_t>(length));
}

esp_err_t sendError(httpd_req_t* request, uint16_t status,
                    const char* code, const char* message,
                    const char* allow = nullptr) {
    const int length = snprintf(responseBuffer, sizeof(responseBuffer),
        "{\"error\":{\"code\":\"%s\",\"message\":\"%s\"}}", code, message);
    if (length <= 0 || static_cast<size_t>(length) >= sizeof(responseBuffer))
        return ESP_FAIL;
    return sendJson(request, status, responseBuffer, static_cast<size_t>(length),
                    allow, status == 401);
}

const char* commandState(const PendingMessage& pending) {
    if (pending.timedOut) return "expired";
    if (!pending.completed) return pending.accepted ? "accepted" : "sent";
    return pending.message.executionStatus == ExecutionStatus::EXECUTED
        ? "executed" : "failed";
}

const char* commandError(const PendingMessage& pending) {
    if (pending.timedOut) return "execution_unknown";
    if (!pending.completed || pending.message.executionStatus != ExecutionStatus::FAILED)
        return nullptr;
    const int32_t error = pending.message.value2;
    if (error >= static_cast<int32_t>(CoreCommandError::UNKNOWN_DESTINATION) &&
        error <= static_cast<int32_t>(CoreCommandError::COMMAND_IN_PROGRESS)) {
        return coreCommandErrorName(static_cast<CoreCommandError>(error));
    }
    return "execution_failed";
}

struct CommandApiContext {
    CoreCommandService& commands;
    MessageTracker& tracker;
    const char* apiToken;
    SemaphoreHandle_t* registryMutex;
};

esp_err_t handleCommandRequest(CommandApiContext& api, httpd_req_t* request,
                               const CommandRoute& route) {
    if (route.kind == CommandRouteKind::INVALID)
        return sendError(request, 400, "bad_request", "The command path or ID is invalid.");
    if (!coreHttpTokenConfigured(api.apiToken))
        return sendError(request, 503, "command_api_disabled",
                         "Configure a 32 to 128 character API token to enable commands.");
    if (!authorized(request, api.apiToken))
        return sendError(request, 401, "unauthorized", "A valid Bearer token is required.");

    if (route.kind == CommandRouteKind::SUBMIT_POWER ||
        route.kind == CommandRouteKind::SUBMIT_BRIGHTNESS) {
        const bool isBrightness =
            route.kind == CommandRouteKind::SUBMIT_BRIGHTNESS;
        if (request->method != HTTP_POST)
            return sendError(request, 405, "method_not_allowed",
                             isBrightness
                                 ? "Use POST to submit a brightness command."
                                 : "Use POST to submit a power command.", "POST");
        if (request->content_len > MAX_COMMAND_BODY_BYTES)
            return sendError(request, 413, "payload_too_large",
                             "The command body must not exceed 64 bytes.");
        if (!isJsonContentType(request))
            return sendError(request, 415, "unsupported_media_type",
                             "Content-Type must be application/json.");
        bool power = false;
        int32_t percent = 0;
        const bool validBody = isBrightness
            ? receiveBrightnessPercent(request, percent)
            : receivePowerState(request, power);
        if (!validBody)
            return sendError(request, 400, "bad_request",
                isBrightness
                    ? "Expected a JSON body with integer percent from 0 to 100."
                    : "Expected a JSON body with state set to on or off.");

        if (api.registryMutex == nullptr || *api.registryMutex == nullptr ||
            xSemaphoreTake(*api.registryMutex, pdMS_TO_TICKS(250)) != pdTRUE) {
            return sendError(request, 503, "core_busy", "The CORE is busy; retry shortly.");
        }
        uint32_t id = 0;
        const CoreCommandError result = isBrightness
            ? api.commands.submitBrightness(route.id, percent, id)
            : api.commands.submitPower(route.id, power, id);
        (void)xSemaphoreGive(*api.registryMutex);
        if (result != CoreCommandError::NONE) {
            switch (result) {
                case CoreCommandError::UNKNOWN_DESTINATION:
                    return sendError(request, 404, "unknown_destination",
                                     "The requested lamp was not found.");
                case CoreCommandError::OFFLINE:
                    return sendError(request, 409, "offline",
                                     "The lamp or its MAIN module is offline.");
                case CoreCommandError::INVALID_ROUTE:
                    return sendError(request, 409, "invalid_route",
                                     "The registered command route is inconsistent.");
                case CoreCommandError::TRANSPORT_FAILURE:
                    return sendError(request, 503, "transport_failure",
                                     "The CORE command transport is unavailable.");
                case CoreCommandError::TRACKER_FULL:
                    return sendError(request, 503, "tracker_full",
                                     "No command tracking slot is available.");
                case CoreCommandError::DUPLICATE_ID:
                    return sendError(request, 503, "id_collision",
                                     "The generated command ID collided; retry shortly.");
                case CoreCommandError::REMOVAL_IN_PROGRESS:
                    return sendError(request, 409, "removal_in_progress",
                                     "The lamp is being removed.");
                case CoreCommandError::UNSUPPORTED_CAPABILITY:
                    return sendError(request, 409, "unsupported_capability",
                                     "The lamp does not support brightness control.");
                case CoreCommandError::COMMAND_IN_PROGRESS:
                    return sendError(request, 409, "command_in_progress",
                                     "Another output command is still awaiting a final result.");
                case CoreCommandError::INVALID_COMMAND:
                    return sendError(request, 400, "bad_request",
                                     "The command value is invalid.");
                default:
                    return sendError(request, 500, "command_rejected",
                                     "The CORE rejected the command.");
            }
        }

        const int length = snprintf(responseBuffer, sizeof(responseBuffer),
            "{\"command\":{\"id\":%lu,\"state\":\"sent\"}}",
            static_cast<unsigned long>(id));
        if (length <= 0 || static_cast<size_t>(length) >= sizeof(responseBuffer))
            return sendError(request, 500, "internal_error", "The response could not be serialized.");
        char location[48] = {};
        const int locationLength = snprintf(location, sizeof(location),
            "/api/v1/commands/%lu", static_cast<unsigned long>(id));
        if (locationLength <= 0 || static_cast<size_t>(locationLength) >= sizeof(location) ||
            httpd_resp_set_status(request, statusText(202)) != ESP_OK ||
            httpd_resp_set_hdr(request, "Location", location) != ESP_OK) {
            return ESP_FAIL;
        }
        return sendJson(request, 202, responseBuffer, static_cast<size_t>(length));
    }

    if (route.kind == CommandRouteKind::STATUS) {
        if (request->method != HTTP_GET)
            return sendError(request, 405, "method_not_allowed",
                             "Use GET to read a command status.", "GET");
        if (api.registryMutex == nullptr || *api.registryMutex == nullptr ||
            xSemaphoreTake(*api.registryMutex, pdMS_TO_TICKS(250)) != pdTRUE) {
            return sendError(request, 503, "core_busy", "The CORE is busy; retry shortly.");
        }
        PendingMessage snapshot = {};
        const PendingMessage* pending = findPendingMessage(api.tracker, route.id);
        const bool found = pending != nullptr;
        if (found) snapshot = *pending;
        (void)xSemaphoreGive(*api.registryMutex);
        if (!found)
            return sendError(request, 404, "command_not_found",
                             "The command is unknown or has been evicted from the in-memory tracker.");

        const char* state = commandState(snapshot);
        const char* error = commandError(snapshot);
        const int length = error == nullptr
            ? snprintf(responseBuffer, sizeof(responseBuffer),
                "{\"command\":{\"id\":%lu,\"state\":\"%s\",\"retries\":%u}}",
                static_cast<unsigned long>(route.id), state,
                static_cast<unsigned>(snapshot.retryCount))
            : snprintf(responseBuffer, sizeof(responseBuffer),
                "{\"command\":{\"id\":%lu,\"state\":\"%s\",\"retries\":%u,\"error\":\"%s\"}}",
                static_cast<unsigned long>(route.id), state,
                static_cast<unsigned>(snapshot.retryCount), error);
        if (length <= 0 || static_cast<size_t>(length) >= sizeof(responseBuffer))
            return sendError(request, 500, "internal_error", "The response could not be serialized.");
        return sendJson(request, 200, responseBuffer, static_cast<size_t>(length));
    }

    return sendError(request, 404, "not_found", "The requested resource was not found.");
}
}  // namespace

CoreHttpApi::CoreHttpApi(DeviceRegistry& coreRegistry,
                         WiFiTransport& wifiTransport,
                         CoreUartTransport& uartTransport,
                         SemaphoreHandle_t& coreRegistryMutex,
                         CoreCommandService& commandService,
                         MessageTracker& messageTracker,
                         const char* commandApiToken)
    : registry(coreRegistry), wifi(wifiTransport), uart(uartTransport),
      commands(commandService), tracker(messageTracker), apiToken(commandApiToken),
      registryMutex(&coreRegistryMutex), removalOperationId(0),
      removalTargetId(0), removalMainLocalId(0), removalMainCoreId(0),
      removalLampLocalId(0), removalLastQueryAt(0),
      removalResult(CorePairingResultCode::NONE), groupStateMutex(nullptr),
      activeGroupOperationId(0), nextGroupOperationSlot(0), started(false),
      server(nullptr) {
    memset(groupOperations, 0, sizeof(groupOperations));
    groupReadOperation = {};
}

esp_err_t CoreHttpApi::handleGroupRequest(httpd_req_t* request) {
    const GroupRoute route = parseGroupRoute(request->uri);
    if (route.kind == GroupRouteKind::INVALID)
        return sendError(request, 400, "bad_request", "The group path or ID is invalid.");
    if (!coreHttpTokenConfigured(apiToken))
        return sendError(request, 503, "group_api_disabled",
                         "Configure a valid API token to enable group configuration.");
    if (!authorized(request, apiToken))
        return sendError(request, 401, "unauthorized", "A valid Bearer token is required.");
    if (groupStateMutex == nullptr)
        return sendError(request, 503, "group_api_unavailable",
                         "The group operation tracker is unavailable.");

    if (route.kind == GroupRouteKind::OPERATION) {
        if (request->method != HTTP_GET)
            return sendError(request, 405, "method_not_allowed",
                             "Use GET to inspect a group operation.", "GET");
        if (xSemaphoreTake(groupStateMutex, pdMS_TO_TICKS(250)) != pdTRUE)
            return sendError(request, 503, "core_busy", "The CORE is busy; retry shortly.");
        CoreGroupOperationRecord* found = nullptr;
        for (uint8_t index = 0; index < CORE_GROUP_OPERATION_SLOTS; ++index) {
            if (groupOperations[index].id == route.id &&
                groupOperations[index].state != CoreGroupOperationState::EMPTY) {
                found = &groupOperations[index];
                break;
            }
        }
        CoreGroupOperationRecord snapshot = {};
        bool queryMainStatus = false;
        if (found != nullptr) {
            const uint32_t now = millis();
            if (isGroupPowerOperation(*found) &&
                found->state == CoreGroupOperationState::UNCERTAIN &&
                (activeGroupOperationId == 0 || activeGroupOperationId == route.id) &&
                now - found->lastQueryAt >= GROUP_POWER_STATUS_REQUERY_MS) {
                found->lastQueryAt = now;
                activeGroupOperationId = route.id;
                queryMainStatus = true;
            }
            snapshot = *found;
        }
        (void)xSemaphoreGive(groupStateMutex);
        if (found == nullptr)
            return sendError(request, 404, "group_operation_not_found",
                "The operation is unknown or has been evicted from the CORE's in-memory tracker.");
        if (queryMainStatus) {
            CoreLinkPacket query = {};
            query.sequence = static_cast<uint16_t>(snapshot.id);
            query.type = CoreLinkMessageType::GROUPS;
            query.group = snapshot.request;
            query.group.action = GroupProtocolAction::QUERY_OPERATION;
            if (!uart.send(query) &&
                xSemaphoreTake(groupStateMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
                if (activeGroupOperationId == snapshot.id)
                    activeGroupOperationId = 0;
                (void)xSemaphoreGive(groupStateMutex);
            }
        }
        if (!appendGroupOperationJson(snapshot))
            return sendError(request, 500, "internal_error", "The response could not be serialized.");
        return sendJson(request, 200, responseBuffer, strlen(responseBuffer));
    }

    if (route.kind != GroupRouteKind::COLLECTION && route.kind != GroupRouteKind::ITEM &&
        route.kind != GroupRouteKind::POWER)
        return sendError(request, 404, "not_found", "The requested group resource was not found.");

    if (apiToken == nullptr || !coreHttpTokenConfigured(apiToken))
        return sendError(request, 503, "group_api_disabled",
                         "Configure a valid API token to enable group configuration.");
    if (route.kind == GroupRouteKind::POWER && request->method != HTTP_POST)
        return sendError(request, 405, "method_not_allowed",
                         "Use POST to submit a group power operation.", "POST");
    if ((route.kind == GroupRouteKind::COLLECTION && request->method == HTTP_GET) ||
        (route.kind == GroupRouteKind::ITEM && request->method == HTTP_GET)) {
        // Read requests are proxied to MAIN and only answered after the complete
        // metadata/member stream has been validated.
    } else if ((route.kind == GroupRouteKind::COLLECTION && request->method != HTTP_POST) ||
               (route.kind == GroupRouteKind::ITEM &&
                request->method != HTTP_PATCH && request->method != HTTP_DELETE)) {
        return sendError(request, 405, "method_not_allowed",
            route.kind == GroupRouteKind::COLLECTION
                ? "Use GET or POST for the group collection."
                : "Use GET, PATCH, DELETE, or the group power command.",
            route.kind == GroupRouteKind::COLLECTION ? "GET, POST" : "GET, PATCH, DELETE, POST");
    }

    if (route.kind != GroupRouteKind::COLLECTION && route.id == 0)
        return sendError(request, 400, "bad_request", "The group ID must be nonzero.");

    if (registryMutex == nullptr || *registryMutex == nullptr ||
        xSemaphoreTake(*registryMutex, pdMS_TO_TICKS(250)) != pdTRUE)
        return sendError(request, 503, "core_busy", "The CORE is busy; retry shortly.");
    const Device* main = findDeviceById(registry, route.mainId);
    uint8_t mainCount = 0;
    for (uint8_t index = 0; index < registry.count; ++index)
        if (registry.devices[index].role == DeviceRole::MAIN) ++mainCount;
    if (main == nullptr || main->role != DeviceRole::MAIN || mainCount != 1 ||
        main->localId == 0 ||
        makeCoreModuleId(CORE_LOGICAL_ID, main->localId) != main->id) {
        (void)xSemaphoreGive(*registryMutex);
        return sendError(request, 404, "unknown_main", "The requested MAIN was not found.");
    }
    if (main->status != DeviceStatus::ONLINE) {
        (void)xSemaphoreGive(*registryMutex);
        return sendError(request, 409, "main_offline", "The MAIN is offline.");
    }
    if (!hasDeviceCapability(*main, DEVICE_CAP_GROUPS)) {
        (void)xSemaphoreGive(*registryMutex);
        return sendError(request, 409, "groups_unsupported",
                         "The MAIN does not announce group configuration support.");
    }
    const uint32_t mainLocalId = main->localId;
    const uint32_t mainCoreId = main->id;
    (void)xSemaphoreGive(*registryMutex);

    GroupProtocolPayload groupRequest = {};
    groupRequest.version = route.kind == GroupRouteKind::POWER
        ? GROUP_POWER_PROTOCOL_VERSION : GROUP_PROTOCOL_VERSION;
    groupRequest.mainLocalId = mainLocalId;
    groupRequest.mainCoreId = mainCoreId;
    const bool readRequest = request->method == HTTP_GET;
    GroupHttpBody body = {};
    uint32_t operationId = 0;

    if (readRequest) {
        groupRequest.action = route.kind == GroupRouteKind::COLLECTION
            ? GroupProtocolAction::LIST : GroupProtocolAction::GET;
        groupRequest.groupId = route.id;
        operationId = esp_random();
        if (operationId == 0) operationId = 1;
        groupRequest.operationId = operationId;
    } else {
        if (request->content_len > MAX_GROUP_BODY_BYTES)
            return sendError(request, 413, "payload_too_large",
                             "The group body must not exceed 192 bytes.");
        if (!isJsonContentType(request))
            return sendError(request, 415, "unsupported_media_type",
                             "Content-Type must be application/json.");
        char bodyText[MAX_GROUP_BODY_BYTES + 1] = {};
        if (!parsePairingBody(request, bodyText, sizeof(bodyText)) ||
            !parseGroupBody(bodyText, body))
            return sendError(request, 400, "bad_request",
                             "The group JSON body is invalid or contains unsupported fields.");
        operationId = body.operationId;
        groupRequest.operationId = operationId;
        if (route.kind == GroupRouteKind::POWER) {
            if (request->method != HTTP_POST || !body.havePower ||
                body.haveName || body.haveExpectedRevision || body.haveLampId ||
                body.haveMemberAction)
                return sendError(request, 400, "bad_request",
                    "Group power expects operation_id and power set to on or off.");
            groupRequest.action = GroupProtocolAction::SET_POWER;
            groupRequest.groupId = route.id;
            groupRequest.power = strcmp(body.power, "on") == 0 ? 1 : 0;
        } else if (route.kind == GroupRouteKind::COLLECTION) {
            if (request->method != HTTP_POST || !body.haveName ||
                body.haveExpectedRevision || body.haveLampId || body.haveMemberAction)
                return sendError(request, 400, "bad_request",
                    "Create expects only operation_id and a 1 to 31 character group name.");
            LampGroup nameCheck = {};
            if (!setGroupName(nameCheck, body.name))
                return sendError(request, 400, "invalid_group_name",
                                 "The group name is invalid or too long.");
            groupRequest.action = GroupProtocolAction::CREATE;
            // A stable group ID derived from the caller's idempotency key makes
            // an identical retry safe even if the HTTP response was lost.
            groupRequest.groupId = operationId;
            strncpy(groupRequest.name, body.name, sizeof(groupRequest.name) - 1);
        } else {
            if (!body.haveExpectedRevision)
                return sendError(request, 400, "bad_request",
                                 "A nonzero expected_revision is required for changes.");
            groupRequest.groupId = route.id;
            groupRequest.expectedRevision = body.expectedRevision;
            if (request->method == HTTP_DELETE) {
                if (body.haveName || body.haveLampId || body.haveMemberAction)
                    return sendError(request, 400, "bad_request",
                                     "Delete expects operation_id and expected_revision only.");
                groupRequest.action = GroupProtocolAction::DELETE_GROUP;
            } else if (body.haveName && !body.haveLampId && !body.haveMemberAction) {
                LampGroup nameCheck = {};
                if (!setGroupName(nameCheck, body.name))
                    return sendError(request, 400, "invalid_group_name",
                                     "The group name is invalid or too long.");
                groupRequest.action = GroupProtocolAction::RENAME;
                strncpy(groupRequest.name, body.name, sizeof(groupRequest.name) - 1);
            } else if (!body.haveName && body.haveLampId && body.haveMemberAction &&
                       (strcmp(body.memberAction, "add") == 0 ||
                        strcmp(body.memberAction, "remove") == 0)) {
                if (xSemaphoreTake(*registryMutex, pdMS_TO_TICKS(250)) != pdTRUE)
                    return sendError(request, 503, "core_busy", "The CORE is busy; retry shortly.");
                const Device* lamp = findDeviceById(registry, body.lampId);
                const bool validLamp = lamp != nullptr &&
                    lamp->role == DeviceRole::LAMP && lamp->parentId == mainCoreId &&
                    lamp->localId != 0 &&
                    makeCoreModuleId(mainCoreId, lamp->localId) == lamp->id;
                if (validLamp) groupRequest.memberLocalId = lamp->localId;
                (void)xSemaphoreGive(*registryMutex);
                if (!validLamp)
                    return sendError(request, 404, "unknown_lamp",
                        "The lamp is not registered under this MAIN.");
                groupRequest.action = strcmp(body.memberAction, "add") == 0
                    ? GroupProtocolAction::ADD_MEMBER
                    : GroupProtocolAction::REMOVE_MEMBER;
            } else {
                return sendError(request, 400, "bad_request",
                    "Patch expects either name or member_action with lamp_id.");
            }
        }
    }

    int slot = -1;
    if (xSemaphoreTake(groupStateMutex, pdMS_TO_TICKS(250)) != pdTRUE)
        return sendError(request, 503, "core_busy", "The CORE is busy; retry shortly.");
    if (!readRequest) {
        for (uint8_t index = 0; index < CORE_GROUP_OPERATION_SLOTS; ++index) {
            if (groupOperations[index].id == operationId &&
                groupOperations[index].state != CoreGroupOperationState::EMPTY) {
                slot = index;
                if (!sameGroupRequest(groupOperations[index].request, groupRequest)) {
                    (void)xSemaphoreGive(groupStateMutex);
                    return sendError(request, 409, "operation_id_conflict",
                        "This operation_id is already associated with another group request.");
                }
                break;
            }
        }
    }
    if (slot >= 0) {
        const CoreGroupOperationRecord saved = groupOperations[slot];
        (void)xSemaphoreGive(groupStateMutex);
        const uint16_t status = groupOperationTerminal(saved.state) ? 200 : 202;
        if (!appendGroupOperationJson(saved))
            return sendError(request, 500, "internal_error", "The response could not be serialized.");
        return sendJson(request, status, responseBuffer, strlen(responseBuffer));
    }
    if (activeGroupOperationId != 0) {
        (void)xSemaphoreGive(groupStateMutex);
        return sendError(request, 409, "group_operation_in_progress",
                         "Another group operation is still being resolved.");
    }
    if (readRequest) {
        if (groupReadOperation.state != CoreGroupOperationState::EMPTY) {
            (void)xSemaphoreGive(groupStateMutex);
            return sendError(request, 409, "group_operation_in_progress",
                "A group read is still being resolved; retry after it completes.");
        }
        groupReadOperation = {};
        groupReadOperation.id = operationId;
        groupReadOperation.request = groupRequest;
        groupReadOperation.startedAt = millis();
        groupReadOperation.lastQueryAt = groupReadOperation.startedAt;
        groupReadOperation.state = CoreGroupOperationState::SENT;
        groupReadOperation.result = GroupProtocolResult::NONE;
        if (groupRequest.action == GroupProtocolAction::GET) {
            groupReadOperation.expectedGroupCount = 1;
            groupReadOperation.listStarted = true;
        }
        activeGroupOperationId = operationId;
    } else {
        const uint32_t now = millis();
        for (uint8_t attempt = 0; attempt < CORE_GROUP_OPERATION_SLOTS; ++attempt) {
            const uint8_t candidate = static_cast<uint8_t>(
                (nextGroupOperationSlot + attempt) % CORE_GROUP_OPERATION_SLOTS);
            const CoreGroupOperationRecord& current = groupOperations[candidate];
            if (groupOperationReusable(current, now)) {
                slot = candidate;
                nextGroupOperationSlot = static_cast<uint8_t>(
                    (candidate + 1) % CORE_GROUP_OPERATION_SLOTS);
                break;
            }
        }
        if (slot < 0) {
            (void)xSemaphoreGive(groupStateMutex);
            return sendError(request, 409, "group_operation_history_full",
                "Recent group changes and command results are retained; wait for expiration before submitting another.");
        }
        groupOperations[slot] = {};
        groupOperations[slot].id = operationId;
        groupOperations[slot].request = groupRequest;
        groupOperations[slot].startedAt = millis();
        groupOperations[slot].lastQueryAt = groupOperations[slot].startedAt;
        groupOperations[slot].state = CoreGroupOperationState::SENT;
        groupOperations[slot].result = GroupProtocolResult::NONE;
        activeGroupOperationId = operationId;
    }
    (void)xSemaphoreGive(groupStateMutex);

    CoreLinkPacket outbound = {};
    outbound.sequence = static_cast<uint16_t>(operationId);
    outbound.type = CoreLinkMessageType::GROUPS;
    outbound.group = groupRequest;
    if (!uart.send(outbound)) {
        if (xSemaphoreTake(groupStateMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            if (readRequest && groupReadOperation.id == operationId) {
                groupReadOperation = {};
            } else if (!readRequest && slot >= 0 &&
                       groupOperations[slot].id == operationId) {
                groupOperations[slot].state = CoreGroupOperationState::UNCERTAIN;
                groupOperations[slot].result = GroupProtocolResult::UNCERTAIN;
                groupOperations[slot].retainedAt = millis();
            }
            if (activeGroupOperationId == operationId) activeGroupOperationId = 0;
            (void)xSemaphoreGive(groupStateMutex);
        }
        if (readRequest)
            return sendError(request, 503, "group_read_unavailable",
                "The read could not be sent to MAIN. Repeat the GET safely.");
        return sendError(request, 503, "group_transport_uncertain",
            "The MAIN result is unknown. Inspect the same operation_id before retrying.");
    }

    if (!readRequest) {
        const int length = snprintf(responseBuffer, sizeof(responseBuffer),
            "{\"operation\":{\"id\":%lu,\"state\":\"sent\"}}",
            static_cast<unsigned long>(operationId));
        if (length <= 0 || static_cast<size_t>(length) >= sizeof(responseBuffer))
            return sendError(request, 500, "internal_error", "The response could not be serialized.");
        char location[64] = {};
        const int locationLength = snprintf(location, sizeof(location),
            "/api/v1/group-operations/%lu", static_cast<unsigned long>(operationId));
        if (locationLength <= 0 || static_cast<size_t>(locationLength) >= sizeof(location) ||
            httpd_resp_set_status(request, statusText(202)) != ESP_OK ||
            httpd_resp_set_hdr(request, "Location", location) != ESP_OK)
            return ESP_FAIL;
        return sendJson(request, 202, responseBuffer, static_cast<size_t>(length));
    }

    const uint32_t waitStarted = millis();
    CoreGroupReadOperationRecord completed = {};
    bool isTerminal = false;
    while (millis() - waitStarted < GROUP_OPERATION_TIMEOUT_MS) {
        if (xSemaphoreTake(groupStateMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            if (groupReadOperation.id == operationId &&
                groupReadOperation.state != CoreGroupOperationState::EMPTY) {
                completed = groupReadOperation;
                isTerminal = groupOperationTerminal(completed.state);
                if (isTerminal) groupReadOperation = {};
            }
            (void)xSemaphoreGive(groupStateMutex);
        }
        if (isTerminal) break;
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    if (!isTerminal) {
        if (xSemaphoreTake(groupStateMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            if (groupReadOperation.id == operationId &&
                groupReadOperation.state != CoreGroupOperationState::EMPTY) {
                completed = groupReadOperation;
                completed.state = CoreGroupOperationState::UNCERTAIN;
                completed.result = GroupProtocolResult::UNCERTAIN;
                groupReadOperation = {};
            }
            if (activeGroupOperationId == operationId) activeGroupOperationId = 0;
            (void)xSemaphoreGive(groupStateMutex);
        }
        return sendError(request, 503, "group_result_uncertain",
            "The MAIN did not return a complete group snapshot. Repeat this read request safely.");
    }
    if (completed.state == CoreGroupOperationState::FAILED ||
        completed.state == CoreGroupOperationState::UNCERTAIN) {
        switch (completed.result) {
            case GroupProtocolResult::NOT_FOUND:
                return sendError(request, 404, "group_not_found", "The group was not found on MAIN.");
            case GroupProtocolResult::REVISION_CONFLICT:
                return sendError(request, 409, "revision_conflict",
                    "The group changed elsewhere. Reload it before editing.");
            case GroupProtocolResult::STORAGE_UNAVAILABLE:
                return sendError(request, 503, "group_storage_unavailable",
                    "MAIN could not read its persisted group configuration.");
            case GroupProtocolResult::BUSY:
                return sendError(request, 409, "group_operation_in_progress",
                    "MAIN is processing another group configuration request.");
            default:
                return sendError(request, 503, "group_result_uncertain",
                    "MAIN did not confirm a complete result. Repeat this read request safely.");
        }
    }
    size_t used = 0;
    responseBuffer[0] = '\0';
    int length = 0;
    if (route.kind == GroupRouteKind::COLLECTION) {
        length = snprintf(responseBuffer, sizeof(responseBuffer), "{\"groups\":[");
        if (length <= 0 || static_cast<size_t>(length) >= sizeof(responseBuffer))
            return sendError(request, 500, "internal_error", "The response could not be serialized.");
        used = static_cast<size_t>(length);
        for (uint8_t index = 0; index < completed.snapshot.count; ++index) {
            if (!appendGroupRecordJson(used, completed.snapshot, index, mainCoreId))
                return sendError(request, 500, "internal_error", "The response could not be serialized.");
        }
        length = snprintf(responseBuffer + used, sizeof(responseBuffer) - used,
                          "],\"count\":%u}",
                          static_cast<unsigned>(completed.snapshot.count));
    } else {
        length = snprintf(responseBuffer, sizeof(responseBuffer), "{\"group\":");
        if (length <= 0 || static_cast<size_t>(length) >= sizeof(responseBuffer))
            return sendError(request, 500, "internal_error", "The response could not be serialized.");
        used = static_cast<size_t>(length);
        if (!appendGroupRecordJson(used, completed.snapshot, 0, mainCoreId))
            return sendError(request, 500, "internal_error", "The response could not be serialized.");
        length = snprintf(responseBuffer + used, sizeof(responseBuffer) - used, "}");
    }
    if (length <= 0 || static_cast<size_t>(length) >= sizeof(responseBuffer) - used)
        return sendError(request, 500, "internal_error", "The response could not be serialized.");
    return sendJson(request, 200, responseBuffer, used + static_cast<size_t>(length));
}

esp_err_t CoreHttpApi::handleRemovalRequest(httpd_req_t* request) {
    const RemovalRoute route = parseRemovalRoute(request->uri);
    if (route.kind == RemovalRouteKind::INVALID)
        return sendError(request, 400, "bad_request", "The removal path or ID is invalid.");
    if (!coreHttpTokenConfigured(apiToken))
        return sendError(request, 503, "removal_api_disabled",
                         "Configure a valid API token to enable device removal.");
    if (!authorized(request, apiToken))
        return sendError(request, 401, "unauthorized", "A valid Bearer token is required.");

    if (route.kind == RemovalRouteKind::CREATE) {
        if (request->method != HTTP_POST)
            return sendError(request, 405, "method_not_allowed",
                             "Use POST to request removal.", "POST");
        if (request->content_len > MAX_REMOVAL_BODY_BYTES)
            return sendError(request, 413, "payload_too_large",
                             "The removal request body is too large.");
        char body[MAX_REMOVAL_BODY_BYTES + 1] = {};
        uint32_t operationId = 0;
        if (!parsePairingBody(request, body, sizeof(body)) ||
            !parseRemovalBody(body, operationId))
            return sendError(request, 400, "bad_request",
                "Expected JSON containing a nonzero operation_id.");
        if (registryMutex == nullptr || *registryMutex == nullptr ||
            xSemaphoreTake(*registryMutex, pdMS_TO_TICKS(250)) != pdTRUE)
            return sendError(request, 503, "core_busy", "The CORE is busy; retry shortly.");

        const uint32_t now = millis();
        bool duplicate = removalOperationId == operationId;
        if (duplicate && removalTargetId != 0 && removalTargetId != route.id) {
            (void)xSemaphoreGive(*registryMutex);
            return sendError(request, 409, "operation_id_conflict",
                "This operation_id is already associated with another lamp.");
        }
        if (duplicate && removalResult == CorePairingResultCode::SUCCEEDED) {
            const uint32_t targetId = removalTargetId;
            (void)xSemaphoreGive(*registryMutex);
            const int length = snprintf(responseBuffer, sizeof(responseBuffer),
                "{\"operation\":{\"id\":%lu,\"device_id\":%lu,\"state\":\"succeeded\"}}",
                static_cast<unsigned long>(operationId),
                static_cast<unsigned long>(targetId));
            if (length <= 0 || static_cast<size_t>(length) >= sizeof(responseBuffer))
                return sendError(request, 500, "internal_error", "The response could not be serialized.");
            return sendJson(request, 200, responseBuffer, static_cast<size_t>(length));
        }
        if (duplicate && removalResult == CorePairingResultCode::FAILED) {
            const uint32_t targetId = removalTargetId;
            (void)xSemaphoreGive(*registryMutex);
            const int length = snprintf(responseBuffer, sizeof(responseBuffer),
                "{\"operation\":{\"id\":%lu,\"device_id\":%lu,\"state\":\"failed\"}}",
                static_cast<unsigned long>(operationId),
                static_cast<unsigned long>(targetId));
            if (length <= 0 || static_cast<size_t>(length) >= sizeof(responseBuffer))
                return sendError(request, 500, "internal_error", "The response could not be serialized.");
            return sendJson(request, 200, responseBuffer, static_cast<size_t>(length));
        }
        if (duplicate && removalResult == CorePairingResultCode::NOT_FOUND) {
            (void)xSemaphoreGive(*registryMutex);
            return sendJson(request, 200,
                "{\"operation\":{\"state\":\"not_found\"}}",
                sizeof("{\"operation\":{\"state\":\"not_found\"}}") - 1);
        }
        if (duplicate && removalTargetId == 0) {
            CoreLinkPacket query = {};
            query.type = CoreLinkMessageType::PAIRING;
            query.sequence = static_cast<uint16_t>(operationId);
            query.pairingAction = CoreLinkPairingAction::QUERY_REMOVAL;
            query.localId = removalMainLocalId;
            query.parentId = removalMainCoreId;
            query.operationId = operationId;
            removalLastQueryAt = now;
            (void)xSemaphoreGive(*registryMutex);
            if (!uart.send(query)) {
                return sendError(request, 503, "removal_query_uncertain",
                                 "The existing operation could not be queried.");
            }
            return sendJson(request, 202,
                "{\"operation\":{\"state\":\"uncertain\"}}",
                sizeof("{\"operation\":{\"state\":\"uncertain\"}}") - 1);
        }
        if (!duplicate && removalOperationId != 0 &&
            (removalResult == CorePairingResultCode::PROCESSING ||
             removalResult == CorePairingResultCode::UNCERTAIN)) {
            (void)xSemaphoreGive(*registryMutex);
            return sendError(request, 409, "removal_in_progress",
                             "Another lamp removal is still unresolved.");
        }

        const Device* target = findDeviceById(registry, route.id);
        const Device* main = nullptr;
        uint16_t mainCount = 0;
        (void)findPairingMain(registry, main, mainCount);
        if (target == nullptr || target->role != DeviceRole::LAMP ||
            target->localId == 0 || target->parentId == 0 ||
            main == nullptr || mainCount != 1 ||
            target->parentId != main->id || main->localId == 0 ||
            makeCoreModuleId(main->id, target->localId) != target->id) {
            (void)xSemaphoreGive(*registryMutex);
            return sendError(request, 404, "unknown_destination",
                             "The requested lamp was not found.");
        }
        if (target->status != DeviceStatus::ONLINE ||
            main->status != DeviceStatus::ONLINE) {
            (void)xSemaphoreGive(*registryMutex);
            return sendError(request, 409, "offline",
                             "The lamp or its MAIN module is offline.");
        }
        if (!duplicate && !commands.canBeginRemoval(target->id)) {
            (void)xSemaphoreGive(*registryMutex);
            return sendError(request, 409, "command_unresolved",
                "The lamp has an OFF command or an output result that is still unresolved.");
        }
        if (duplicate && removalTargetId == 0) {
            removalTargetId = target->id;
            removalMainLocalId = main->localId;
            removalMainCoreId = main->id;
            removalLampLocalId = target->localId;
        } else if (!duplicate) {
            removalOperationId = operationId;
            removalTargetId = target->id;
            removalMainLocalId = main->localId;
            removalMainCoreId = main->id;
            removalLampLocalId = target->localId;
            removalResult = CorePairingResultCode::PROCESSING;
            removalLastQueryAt = 0;
        }

        CoreLinkPacket command = {};
        command.type = CoreLinkMessageType::PAIRING;
        command.sequence = static_cast<uint16_t>(operationId);
        command.pairingAction = CoreLinkPairingAction::REMOVE_DEVICE;
        command.localId = main->localId;
        command.parentId = main->id;
        command.operationId = operationId;
        command.deviceId = target->localId;
        command.coreId = target->id;
        (void)xSemaphoreGive(*registryMutex);

        if (!uart.send(command)) {
            if (registryMutex != nullptr && *registryMutex != nullptr &&
                xSemaphoreTake(*registryMutex, pdMS_TO_TICKS(250)) == pdTRUE) {
                if (removalOperationId == operationId)
                    removalResult = CorePairingResultCode::UNCERTAIN;
                (void)xSemaphoreGive(*registryMutex);
            }
            const int length = snprintf(responseBuffer, sizeof(responseBuffer),
                "{\"operation\":{\"id\":%lu,\"device_id\":%lu,\"state\":\"uncertain\"}}",
                static_cast<unsigned long>(operationId),
                static_cast<unsigned long>(route.id));
            if (length <= 0 || static_cast<size_t>(length) >= sizeof(responseBuffer))
                return sendError(request, 503, "removal_transport_uncertain",
                                 "The request could not be confirmed.");
            return sendJson(request, 503, responseBuffer, static_cast<size_t>(length));
        }
        const int length = snprintf(responseBuffer, sizeof(responseBuffer),
            "{\"operation\":{\"id\":%lu,\"device_id\":%lu,\"state\":\"processing\"}}",
            static_cast<unsigned long>(operationId),
            static_cast<unsigned long>(route.id));
        if (length <= 0 || static_cast<size_t>(length) >= sizeof(responseBuffer))
            return sendError(request, 500, "internal_error", "The response could not be serialized.");
        return sendJson(request, 202, responseBuffer, static_cast<size_t>(length));
    }

    if (route.kind == RemovalRouteKind::STATUS) {
        if (request->method != HTTP_GET)
            return sendError(request, 405, "method_not_allowed",
                             "Use GET to read a removal operation.", "GET");
        if (registryMutex == nullptr || *registryMutex == nullptr ||
            xSemaphoreTake(*registryMutex, pdMS_TO_TICKS(250)) != pdTRUE)
            return sendError(request, 503, "core_busy", "The CORE is busy; retry shortly.");
        const uint32_t now = millis();
        bool query = false;
        CoreLinkPacket command = {};
        if (removalOperationId != route.id) {
            const Device* main = nullptr;
            uint16_t mainCount = 0;
            (void)findPairingMain(registry, main, mainCount);
            if (main == nullptr || mainCount != 1 || main->localId == 0) {
                (void)xSemaphoreGive(*registryMutex);
                return sendError(request, 503, "main_unavailable",
                                 "The MAIN is not available for operation reconciliation.");
            }
            removalOperationId = route.id;
            removalTargetId = 0;
            removalMainLocalId = main->localId;
            removalMainCoreId = main->id;
            removalLampLocalId = 0;
            removalResult = CorePairingResultCode::UNCERTAIN;
            removalLastQueryAt = 0;
        }
        if ((removalResult == CorePairingResultCode::PROCESSING ||
             removalResult == CorePairingResultCode::UNCERTAIN) &&
            (removalLastQueryAt == 0 || now - removalLastQueryAt >= 2000)) {
            command.type = CoreLinkMessageType::PAIRING;
            command.sequence = static_cast<uint16_t>(route.id);
            command.pairingAction = CoreLinkPairingAction::QUERY_REMOVAL;
            command.localId = removalMainLocalId;
            command.parentId = removalMainCoreId;
            command.operationId = route.id;
            removalLastQueryAt = now;
            query = true;
        }
        const uint32_t targetId = removalTargetId;
        CorePairingResultCode result = removalResult;
        (void)xSemaphoreGive(*registryMutex);
        if (query && !uart.send(command)) {
            if (registryMutex != nullptr && *registryMutex != nullptr &&
                xSemaphoreTake(*registryMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
                removalResult = CorePairingResultCode::UNCERTAIN;
                (void)xSemaphoreGive(*registryMutex);
            }
            result = CorePairingResultCode::UNCERTAIN;
        }
        const int length = snprintf(responseBuffer, sizeof(responseBuffer),
            "{\"operation\":{\"id\":%lu,\"device_id\":%lu,\"state\":\"%s\",\"result_code\":\"%s\"}}",
            static_cast<unsigned long>(route.id),
            static_cast<unsigned long>(targetId), removalStateName(result),
            corePairingResultCodeName(result));
        if (length <= 0 || static_cast<size_t>(length) >= sizeof(responseBuffer))
            return sendError(request, 500, "internal_error", "The response could not be serialized.");
        return sendJson(request, 200, responseBuffer, static_cast<size_t>(length));
    }
    return sendError(request, 404, "not_found", "The removal resource was not found.");
}

esp_err_t CoreHttpApi::handlePairingRequest(httpd_req_t* request) {
    const PairingRoute route = parsePairingRoute(request->uri);
    if (route.kind == PairingRouteKind::INVALID)
        return sendError(request, 400, "bad_request", "The pairing path or ID is invalid.");
    if (!coreHttpTokenConfigured(apiToken))
        return sendError(request, 503, "pairing_api_disabled",
                         "Configure a valid API token to enable pairing.");
    if (!authorized(request, apiToken))
        return sendError(request, 401, "unauthorized", "A valid Bearer token is required.");

    const uint32_t now = millis();
    pollPairing(now);

    if (route.kind == PairingRouteKind::SESSION) {
        if (request->method == HTTP_POST) {
            if (request->content_len > MAX_PAIRING_BODY_BYTES)
                return sendError(request, 413, "payload_too_large",
                                 "The pairing request body is too large.");
            char body[MAX_PAIRING_BODY_BYTES + 1] = {};
            uint16_t duration = 0;
            if (!parsePairingBody(request, body, sizeof(body)) ||
                !parseSessionBody(body, duration)) {
                return sendError(request, 400, "bad_request",
                                 "Expected JSON with duration_seconds between 15 and 180.");
            }
            if (registryMutex == nullptr || *registryMutex == nullptr ||
                xSemaphoreTake(*registryMutex, pdMS_TO_TICKS(250)) != pdTRUE)
                return sendError(request, 503, "core_busy", "The CORE is busy; retry shortly.");

            const Device* main = nullptr;
            uint16_t mainCount = 0;
            const bool exactlyOneMain = findPairingMain(registry, main, mainCount);
            if (!exactlyOneMain || main == nullptr || mainCount != 1) {
                (void)xSemaphoreGive(*registryMutex);
                return sendError(request, mainCount > 1 ? 409 : 503,
                    mainCount > 1 ? "unsupported_topology" : "main_unavailable",
                    mainCount > 1 ? "Pairing currently supports one MAIN." :
                                    "No assigned MAIN is available.");
            }
            if (main->status != DeviceStatus::ONLINE) {
                (void)xSemaphoreGive(*registryMutex);
                return sendError(request, 409, "main_offline",
                                 "The MAIN must be online to open a pairing session.");
            }

            const uint32_t sessionId = makePairingId();
            CoreLinkPacket command = {};
            command.type = CoreLinkMessageType::PAIRING;
            command.sequence = static_cast<uint16_t>(sessionId);
            command.pairingAction = CoreLinkPairingAction::OPEN_SESSION;
            command.localId = main->localId;
            command.sessionId = sessionId;
            command.pairingDurationSeconds = duration;
            const bool accepted = pairing.beginSession(sessionId, main->localId,
                                                       duration, now);
            (void)xSemaphoreGive(*registryMutex);
            if (!accepted)
                return sendError(request, 409, "pairing_session_busy",
                                 "A pairing session or operation is already active.");
            if (!uart.send(command)) {
                if (registryMutex != nullptr && *registryMutex != nullptr &&
                    xSemaphoreTake(*registryMutex, pdMS_TO_TICKS(250)) == pdTRUE) {
                    pairing.failSessionRequest(sessionId, true, millis());
                    (void)xSemaphoreGive(*registryMutex);
                }
                return sendError(request, 503, "pairing_transport_uncertain",
                                 "The session request could not be confirmed; inspect its status before retrying.");
            }
            const int length = snprintf(responseBuffer, sizeof(responseBuffer),
                "{\"session\":{\"id\":%lu,\"state\":\"opening\",\"duration_seconds\":%u}}",
                static_cast<unsigned long>(sessionId), static_cast<unsigned>(duration));
            if (length <= 0 || static_cast<size_t>(length) >= sizeof(responseBuffer))
                return sendError(request, 500, "internal_error", "The response could not be serialized.");
            return sendJson(request, 202, responseBuffer, static_cast<size_t>(length));
        }
        if (request->method == HTTP_GET) {
            if (registryMutex == nullptr || *registryMutex == nullptr ||
                xSemaphoreTake(*registryMutex, pdMS_TO_TICKS(250)) != pdTRUE)
                return sendError(request, 503, "core_busy", "The CORE is busy; retry shortly.");
            if (pairing.getSessionState() == CorePairingSessionState::NONE) {
                (void)xSemaphoreGive(*registryMutex);
                constexpr char EMPTY_SESSION[] = "{\"session\":null}";
                return sendJson(request, 200, EMPTY_SESSION, sizeof(EMPTY_SESSION) - 1);
            }
            const int length = snprintf(responseBuffer, sizeof(responseBuffer),
                "{\"session\":{\"id\":%lu,\"state\":\"%s\",\"duration_seconds\":%u,\"candidate_count\":%u,\"candidate_limit_reached\":%s}}",
                static_cast<unsigned long>(pairing.getSessionId()),
                corePairingSessionStateName(pairing.getSessionState()),
                static_cast<unsigned>(pairing.getDurationSeconds()),
                static_cast<unsigned>(pairing.getCandidateCount()),
                pairing.hasCandidateLimit() ? "true" : "false");
            (void)xSemaphoreGive(*registryMutex);
            if (length <= 0 || static_cast<size_t>(length) >= sizeof(responseBuffer))
                return sendError(request, 500, "internal_error", "The response could not be serialized.");
            return sendJson(request, 200, responseBuffer, static_cast<size_t>(length));
        }
        if (request->method == HTTP_DELETE) {
            if (registryMutex == nullptr || *registryMutex == nullptr ||
                xSemaphoreTake(*registryMutex, pdMS_TO_TICKS(250)) != pdTRUE)
                return sendError(request, 503, "core_busy", "The CORE is busy; retry shortly.");
            const uint32_t sessionId = pairing.getSessionId();
            const uint32_t mainLocalId = pairing.getMainLocalId();
            const bool accepted = pairing.beginClose(sessionId, now);
            (void)xSemaphoreGive(*registryMutex);
            if (!accepted)
                return sendError(request, 409, "session_not_closable",
                                 "No active session can be closed, or a pairing operation is in progress.");
            CoreLinkPacket command = {};
            command.type = CoreLinkMessageType::PAIRING;
            command.sequence = static_cast<uint16_t>(sessionId);
            command.pairingAction = CoreLinkPairingAction::CLOSE_SESSION;
            command.localId = mainLocalId;
            command.sessionId = sessionId;
            if (!uart.send(command)) {
                if (registryMutex != nullptr && *registryMutex != nullptr &&
                    xSemaphoreTake(*registryMutex, pdMS_TO_TICKS(250)) == pdTRUE) {
                    pairing.failSessionRequest(sessionId, true, millis());
                    (void)xSemaphoreGive(*registryMutex);
                }
                return sendError(request, 503, "pairing_transport_uncertain",
                                 "The close request was not confirmed; inspect session status.");
            }
            const int length = snprintf(responseBuffer, sizeof(responseBuffer),
                "{\"session\":{\"id\":%lu,\"state\":\"closing\"}}",
                static_cast<unsigned long>(sessionId));
            if (length <= 0 || static_cast<size_t>(length) >= sizeof(responseBuffer))
                return sendError(request, 500, "internal_error", "The response could not be serialized.");
            return sendJson(request, 202, responseBuffer, static_cast<size_t>(length));
        }
        return sendError(request, 405, "method_not_allowed",
                         "Use POST, GET, or DELETE for the pairing session.", "POST, GET, DELETE");
    }

    if (route.kind == PairingRouteKind::CANDIDATES) {
        if (request->method != HTTP_GET)
            return sendError(request, 405, "method_not_allowed",
                             "Use GET to read pairing candidates.", "GET");
        if (registryMutex == nullptr || *registryMutex == nullptr ||
            xSemaphoreTake(*registryMutex, pdMS_TO_TICKS(250)) != pdTRUE)
            return sendError(request, 503, "core_busy", "The CORE is busy; retry shortly.");
        int length = snprintf(responseBuffer, sizeof(responseBuffer),
            "{\"session_id\":%lu,\"capacity_reached\":%s,\"candidates\":[",
            static_cast<unsigned long>(pairing.getSessionId()),
            pairing.hasCandidateLimit() ? "true" : "false");
        bool ok = length > 0 && static_cast<size_t>(length) < sizeof(responseBuffer);
        size_t used = ok ? static_cast<size_t>(length) : 0;
        for (uint8_t index = 0; ok && index < pairing.getCandidateCount(); ++index) {
            const CorePairingCandidate* candidate = pairing.getCandidate(index);
            if (candidate == nullptr) { ok = false; break; }
            length = snprintf(responseBuffer + used, sizeof(responseBuffer) - used,
                "%s{\"hardware_id\":\"%s\",\"capabilities\":%lu,\"last_seen_ms\":%lu}",
                index == 0 ? "" : ",", candidate->hardwareId,
                static_cast<unsigned long>(candidate->capabilities),
                static_cast<unsigned long>(candidate->lastSeen));
            if (length <= 0 || static_cast<size_t>(length) >= sizeof(responseBuffer) - used)
                ok = false;
            else used += static_cast<size_t>(length);
        }
        if (ok) {
            length = snprintf(responseBuffer + used, sizeof(responseBuffer) - used,
                              "],\"count\":%u}",
                              static_cast<unsigned>(pairing.getCandidateCount()));
            if (length <= 0 || static_cast<size_t>(length) >= sizeof(responseBuffer) - used)
                ok = false;
            else used += static_cast<size_t>(length);
        }
        (void)xSemaphoreGive(*registryMutex);
        if (!ok) return sendError(request, 500, "internal_error", "The response could not be serialized.");
        return sendJson(request, 200, responseBuffer, used);
    }

    if (route.kind == PairingRouteKind::OPERATION_CREATE) {
        if (request->method != HTTP_POST)
            return sendError(request, 405, "method_not_allowed",
                             "Use POST to request pairing.", "POST");
        if (request->content_len > MAX_PAIRING_BODY_BYTES)
            return sendError(request, 413, "payload_too_large",
                             "The pairing request body is too large.");
        char body[MAX_PAIRING_BODY_BYTES + 1] = {};
        char hardwareId[32] = {};
        char name[32] = {};
        uint32_t requestedSessionId = 0;
        uint32_t requestedOperationId = 0;
        if (!parsePairingBody(request, body, sizeof(body)) ||
            !parsePairOperationBody(body, requestedSessionId,
                                    requestedOperationId, hardwareId,
                                    sizeof(hardwareId), name, sizeof(name)) ||
            !isValidCorePairingHardwareId(hardwareId) ||
            !isValidCorePairingName(name)) {
            return sendError(request, 400, "bad_request",
                "Expected nonzero session_id and operation_id, a C6 hardware_id, and a 1 to 31 character ASCII name.");
        }
        if (registryMutex == nullptr || *registryMutex == nullptr ||
            xSemaphoreTake(*registryMutex, pdMS_TO_TICKS(250)) != pdTRUE)
            return sendError(request, 503, "core_busy", "The CORE is busy; retry shortly.");
        if (pairing.getOperationId() == requestedOperationId &&
            pairing.getOperationState() != CorePairingOperationState::NONE) {
            const bool sameRequest =
                pairing.getOperationSessionId() == requestedSessionId &&
                strcmp(pairing.getOperationHardwareId(), hardwareId) == 0 &&
                strcmp(pairing.getOperationName(), name) == 0;
            if (!sameRequest) {
                (void)xSemaphoreGive(*registryMutex);
                return sendError(request, 409, "operation_id_conflict",
                    "This operation_id is already associated with another request.");
            }
            const int length = snprintf(responseBuffer, sizeof(responseBuffer),
                "{\"operation\":{\"id\":%lu,\"session_id\":%lu,\"hardware_id\":\"%s\",\"state\":\"%s\"}}",
                static_cast<unsigned long>(requestedOperationId),
                static_cast<unsigned long>(requestedSessionId), hardwareId,
                corePairingOperationStateName(pairing.getOperationState()));
            (void)xSemaphoreGive(*registryMutex);
            if (length <= 0 || static_cast<size_t>(length) >= sizeof(responseBuffer))
                return sendError(request, 500, "internal_error", "The response could not be serialized.");
            return sendJson(request, 200, responseBuffer, static_cast<size_t>(length));
        }
        const uint32_t sessionId = pairing.getSessionId();
        const uint32_t mainLocalId = pairing.getMainLocalId();
        const Device* main = nullptr;
        uint16_t mainCount = 0;
        (void)findPairingMain(registry, main, mainCount);
        if (pairing.getSessionState() != CorePairingSessionState::ACTIVE ||
            requestedSessionId != sessionId) {
            (void)xSemaphoreGive(*registryMutex);
            return sendError(request, 409, "session_not_active",
                             "The requested pairing session is not active.");
        }
        if (main == nullptr || mainCount != 1 || main->localId != mainLocalId ||
            main->status != DeviceStatus::ONLINE) {
            pairing.markCommunicationLost(now);
            (void)xSemaphoreGive(*registryMutex);
            return sendError(request, 409, "main_offline",
                             "The MAIN is offline; the pairing result is uncertain.");
        }
        if (pairing.findCandidate(hardwareId) == nullptr) {
            (void)xSemaphoreGive(*registryMutex);
            return sendError(request, 404, "candidate_not_found",
                             "The candidate is absent or has expired.");
        }
        const uint32_t operationId = requestedOperationId;
        if (!pairing.beginOperation(sessionId, operationId, hardwareId, name, now)) {
            (void)xSemaphoreGive(*registryMutex);
            return sendError(request, 409, "pairing_in_progress",
                             "Another pairing operation is already active.");
        }
        (void)xSemaphoreGive(*registryMutex);

        CoreLinkPacket command = {};
        command.type = CoreLinkMessageType::PAIRING;
        command.sequence = static_cast<uint16_t>(operationId);
        command.pairingAction = CoreLinkPairingAction::PAIR_CANDIDATE;
        command.localId = mainLocalId;
        command.sessionId = sessionId;
        command.operationId = operationId;
        strncpy(command.hardwareId, hardwareId, sizeof(command.hardwareId) - 1);
        strncpy(command.pairingName, name, sizeof(command.pairingName) - 1);
        if (!uart.send(command)) {
            if (registryMutex != nullptr && *registryMutex != nullptr &&
                xSemaphoreTake(*registryMutex, pdMS_TO_TICKS(250)) == pdTRUE) {
                pairing.failOperationRequest(operationId, millis());
                (void)xSemaphoreGive(*registryMutex);
            }
            return sendError(request, 503, "pairing_transport_uncertain",
                             "The operation was not confirmed; do not submit it again automatically.");
        }
        const int length = snprintf(responseBuffer, sizeof(responseBuffer),
            "{\"operation\":{\"id\":%lu,\"session_id\":%lu,\"hardware_id\":\"%s\",\"state\":\"pending\"}}",
            static_cast<unsigned long>(operationId),
            static_cast<unsigned long>(sessionId), hardwareId);
        if (length <= 0 || static_cast<size_t>(length) >= sizeof(responseBuffer))
            return sendError(request, 500, "internal_error", "The response could not be serialized.");
        return sendJson(request, 202, responseBuffer, static_cast<size_t>(length));
    }

    if (route.kind == PairingRouteKind::OPERATION_STATUS) {
        if (request->method != HTTP_GET)
            return sendError(request, 405, "method_not_allowed",
                             "Use GET to read an operation status.", "GET");
        if (registryMutex == nullptr || *registryMutex == nullptr ||
            xSemaphoreTake(*registryMutex, pdMS_TO_TICKS(250)) != pdTRUE)
            return sendError(request, 503, "core_busy", "The CORE is busy; retry shortly.");
        if (pairing.getOperationId() != route.id ||
            pairing.getOperationState() == CorePairingOperationState::NONE) {
            (void)xSemaphoreGive(*registryMutex);
            return sendError(request, 404, "operation_not_found",
                             "The operation is unknown or no longer retained in memory.");
        }
        const int length = snprintf(responseBuffer, sizeof(responseBuffer),
            "{\"operation\":{\"id\":%lu,\"session_id\":%lu,\"hardware_id\":\"%s\",\"name\":\"%s\",\"state\":\"%s\",\"result_code\":\"%s\",\"local_device_id\":%lu}}",
            static_cast<unsigned long>(pairing.getOperationId()),
            static_cast<unsigned long>(pairing.getOperationSessionId()),
            pairing.getOperationHardwareId(), pairing.getOperationName(),
            corePairingOperationStateName(pairing.getOperationState()),
            corePairingResultCodeName(pairing.getOperationResult()),
            static_cast<unsigned long>(pairing.getOperationDeviceId()));
        (void)xSemaphoreGive(*registryMutex);
        if (length <= 0 || static_cast<size_t>(length) >= sizeof(responseBuffer))
            return sendError(request, 500, "internal_error", "The response could not be serialized.");
        return sendJson(request, 200, responseBuffer, static_cast<size_t>(length));
    }

    return sendError(request, 404, "not_found", "The requested pairing resource was not found.");
}

void CoreHttpApi::handleGroupPacket(const CoreLinkPacket& packet) {
    if (packet.type != CoreLinkMessageType::GROUPS || groupStateMutex == nullptr)
        return;
    const GroupProtocolPayload& group = packet.group;
    if ((group.version != GROUP_PROTOCOL_VERSION &&
         group.version != GROUP_POWER_PROTOCOL_VERSION) || group.operationId == 0 ||
        group.mainLocalId == 0 || group.mainCoreId == 0 ||
        !isValidGroupProtocolAction(group.action) ||
        !isValidGroupProtocolResult(group.result) ||
        !isValidGroupProtocolPayload(group, GROUP_UART_MEMBER_CHUNK) ||
        xSemaphoreTake(groupStateMutex, pdMS_TO_TICKS(100)) != pdTRUE) return;

    CoreGroupOperationRecord* operation = nullptr;
    bool readOperation = false;
    for (uint8_t index = 0; index < CORE_GROUP_OPERATION_SLOTS; ++index) {
        if (groupOperations[index].id == group.operationId &&
            groupOperations[index].state != CoreGroupOperationState::EMPTY) {
            operation = &groupOperations[index];
            break;
        }
    }
    if (operation == nullptr && groupReadOperation.id == group.operationId &&
        groupReadOperation.state != CoreGroupOperationState::EMPTY) {
        operation = &groupReadOperation;
        readOperation = true;
    }
    if (operation == nullptr ||
        operation->request.mainLocalId != group.mainLocalId ||
        operation->request.mainCoreId != group.mainCoreId ||
        (readOperation && operation->request.action != GroupProtocolAction::LIST &&
         operation->request.action != GroupProtocolAction::GET)) {
        (void)xSemaphoreGive(groupStateMutex);
        return;
    }
    GroupRegistry* readSnapshot = readOperation
        ? &groupReadOperation.snapshot : nullptr;

    if (isGroupPowerOperation(*operation)) {
        const CoreGroupOperationState previousState = operation->state;
        if (group.version != GROUP_POWER_PROTOCOL_VERSION ||
            group.groupId != operation->request.groupId ||
            group.power != operation->request.power ||
            ((operation->state == CoreGroupOperationState::COMPLETED ||
              operation->state == CoreGroupOperationState::FAILED) &&
             operation->receivedGroupCount >= operation->expectedGroupCount)) {
            (void)xSemaphoreGive(groupStateMutex);
            return;
        }
        bool terminal = false;
        uint32_t uncertainMainCoreId = 0;
        uint32_t uncertainOperationStartedAt = 0;
        uint32_t uncertainLampIds[MAX_GROUP_LAMPS] = {};
        uint8_t uncertainLampCount = 0;
        if (group.action == GroupProtocolAction::RESULT) {
            operation->result = group.result;
            operation->resultRevision = 0;
            if (group.groupCount <= MAX_GROUP_LAMPS &&
                (operation->expectedGroupCount == 0 ||
                 group.groupCount == operation->expectedGroupCount)) {
                operation->expectedGroupCount = group.groupCount;
            }
            if (group.result == GroupProtocolResult::ACCEPTED) {
                if (activeGroupOperationId == 0 ||
                    activeGroupOperationId == operation->id) {
                    const bool resumingUncertain =
                        operation->state == CoreGroupOperationState::UNCERTAIN;
                    operation->state = CoreGroupOperationState::ACCEPTED;
                    if (resumingUncertain) {
                        operation->startedAt = millis();
                        operation->lastQueryAt = operation->startedAt;
                        operation->statusQueryCount = 0;
                    }
                    activeGroupOperationId = operation->id;
                }
            } else if (group.result == GroupProtocolResult::COMPLETED ||
                       group.result == GroupProtocolResult::PARTIAL) {
                operation->mainFinalResultReceived = true;
                if (operation->expectedGroupCount == 0) {
                    operation->state = CoreGroupOperationState::UNCERTAIN;
                    operation->result = GroupProtocolResult::UNCERTAIN;
                } else if (operation->receivedGroupCount ==
                           operation->expectedGroupCount) {
                    updateGroupPowerAggregate(*operation);
                    terminal = groupOperationTerminal(operation->state);
                } else {
                    operation->state = CoreGroupOperationState::ACCEPTED;
                }
            } else if (group.result == GroupProtocolResult::UNCERTAIN) {
                operation->state = CoreGroupOperationState::UNCERTAIN;
                terminal = true;
                if (isGroupPowerOperation(*operation)) {
                    for (uint8_t memberIndex = 0;
                         memberIndex < MAX_GROUP_LAMPS; ++memberIndex) {
                        CoreGroupMemberResult& member = operation->members[memberIndex];
                        if (member.valid && member.result ==
                                GroupProtocolResult::IN_PROGRESS) {
                            member.result = GroupProtocolResult::UNCERTAIN;
                            if (uncertainLampCount < MAX_GROUP_LAMPS)
                                uncertainLampIds[uncertainLampCount++] =
                                    member.localId;
                        }
                    }
                    uncertainMainCoreId = operation->request.mainCoreId;
                    uncertainOperationStartedAt = operation->startedAt;
                }
            } else if (group.result == GroupProtocolResult::NOT_FOUND ||
                       group.result == GroupProtocolResult::EMPTY_GROUP ||
                       group.result == GroupProtocolResult::BUSY ||
                       group.result == GroupProtocolResult::INVALID ||
                       group.result == GroupProtocolResult::FAILED) {
                operation->state = CoreGroupOperationState::FAILED;
                terminal = true;
            }
        } else if (group.action == GroupProtocolAction::MEMBER_RESULT &&
                   group.groupCount > 0 &&
                   group.groupCount <= MAX_GROUP_LAMPS &&
                   group.memberOffset < group.groupCount &&
                   (operation->expectedGroupCount == 0 ||
                    operation->expectedGroupCount == group.groupCount)) {
            operation->expectedGroupCount = group.groupCount;
            CoreGroupMemberResult& member = operation->members[group.memberOffset];
            if (!member.valid) {
                bool duplicateLocal = false;
                for (uint8_t index = 0; index < MAX_GROUP_LAMPS; ++index)
                    if (operation->members[index].valid &&
                        operation->members[index].localId == group.memberLocalId)
                        duplicateLocal = true;
                bool duplicateChild = false;
                for (uint8_t index = 0; index < MAX_GROUP_LAMPS; ++index)
                    if (operation->members[index].valid &&
                        operation->members[index].childCommandId == group.childCommandId)
                        duplicateChild = true;
                if (!duplicateLocal && !duplicateChild && group.memberLocalId != 0 &&
                    group.childCommandId != 0) {
                    member.localId = group.memberLocalId;
                    member.childCommandId = group.childCommandId;
                    member.index = group.memberOffset;
                    member.result = group.result;
                    member.valid = true;
                    ++operation->receivedGroupCount;
                }
            } else if (member.localId == group.memberLocalId &&
                       member.childCommandId == group.childCommandId) {
                if (member.result == GroupProtocolResult::IN_PROGRESS ||
                    member.result == group.result) {
                    member.result = group.result;
                } else if (group.result == GroupProtocolResult::SUPERSEDED ||
                           (member.result == GroupProtocolResult::UNCERTAIN &&
                            isGroupMemberResultTerminal(group.result))) {
                    member.result = group.result;
                } else {
                    (void)xSemaphoreGive(groupStateMutex);
                    return;
                }
            }
            const bool updateLampOutput = member.valid &&
                (group.result == GroupProtocolResult::EXECUTED ||
                 group.result == GroupProtocolResult::UNCERTAIN);
            const uint32_t resultLampLocalId = group.memberLocalId;
            const GroupProtocolResult resultForLamp = group.result;
            const bool requestedPower = group.power != 0;
            const uint32_t operationStartedAt = operation->startedAt;
            if (operation->state == CoreGroupOperationState::SENT)
                operation->state = CoreGroupOperationState::ACCEPTED;
            updateGroupPowerAggregate(*operation);
            terminal = groupOperationTerminal(operation->state);
            const uint32_t resultMainCoreId = group.mainCoreId;
            updateGroupOperationRetention(*operation, previousState, millis());
            if (terminal && activeGroupOperationId == operation->id)
                activeGroupOperationId = 0;
            (void)xSemaphoreGive(groupStateMutex);
            if (updateLampOutput && registryMutex != nullptr &&
                *registryMutex != nullptr &&
                xSemaphoreTake(*registryMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
                commands.applyGroupPowerResult(resultMainCoreId,
                    resultLampLocalId, requestedPower, operationStartedAt,
                    resultForLamp);
                (void)xSemaphoreGive(*registryMutex);
            }
            return;
        }
        if (terminal && activeGroupOperationId == operation->id)
            activeGroupOperationId = 0;
        updateGroupOperationRetention(*operation, previousState, millis());
        (void)xSemaphoreGive(groupStateMutex);
        if (uncertainLampCount != 0 && registryMutex != nullptr &&
            *registryMutex != nullptr &&
            xSemaphoreTake(*registryMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            for (uint8_t index = 0; index < uncertainLampCount; ++index) {
                commands.applyGroupPowerResult(uncertainMainCoreId,
                    uncertainLampIds[index], false, uncertainOperationStartedAt,
                    GroupProtocolResult::UNCERTAIN);
            }
            (void)xSemaphoreGive(*registryMutex);
        }
        return;
    }

    if (operation->state == CoreGroupOperationState::PERSISTED ||
        operation->state == CoreGroupOperationState::COMPLETED ||
        operation->state == CoreGroupOperationState::FAILED ||
        operation->state == CoreGroupOperationState::UNCERTAIN) {
        (void)xSemaphoreGive(groupStateMutex);
        return;
    }

    const CoreGroupOperationState previousState = operation->state;
    bool terminal = false;
    if (group.action == GroupProtocolAction::RESULT) {
        operation->result = group.result;
        operation->resultRevision = group.expectedRevision;
        if (group.result == GroupProtocolResult::ACCEPTED) {
            operation->state = CoreGroupOperationState::ACCEPTED;
        } else if (group.result == GroupProtocolResult::PERSISTED &&
                   operation->request.action != GroupProtocolAction::LIST &&
                   operation->request.action != GroupProtocolAction::GET) {
            operation->state = CoreGroupOperationState::PERSISTED;
            terminal = true;
        } else if (group.result == GroupProtocolResult::COMPLETED &&
                   operation->request.action == GroupProtocolAction::GET &&
                   operation->receivedGroupCount == 1 &&
                   readSnapshot != nullptr &&
                   operation->receivedMembersForCurrent ==
                       readSnapshot->groups[0].lampCount) {
            operation->state = CoreGroupOperationState::COMPLETED;
            terminal = true;
        } else if (group.result == GroupProtocolResult::NOT_FOUND ||
                   group.result == GroupProtocolResult::REVISION_CONFLICT ||
                   group.result == GroupProtocolResult::INVALID ||
                   group.result == GroupProtocolResult::STORAGE_UNAVAILABLE ||
                   group.result == GroupProtocolResult::BUSY ||
                   group.result == GroupProtocolResult::FAILED ||
                   group.result == GroupProtocolResult::UNCERTAIN) {
            operation->state = group.result == GroupProtocolResult::UNCERTAIN
                ? CoreGroupOperationState::UNCERTAIN
                : CoreGroupOperationState::FAILED;
            terminal = true;
        }
    } else if (group.action == GroupProtocolAction::LIST_BEGIN &&
               operation->request.action == GroupProtocolAction::LIST &&
               group.groupCount <= MAX_GROUPS && !operation->listStarted) {
        if (readSnapshot == nullptr) {
            (void)xSemaphoreGive(groupStateMutex);
            return;
        }
        *readSnapshot = {};
        readSnapshot->mainId = group.mainCoreId;
        operation->expectedGroupCount = group.groupCount;
        operation->receivedGroupCount = 0;
        operation->receivedMembersForCurrent = 0;
        operation->listStarted = true;
        operation->state = CoreGroupOperationState::ACCEPTED;
    } else if (group.action == GroupProtocolAction::GROUP_META &&
               (operation->request.action == GroupProtocolAction::LIST ||
                operation->request.action == GroupProtocolAction::GET)) {
        const bool listReady = operation->request.action == GroupProtocolAction::LIST
            ? operation->listStarted &&
                operation->receivedGroupCount < operation->expectedGroupCount
            : operation->listStarted && operation->receivedGroupCount == 0;
        const bool previousComplete = operation->receivedGroupCount == 0 ||
            operation->receivedMembersForCurrent ==
                readSnapshot->groups[operation->receivedGroupCount - 1].lampCount;
        const bool targetMatches = operation->request.action != GroupProtocolAction::GET ||
            operation->request.groupId == group.groupId;
        if (listReady && previousComplete && targetMatches && group.groupId != 0 &&
            group.expectedRevision != 0 && group.memberCount <= MAX_GROUP_LAMPS) {
            bool duplicateId = false;
            for (uint8_t index = 0; index < operation->receivedGroupCount; ++index)
                if (readSnapshot->groups[index].id == group.groupId)
                    duplicateId = true;
            LampGroup candidate = {};
            candidate.id = group.groupId;
            candidate.revision = group.expectedRevision;
            candidate.lampCount = group.memberCount;
            if (!duplicateId && setGroupName(candidate, group.name)) {
                readSnapshot->groups[operation->receivedGroupCount] = candidate;
                ++operation->receivedGroupCount;
                readSnapshot->count = operation->receivedGroupCount;
                operation->receivedMembersForCurrent = 0;
                operation->state = CoreGroupOperationState::ACCEPTED;
            }
        }
    } else if (group.action == GroupProtocolAction::MEMBER_CHUNK &&
               (operation->request.action == GroupProtocolAction::LIST ||
                operation->request.action == GroupProtocolAction::GET) &&
               operation->receivedGroupCount != 0) {
        LampGroup& current = readSnapshot->groups[
            operation->receivedGroupCount - 1];
        bool validChunk = group.groupId == current.id && group.memberCount != 0 &&
            group.memberOffset == operation->receivedMembersForCurrent &&
            static_cast<uint16_t>(group.memberOffset) + group.memberCount <=
                current.lampCount;
        for (uint8_t index = 0; validChunk && index < group.memberCount; ++index) {
            const uint32_t memberId = group.memberIds[index];
            if (memberId == 0) { validChunk = false; break; }
            for (uint8_t prior = 0; prior < operation->receivedMembersForCurrent; ++prior)
                if (current.lampIds[prior] == memberId) validChunk = false;
            for (uint8_t prior = 0; prior < index; ++prior)
                if (group.memberIds[prior] == memberId) validChunk = false;
        }
        if (validChunk) {
            for (uint8_t index = 0; index < group.memberCount; ++index)
                current.lampIds[operation->receivedMembersForCurrent + index] =
                    group.memberIds[index];
            operation->receivedMembersForCurrent = static_cast<uint8_t>(
                operation->receivedMembersForCurrent + group.memberCount);
        }
    } else if (group.action == GroupProtocolAction::LIST_END &&
               operation->request.action == GroupProtocolAction::LIST &&
               operation->listStarted && group.groupCount == operation->expectedGroupCount &&
               operation->receivedGroupCount == operation->expectedGroupCount &&
               (operation->receivedGroupCount == 0 ||
                operation->receivedMembersForCurrent ==
                    readSnapshot->groups[operation->receivedGroupCount - 1].lampCount)) {
        operation->result = GroupProtocolResult::COMPLETED;
        operation->state = CoreGroupOperationState::COMPLETED;
        terminal = true;
    }

    updateGroupOperationRetention(*operation, previousState, millis());
    if (terminal && activeGroupOperationId == operation->id)
        activeGroupOperationId = 0;
    (void)xSemaphoreGive(groupStateMutex);
}

void CoreHttpApi::pollGroupOperations(uint32_t now) {
    if (groupStateMutex == nullptr ||
        xSemaphoreTake(groupStateMutex, pdMS_TO_TICKS(10)) != pdTRUE) return;
    uint32_t uncertainMainCoreId = 0;
    uint32_t uncertainOperationStartedAt = 0;
    uint32_t uncertainLampIds[MAX_GROUP_LAMPS] = {};
    uint8_t uncertainLampCount = 0;
    if (activeGroupOperationId != 0) {
        for (uint8_t index = 0; index < CORE_GROUP_OPERATION_SLOTS; ++index) {
            CoreGroupOperationRecord& operation = groupOperations[index];
            if (operation.id == activeGroupOperationId &&
                operation.state == CoreGroupOperationState::UNCERTAIN &&
                now - operation.lastQueryAt >=
                    GROUP_POWER_STATUS_QUERY_TIMEOUT_MS) {
                activeGroupOperationId = 0;
            } else if (operation.id == activeGroupOperationId &&
                (operation.state == CoreGroupOperationState::SENT ||
                 operation.state == CoreGroupOperationState::ACCEPTED)) {
                const bool powerOperation = isGroupPowerOperation(operation);
                if (powerOperation && operation.statusQueryCount < 2 &&
                    now - operation.lastQueryAt >= GROUP_POWER_STATUS_REQUERY_MS) {
                    CoreLinkPacket replay = {};
                    replay.sequence = static_cast<uint16_t>(operation.id);
                    replay.type = CoreLinkMessageType::GROUPS;
                    replay.group = operation.request;
                    replay.group.action = GroupProtocolAction::QUERY_OPERATION;
                    if (uart.send(replay)) {
                        ++operation.statusQueryCount;
                        operation.lastQueryAt = now;
                    }
                }
                const uint32_t timeout = powerOperation
                    ? GROUP_POWER_OPERATION_TIMEOUT_MS : GROUP_OPERATION_TIMEOUT_MS;
                if (now - operation.startedAt >= timeout) {
                    operation.state = CoreGroupOperationState::UNCERTAIN;
                    operation.result = GroupProtocolResult::UNCERTAIN;
                    operation.retainedAt = now;
                    if (powerOperation) {
                        uncertainMainCoreId = operation.request.mainCoreId;
                        uncertainOperationStartedAt = operation.startedAt;
                        for (uint8_t member = 0; member < MAX_GROUP_LAMPS; ++member) {
                            if (operation.members[member].valid &&
                                operation.members[member].result ==
                                    GroupProtocolResult::IN_PROGRESS) {
                                operation.members[member].result =
                                    GroupProtocolResult::UNCERTAIN;
                                uncertainLampIds[uncertainLampCount++] =
                                    operation.members[member].localId;
                            }
                        }
                    }
                    activeGroupOperationId = 0;
                    break;
                }
            }
        }
    }
    (void)xSemaphoreGive(groupStateMutex);
    if (uncertainLampCount != 0 && registryMutex != nullptr &&
        *registryMutex != nullptr &&
        xSemaphoreTake(*registryMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        for (uint8_t index = 0; index < uncertainLampCount; ++index) {
            commands.applyGroupPowerResult(uncertainMainCoreId,
                uncertainLampIds[index], false, uncertainOperationStartedAt,
                GroupProtocolResult::UNCERTAIN);
        }
        (void)xSemaphoreGive(*registryMutex);
    }
}

void CoreHttpApi::handlePairingPacket(const CoreLinkPacket& packet) {
    if (packet.type != CoreLinkMessageType::PAIRING ||
        registryMutex == nullptr || *registryMutex == nullptr ||
        xSemaphoreTake(*registryMutex, pdMS_TO_TICKS(100)) != pdTRUE) return;
    const uint32_t now = millis();
    switch (packet.pairingAction) {
        case CoreLinkPairingAction::REMOVAL_RESULT: {
            if (packet.operationId != removalOperationId ||
                packet.localId != removalMainLocalId ||
                packet.parentId != removalMainCoreId) break;
            if (packet.pairingResult != CorePairingResultCode::NOT_FOUND) {
                const bool identityUnavailable =
                    packet.pairingResult == CorePairingResultCode::UNCERTAIN &&
                    packet.deviceId == 0 && packet.coreId == 0;
                if (!identityUnavailable && (packet.deviceId == 0 || packet.coreId == 0 ||
                    makeCoreModuleId(packet.parentId, packet.deviceId) != packet.coreId ||
                    (removalTargetId != 0 && removalTargetId != packet.coreId) ||
                    (removalLampLocalId != 0 && removalLampLocalId != packet.deviceId)))
                    break;
                Device* target = identityUnavailable ? nullptr :
                    findDeviceById(registry, packet.coreId);
                if (target != nullptr &&
                    (target->role != DeviceRole::LAMP ||
                     target->parentId != packet.parentId ||
                     target->localId != packet.deviceId)) break;
                if (!identityUnavailable) {
                    removalTargetId = packet.coreId;
                    removalLampLocalId = packet.deviceId;
                }
            }
            removalResult = packet.pairingResult;
            if (packet.pairingResult == CorePairingResultCode::SUCCEEDED &&
                removalTargetId != 0) {
                (void)unregisterDevice(registry, removalTargetId);
            }
            break;
        }
        case CoreLinkPairingAction::CANDIDATE:
            (void)pairing.addCandidate(packet.sessionId, packet.localId,
                packet.hardwareId, packet.capabilities, now);
            break;
        case CoreLinkPairingAction::SESSION_RESULT:
            (void)pairing.applySessionResult(packet.sessionId,
                                             packet.pairingResult, now);
            break;
        case CoreLinkPairingAction::OPERATION_RESULT:
            (void)pairing.applyOperationResult(packet.sessionId,
                packet.operationId, packet.pairingResult, packet.deviceId,
                packet.hardwareId, now);
            break;
        case CoreLinkPairingAction::CANDIDATE_LIMIT:
            (void)pairing.markCandidateLimit(packet.sessionId);
            break;
        default:
            break;
    }
    (void)xSemaphoreGive(*registryMutex);
}

void CoreHttpApi::pollPairing(uint32_t now) {
    if (registryMutex == nullptr || *registryMutex == nullptr ||
        xSemaphoreTake(*registryMutex, pdMS_TO_TICKS(10)) != pdTRUE) return;
    pairing.poll(now);
    const CorePairingSessionState state = pairing.getSessionState();
    if (state == CorePairingSessionState::OPENING ||
        state == CorePairingSessionState::ACTIVE ||
        state == CorePairingSessionState::CLOSING) {
        const uint32_t mainId = makeCoreModuleId(CORE_LOGICAL_ID,
                                                 pairing.getMainLocalId());
        const Device* main = findDeviceById(registry, mainId);
        if (main == nullptr || main->status != DeviceStatus::ONLINE)
            pairing.markCommunicationLost(now);
    }
    (void)xSemaphoreGive(*registryMutex);
}

bool CoreHttpApi::begin() {
    if (started) return true;
    if (registryMutex == nullptr || *registryMutex == nullptr) return false;
    if (groupStateMutex == nullptr) groupStateMutex = xSemaphoreCreateMutex();
    if (groupStateMutex == nullptr)
        Serial.println("[CORE-WIFI] group API state mutex unavailable");

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    // Group requests keep bounded snapshots in the HTTP handler's stack frame.
    config.stack_size = 8192;
    config.uri_match_fn = httpd_uri_match_wildcard;

    if (httpd_start(&server, &config) != ESP_OK) {
        server = nullptr;
        Serial.println("[CORE-WIFI] HTTP server start failed");
        return false;
    }

    httpd_uri_t apiRoute = {};
    apiRoute.uri = "/api/v1/*";
    apiRoute.method = static_cast<httpd_method_t>(HTTP_ANY);
    apiRoute.handler = handleRequest;
    apiRoute.user_ctx = this;
    if (httpd_register_uri_handler(server, &apiRoute) != ESP_OK) {
        (void)httpd_stop(server);
        server = nullptr;
        Serial.println("[CORE-WIFI] HTTP route registration failed");
        return false;
    }

    started = true;
    return true;
}

bool CoreHttpApi::isReady() const {
    return started;
}

esp_err_t CoreHttpApi::handleRequest(httpd_req_t* request) {
    if (request == nullptr || request->user_ctx == nullptr) return ESP_FAIL;
    CoreHttpApi* api = static_cast<CoreHttpApi*>(request->user_ctx);
    const GroupRoute groupRoute = parseGroupRoute(request->uri);
    if (groupRoute.kind != GroupRouteKind::NONE)
        return api->handleGroupRequest(request);
    const RemovalRoute removalRoute = parseRemovalRoute(request->uri);
    if (removalRoute.kind != RemovalRouteKind::NONE)
        return api->handleRemovalRequest(request);
    const PairingRoute pairingRoute = parsePairingRoute(request->uri);
    if (pairingRoute.kind != PairingRouteKind::NONE)
        return api->handlePairingRequest(request);
    const CommandRoute commandRoute = parseCommandRoute(request->uri);
    if (commandRoute.kind != CommandRouteKind::NONE) {
        CommandApiContext context{
            api->commands, api->tracker, api->apiToken, api->registryMutex
        };
        return handleCommandRequest(context, request, commandRoute);
    }

    const char* method = request->method == HTTP_GET ? "GET" : "UNSUPPORTED";
    const esp_app_desc_t* description = esp_app_get_description();
    const CoreApiRuntimeInfo runtime = {
        static_cast<uint64_t>(esp_timer_get_time() / 1000),
        description == nullptr ? "unknown" : description->version,
        api->wifi.isReady(),
        api->uart.isReady()
    };

    static DeviceRegistry registrySnapshot = {};
    if (api->registryMutex == nullptr || *api->registryMutex == nullptr ||
        xSemaphoreTake(*api->registryMutex, pdMS_TO_TICKS(250)) != pdTRUE) {
        initDeviceRegistry(registrySnapshot);
    } else {
        registrySnapshot = api->registry;
        const uint8_t safeCount = registrySnapshot.count > MAX_DEVICES
            ? MAX_DEVICES
            : registrySnapshot.count;
        for (uint8_t index = 0; index < safeCount; ++index) {
            registrySnapshot.devices[index].name = registrySnapshot.names[index];
        }
        (void)xSemaphoreGive(*api->registryMutex);
    }

    size_t responseLength = 0;
    const uint16_t status = handleCoreApiRequest(
        registrySnapshot,
        runtime,
        method,
        request->uri,
        responseBuffer,
        sizeof(responseBuffer),
        responseLength
    );

    if (status != 200 &&
        httpd_resp_set_status(request, statusText(status)) != ESP_OK) {
        return ESP_FAIL;
    }
    if (httpd_resp_set_type(request, "application/json; charset=utf-8") != ESP_OK) {
        return ESP_FAIL;
    }
    if (status == 405 && httpd_resp_set_hdr(request, "Allow", "GET") != ESP_OK) {
        return ESP_FAIL;
    }
    return httpd_resp_send(request, responseBuffer,
                           static_cast<ssize_t>(responseLength));
}

#else

CoreHttpApi::CoreHttpApi(DeviceRegistry& coreRegistry,
                         WiFiTransport& wifiTransport,
                         CoreUartTransport& uartTransport,
                         SemaphoreHandle_t& coreRegistryMutex,
                         CoreCommandService& commandService,
                         MessageTracker& messageTracker,
                         const char* commandApiToken)
    : registry(coreRegistry), wifi(wifiTransport), uart(uartTransport),
      commands(commandService), tracker(messageTracker), apiToken(commandApiToken),
      registryMutex(&coreRegistryMutex), removalOperationId(0),
      removalTargetId(0), removalMainLocalId(0), removalMainCoreId(0),
      removalLampLocalId(0), removalLastQueryAt(0),
      removalResult(CorePairingResultCode::NONE), groupStateMutex(nullptr),
      activeGroupOperationId(0), nextGroupOperationSlot(0), groupReadOperation{},
      started(false) {}

bool CoreHttpApi::begin() { return false; }
bool CoreHttpApi::isReady() const { return started; }
void CoreHttpApi::handlePairingPacket(const CoreLinkPacket&) {}
void CoreHttpApi::handleGroupPacket(const CoreLinkPacket&) {}
void CoreHttpApi::pollPairing(uint32_t) {}
void CoreHttpApi::pollGroupOperations(uint32_t) {}

#endif
