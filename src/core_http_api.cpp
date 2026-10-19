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

namespace {
constexpr size_t MAX_COMMAND_BODY_BYTES = 64;
constexpr size_t MAX_PAIRING_BODY_BYTES = 160;
constexpr size_t MAX_REMOVAL_BODY_BYTES = 64;
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
      removalResult(CorePairingResultCode::NONE), started(false), server(nullptr) {}

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
        if (!duplicate && commands.hasUnresolvedPowerCommand(target->id)) {
            (void)xSemaphoreGive(*registryMutex);
            return sendError(request, 409, "command_unresolved",
                "A power command is pending or its execution is unknown for this lamp.");
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

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
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
      removalResult(CorePairingResultCode::NONE), started(false) {}

bool CoreHttpApi::begin() { return false; }
bool CoreHttpApi::isReady() const { return started; }
void CoreHttpApi::handlePairingPacket(const CoreLinkPacket&) {}
void CoreHttpApi::pollPairing(uint32_t) {}

#endif
