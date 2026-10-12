#include <stdint.h>
#include <string.h>

#include "core_http_auth.h"

namespace {
constexpr size_t MIN_TOKEN_LENGTH = 32;
constexpr size_t MAX_TOKEN_LENGTH = 128;
constexpr size_t MAX_AUTHORIZATION_LENGTH = 160;
constexpr char BEARER_PREFIX[] = "Bearer ";
}

bool coreHttpHasValidBearer(const char* authorization, size_t headerLength,
                            const char* configuredToken) {
    if (!coreHttpTokenConfigured(configuredToken) || authorization == nullptr)
        return false;
    const size_t tokenLength = strlen(configuredToken);
    if (
        headerLength >= MAX_AUTHORIZATION_LENGTH ||
        headerLength != sizeof(BEARER_PREFIX) - 1 + tokenLength ||
        memcmp(authorization, BEARER_PREFIX, sizeof(BEARER_PREFIX) - 1) != 0)
        return false;

    uint8_t difference = 0;
    for (size_t index = 0; index < tokenLength; ++index) {
        difference |= static_cast<uint8_t>(
            authorization[sizeof(BEARER_PREFIX) - 1 + index] ^
            configuredToken[index]);
    }
    return difference == 0;
}

bool coreHttpTokenConfigured(const char* configuredToken) {
    if (configuredToken == nullptr) return false;
    const size_t tokenLength = strlen(configuredToken);
    return tokenLength >= MIN_TOKEN_LENGTH && tokenLength <= MAX_TOKEN_LENGTH;
}
