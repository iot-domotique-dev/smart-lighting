#include <stdio.h>
#include <string.h>

#include "core_api_service.h"

#include "core_module_service.h"

namespace {
enum class RouteKind : uint8_t {
    UNKNOWN,
    INVALID_ID,
    HEALTH,
    CORE,
    MODULE_LIST,
    MODULE_ITEM,
    DEVICE_LIST,
    DEVICE_ITEM
};

struct Route {
    RouteKind kind;
    uint32_t id;
};

struct JsonWriter {
    char* output;
    size_t capacity;
    size_t length;
    bool failed;
};

bool appendBytes(JsonWriter& writer, const char* value, size_t length) {
    if (writer.failed || value == nullptr ||
        length >= writer.capacity - writer.length) {
        writer.failed = true;
        return false;
    }
    memcpy(writer.output + writer.length, value, length);
    writer.length += length;
    writer.output[writer.length] = '\0';
    return true;
}

bool appendLiteral(JsonWriter& writer, const char* value) {
    return value != nullptr && appendBytes(writer, value, strlen(value));
}

bool appendCharacter(JsonWriter& writer, char value) {
    return appendBytes(writer, &value, 1);
}

bool appendUnsigned(JsonWriter& writer, uint64_t value) {
    char number[24] = {};
    const int length = snprintf(number, sizeof(number), "%llu",
                                static_cast<unsigned long long>(value));
    return length > 0 && static_cast<size_t>(length) < sizeof(number) &&
           appendBytes(writer, number, static_cast<size_t>(length));
}

bool appendBoolean(JsonWriter& writer, bool value) {
    return appendLiteral(writer, value ? "true" : "false");
}

bool appendJsonString(JsonWriter& writer, const char* value) {
    if (!appendCharacter(writer, '"')) return false;
    if (value != nullptr) {
        static const char HEX[] = "0123456789abcdef";
        for (const unsigned char* cursor =
                 reinterpret_cast<const unsigned char*>(value);
             *cursor != '\0'; ++cursor) {
            const unsigned char byte = *cursor;
            if (byte == '"' || byte == '\\') {
                if (!appendCharacter(writer, '\\') ||
                    !appendCharacter(writer, static_cast<char>(byte))) {
                    return false;
                }
            } else if (byte < 0x20) {
                const char escaped[] = {
                    '\\', 'u', '0', '0', HEX[(byte >> 4) & 0x0f],
                    HEX[byte & 0x0f]
                };
                if (!appendBytes(writer, escaped, sizeof(escaped))) return false;
            } else if (!appendCharacter(writer, static_cast<char>(byte))) {
                return false;
            }
        }
    }
    return appendCharacter(writer, '"');
}

const char* roleName(DeviceRole role) {
    switch (role) {
        case DeviceRole::CORE: return "core";
        case DeviceRole::MAIN: return "main";
        case DeviceRole::LAMP: return "lamp";
        case DeviceRole::RELAY: return "relay";
        case DeviceRole::SENSOR: return "sensor";
        case DeviceRole::CAMERA: return "camera";
    }
    return "unknown";
}

bool isOnline(const Device& device) {
    return device.status == DeviceStatus::ONLINE;
}

bool appendCapability(JsonWriter& writer, bool& first, uint32_t capabilities,
                      DeviceCapability capability, const char* name) {
    if ((capabilities & static_cast<uint32_t>(capability)) == 0) return true;
    if (!first && !appendCharacter(writer, ',')) return false;
    first = false;
    return appendJsonString(writer, name);
}

bool appendCapabilities(JsonWriter& writer, uint32_t capabilities) {
    if (!appendCharacter(writer, '[')) return false;
    bool first = true;
    if (!appendCapability(writer, first, capabilities, DEVICE_CAP_POWER, "power") ||
        !appendCapability(writer, first, capabilities, DEVICE_CAP_BRIGHTNESS, "brightness") ||
        !appendCapability(writer, first, capabilities, DEVICE_CAP_AUTOMATIC, "automatic") ||
        !appendCapability(writer, first, capabilities, DEVICE_CAP_LIGHTING, "lighting") ||
        !appendCapability(writer, first, capabilities, DEVICE_CAP_GROUPS, "groups") ||
        !appendCapability(writer, first, capabilities, DEVICE_CAP_SCENES, "scenes") ||
        !appendCapability(writer, first, capabilities, DEVICE_CAP_AUTOMATION, "automation") ||
        !appendCapability(writer, first, capabilities, DEVICE_CAP_ALARM, "alarm") ||
        !appendCapability(writer, first, capabilities, DEVICE_CAP_PRESENCE, "presence") ||
        !appendCapability(writer, first, capabilities, DEVICE_CAP_DOOR_SENSOR, "door_sensor") ||
        !appendCapability(writer, first, capabilities, DEVICE_CAP_SECURITY_MODE, "security_mode")) {
        return false;
    }
    return appendCharacter(writer, ']');
}

bool appendDevice(JsonWriter& writer, const Device& device) {
    return appendLiteral(writer, "{\"id\":") &&
           appendUnsigned(writer, device.id) &&
           appendLiteral(writer, ",\"name\":") &&
           appendJsonString(writer, device.name) &&
           appendLiteral(writer, ",\"role\":") &&
           appendJsonString(writer, roleName(device.role)) &&
           appendLiteral(writer, ",\"online\":") &&
           appendBoolean(writer, isOnline(device)) &&
           appendLiteral(writer, ",\"status\":") &&
           appendJsonString(writer, isOnline(device) ? "online" : "offline") &&
           appendLiteral(writer, ",\"parent_id\":") &&
           appendUnsigned(writer, device.parentId) &&
           appendLiteral(writer, ",\"capabilities\":") &&
           appendCapabilities(writer, device.capabilities) &&
           appendLiteral(writer, ",\"last_seen_ms\":") &&
           appendUnsigned(writer, device.lastSeen) &&
           appendLiteral(writer, ",\"state\":null}");
}

bool appendError(JsonWriter& writer, const char* code, const char* message) {
    return appendLiteral(writer, "{\"error\":{\"code\":") &&
           appendJsonString(writer, code) &&
           appendLiteral(writer, ",\"message\":") &&
           appendJsonString(writer, message) &&
           appendLiteral(writer, "}}");
}

uint16_t serializeError(char* output, size_t capacity, size_t& length,
                        uint16_t status, const char* code,
                        const char* message) {
    if (output == nullptr || capacity == 0) {
        length = 0;
        return status;
    }
    output[0] = '\0';
    JsonWriter writer{output, capacity, 0, false};
    if (!appendError(writer, code, message)) {
        output[0] = '\0';
        length = 0;
        return 500;
    }
    length = writer.length;
    return status;
}

bool parseId(const char* text, uint32_t& id) {
    if (text == nullptr || *text == '\0') return false;
    uint32_t parsed = 0;
    for (const char* cursor = text; *cursor != '\0'; ++cursor) {
        if (*cursor < '0' || *cursor > '9') return false;
        const uint32_t digit = static_cast<uint32_t>(*cursor - '0');
        if (parsed > (UINT32_MAX - digit) / 10U) return false;
        parsed = parsed * 10U + digit;
    }
    if (parsed == 0) return false;
    id = parsed;
    return true;
}

Route routeRequest(const char* uri) {
    if (uri == nullptr) return {RouteKind::UNKNOWN, 0};
    if (strchr(uri, '?') != nullptr) return {RouteKind::INVALID_ID, 0};
    if (strcmp(uri, "/api/v1/health") == 0) return {RouteKind::HEALTH, 0};
    if (strcmp(uri, "/api/v1/core") == 0) return {RouteKind::CORE, 0};
    if (strcmp(uri, "/api/v1/modules") == 0) return {RouteKind::MODULE_LIST, 0};
    if (strcmp(uri, "/api/v1/devices") == 0) return {RouteKind::DEVICE_LIST, 0};

    const char* suffix = nullptr;
    RouteKind itemKind = RouteKind::UNKNOWN;
    if (strncmp(uri, "/api/v1/modules/", 16) == 0) {
        suffix = uri + 16;
        itemKind = RouteKind::MODULE_ITEM;
    } else if (strncmp(uri, "/api/v1/devices/", 16) == 0) {
        suffix = uri + 16;
        itemKind = RouteKind::DEVICE_ITEM;
    } else {
        return {RouteKind::UNKNOWN, 0};
    }

    uint32_t id = 0;
    if (!parseId(suffix, id)) return {RouteKind::INVALID_ID, 0};
    return {itemKind, id};
}

bool appendHealth(JsonWriter& writer, const Device& core,
                  const CoreApiRuntimeInfo& runtime) {
    return appendLiteral(writer, "{\"status\":\"ok\",\"core_id\":") &&
           appendUnsigned(writer, core.id) &&
           appendLiteral(writer, ",\"firmware_version\":") &&
           appendJsonString(writer, runtime.firmwareVersion) &&
           appendLiteral(writer, ",\"uptime_ms\":") &&
           appendUnsigned(writer, runtime.uptimeMs) &&
           appendLiteral(writer, ",\"wifi_connected\":") &&
           appendBoolean(writer, runtime.wifiConnected) &&
           appendLiteral(writer, ",\"uart_driver_ready\":") &&
           appendBoolean(writer, runtime.uartDriverReady) &&
           appendCharacter(writer, '}');
}

bool appendCore(JsonWriter& writer, const Device& core,
                const CoreApiRuntimeInfo& runtime) {
    return appendLiteral(writer, "{\"id\":") &&
           appendUnsigned(writer, core.id) &&
           appendLiteral(writer, ",\"name\":") &&
           appendJsonString(writer, core.name) &&
           appendLiteral(writer, ",\"role\":\"core\",\"status\":") &&
           appendJsonString(writer, isOnline(core) ? "online" : "offline") &&
           appendLiteral(writer, ",\"online\":") &&
           appendBoolean(writer, isOnline(core)) &&
           appendLiteral(writer, ",\"api_version\":\"v1\",\"firmware_version\":") &&
           appendJsonString(writer, runtime.firmwareVersion) &&
           appendLiteral(writer, ",\"uptime_ms\":") &&
           appendUnsigned(writer, runtime.uptimeMs) &&
           appendLiteral(writer, ",\"wifi_connected\":") &&
           appendBoolean(writer, runtime.wifiConnected) &&
           appendLiteral(writer, ",\"uart_driver_ready\":") &&
           appendBoolean(writer, runtime.uartDriverReady) &&
           appendCharacter(writer, '}');
}

bool isModule(const Device& device) {
    return device.role == DeviceRole::MAIN;
}

bool isPhysicalDevice(const Device& device) {
    return device.role != DeviceRole::CORE && device.role != DeviceRole::MAIN;
}

uint16_t serializeRequestError(char* output, size_t capacity, size_t& length,
                               uint16_t status) {
    switch (status) {
        case 400:
            return serializeError(output, capacity, length, status,
                                  "bad_request", "The request path or ID is invalid.");
        case 404:
            return serializeError(output, capacity, length, status,
                                  "not_found", "The requested resource was not found.");
        case 405:
            return serializeError(output, capacity, length, status,
                                  "method_not_allowed", "Only GET is supported.");
        case 503:
            return serializeError(output, capacity, length, status,
                                  "core_not_ready", "The CORE registry is not ready.");
        default:
            return serializeError(output, capacity, length, 500,
                                  "internal_error", "The response could not be serialized.");
    }
}
}  // namespace

