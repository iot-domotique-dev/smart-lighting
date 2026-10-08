#include "core_http_api.h"

#if defined(SMART_LIGHTING_CORE_WIFI)

#include <Arduino.h>
#include <stdio.h>
#include <string.h>

#include "core_command_protocol.h"
#include "core_module_service.h"
#include "esp_app_desc.h"
#include "esp_timer.h"

namespace {
constexpr size_t MAX_COMMAND_BODY_BYTES = 64;
constexpr size_t MAX_AUTHORIZATION_BYTES = 160;
constexpr size_t MIN_API_TOKEN_BYTES = 32;
constexpr size_t MAX_API_TOKEN_BYTES = 128;
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

enum class CommandRouteKind : uint8_t { NONE, SUBMIT_POWER, STATUS, INVALID };

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
    if (!parseId(idStart, suffix, id) ||
        strcmp(suffix, "/commands/power") != 0 || strchr(uri, '?') != nullptr) {
        return {CommandRouteKind::INVALID, 0};
    }
    return {CommandRouteKind::SUBMIT_POWER, id};
}

bool configuredToken(const char* token) {
    if (token == nullptr) return false;
    const size_t length = strlen(token);
    return length >= MIN_API_TOKEN_BYTES && length <= MAX_API_TOKEN_BYTES;
}

bool authorized(httpd_req_t* request, const char* token) {
    if (!configuredToken(token)) return false;
    const size_t headerLength = httpd_req_get_hdr_value_len(request, "Authorization");
    if (headerLength == 0 || headerLength >= MAX_AUTHORIZATION_BYTES) return false;

    char header[MAX_AUTHORIZATION_BYTES] = {};
    if (httpd_req_get_hdr_value_str(request, "Authorization", header,
                                    sizeof(header)) != ESP_OK) {
        return false;
    }
    constexpr char PREFIX[] = "Bearer ";
    constexpr size_t PREFIX_LENGTH = sizeof(PREFIX) - 1;
    const size_t tokenLength = strlen(token);
    if (headerLength != PREFIX_LENGTH + tokenLength ||
        memcmp(header, PREFIX, PREFIX_LENGTH) != 0) {
        return false;
    }

    uint8_t difference = 0;
    for (size_t index = 0; index < tokenLength; ++index) {
        difference |= static_cast<uint8_t>(header[PREFIX_LENGTH + index] ^ token[index]);
    }
    return difference == 0;
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
        error <= static_cast<int32_t>(CoreCommandError::DUPLICATE_ID)) {
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
    if (!configuredToken(api.apiToken))
        return sendError(request, 503, "command_api_disabled",
                         "Configure a 32 to 128 character API token to enable commands.");
    if (!authorized(request, api.apiToken))
        return sendError(request, 401, "unauthorized", "A valid Bearer token is required.");

    if (route.kind == CommandRouteKind::SUBMIT_POWER) {
        if (request->method != HTTP_POST)
            return sendError(request, 405, "method_not_allowed",
                             "Use POST to submit a power command.", "POST");
        if (request->content_len > MAX_COMMAND_BODY_BYTES)
            return sendError(request, 413, "payload_too_large",
                             "The command body must not exceed 64 bytes.");
        if (!isJsonContentType(request))
            return sendError(request, 415, "unsupported_media_type",
                             "Content-Type must be application/json.");
        bool power = false;
        if (!receivePowerState(request, power))
            return sendError(request, 400, "bad_request",
                             "Expected a JSON body with state set to on or off.");

        if (api.registryMutex == nullptr || *api.registryMutex == nullptr ||
            xSemaphoreTake(*api.registryMutex, pdMS_TO_TICKS(250)) != pdTRUE) {
            return sendError(request, 503, "core_busy", "The CORE is busy; retry shortly.");
        }
        uint32_t id = 0;
        const CoreCommandError result = api.commands.submitPower(route.id, power, id);
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
      registryMutex(&coreRegistryMutex), started(false), server(nullptr) {}

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
      registryMutex(&coreRegistryMutex), started(false) {}

bool CoreHttpApi::begin() { return false; }
bool CoreHttpApi::isReady() const { return started; }

#endif
