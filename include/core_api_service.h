#pragma once

#include <stddef.h>
#include <stdint.h>

#include "device_registry.h"

constexpr size_t CORE_API_MAX_RESPONSE_BYTES = 20 * 1024;

struct CoreApiRuntimeInfo {
    uint64_t uptimeMs;
    const char* firmwareVersion;
    bool wifiConnected;
    bool uartDriverReady;
};

/*
 * Route and serialize one request without exposing Device or DeviceRegistry
 * layouts over HTTP. `responseBody` must be caller-owned storage.
 */
uint16_t handleCoreApiRequest(
    const DeviceRegistry& registry,
    const CoreApiRuntimeInfo& runtime,
    const char* method,
    const char* uri,
    char* responseBody,
    size_t responseCapacity,
    size_t& responseLength
);