uint16_t handleCoreApiRequest(
    const DeviceRegistry& registry,
    const CoreApiRuntimeInfo& runtime,
    const char* method,
    const char* uri,
    char* responseBody,
    size_t responseCapacity,
    size_t& responseLength
) {
    responseLength = 0;
    if (responseBody == nullptr || responseCapacity == 0) return 500;
    responseBody[0] = '\0';

    if (method == nullptr || uri == nullptr) {
        return serializeRequestError(responseBody, responseCapacity,
                                     responseLength, 400);
    }

    const Route route = routeRequest(uri);
    if (route.kind == RouteKind::UNKNOWN) {
        return serializeRequestError(responseBody, responseCapacity,
                                     responseLength, 404);
    }
    if (route.kind == RouteKind::INVALID_ID) {
        return serializeRequestError(responseBody, responseCapacity,
                                     responseLength, 400);
    }
    if (strcmp(method, "GET") != 0) {
        return serializeRequestError(responseBody, responseCapacity,
                                     responseLength, 405);
    }
    if (registry.count > MAX_DEVICES) {
        return serializeRequestError(responseBody, responseCapacity,
                                     responseLength, 500);
    }

    const Device* core = findDeviceById(registry, CORE_LOGICAL_ID);
    if (core == nullptr || core->role != DeviceRole::CORE) {
        return serializeRequestError(responseBody, responseCapacity,
                                     responseLength, 503);
    }

    JsonWriter writer{responseBody, responseCapacity, 0, false};
    bool success = false;
    switch (route.kind) {
        case RouteKind::HEALTH:
            success = appendHealth(writer, *core, runtime);
            break;
        case RouteKind::CORE:
            success = appendLiteral(writer, "{\"core\":") &&
                      appendCore(writer, *core, runtime) &&
                      appendCharacter(writer, '}');
            break;
        case RouteKind::MODULE_LIST:
        case RouteKind::DEVICE_LIST: {
            const bool modules = route.kind == RouteKind::MODULE_LIST;
            const char* collection = modules ? "modules" : "devices";
            success = appendLiteral(writer, "{\"") &&
                      appendLiteral(writer, collection) &&
                      appendLiteral(writer, "\":[");
            bool first = true;
            uint32_t count = 0;
            for (uint8_t index = 0; success && index < registry.count; ++index) {
                const Device& device = registry.devices[index];
                if ((modules && !isModule(device)) ||
                    (!modules && !isPhysicalDevice(device))) {
                    continue;
                }
                if (!first) success = appendCharacter(writer, ',');
                if (success) success = appendDevice(writer, device);
                first = false;
                ++count;
            }
            success = success && appendLiteral(writer, "],\"count\":") &&
                      appendUnsigned(writer, count) && appendCharacter(writer, '}');
            break;
        }
        case RouteKind::MODULE_ITEM:
        case RouteKind::DEVICE_ITEM: {
            const Device* device = findDeviceById(registry, route.id);
            const bool expectedRole = device != nullptr &&
                (route.kind == RouteKind::MODULE_ITEM
                    ? isModule(*device)
                    : isPhysicalDevice(*device));
            if (!expectedRole) {
                return serializeRequestError(responseBody, responseCapacity,
                                             responseLength, 404);
            }
            const char* resource = route.kind == RouteKind::MODULE_ITEM
                ? "module"
                : "device";
            success = appendLiteral(writer, "{\"") &&
                      appendLiteral(writer, resource) &&
                      appendLiteral(writer, "\":") &&
                      appendDevice(writer, *device) &&
                      appendCharacter(writer, '}');
            break;
        }
        case RouteKind::UNKNOWN:
        case RouteKind::INVALID_ID:
            return serializeRequestError(responseBody, responseCapacity,
                                         responseLength, 404);
    }

    if (!success || writer.failed) {
        return serializeRequestError(responseBody, responseCapacity,
                                     responseLength, 500);
    }
    responseLength = writer.length;
    return 200;
}
