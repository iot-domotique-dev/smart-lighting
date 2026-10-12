#pragma once

#include <stdint.h>

#include "core_link_codec.h"

constexpr uint8_t CORE_PAIRING_MAX_CANDIDATES = 10;
constexpr uint32_t CORE_PAIRING_CANDIDATE_TTL_MS = 30000;
constexpr uint32_t CORE_PAIRING_RESPONSE_TIMEOUT_MS = 5000;
constexpr uint32_t CORE_PAIRING_OPERATION_TIMEOUT_MS = 35000;

enum class CorePairingSessionState : uint8_t {
    NONE,
    OPENING,
    ACTIVE,
    CLOSING,
    CLOSED,
    EXPIRED,
    FAILED,
    UNCERTAIN
};

enum class CorePairingOperationState : uint8_t {
    NONE,
    PENDING,
    PROCESSING,
    SUCCEEDED,
    FAILED,
    REJECTED,
    EXPIRED,
    UNCERTAIN
};

struct CorePairingCandidate {
    char hardwareId[32];
    uint32_t capabilities;
    uint32_t lastSeen;
};

class CorePairingState {
private:
    CorePairingSessionState sessionState;
    uint32_t sessionId;
    uint32_t mainLocalId;
    uint16_t durationSeconds;
    uint32_t sessionStateAt;
    bool candidateLimitReached;
    CorePairingCandidate candidates[CORE_PAIRING_MAX_CANDIDATES];
    uint8_t candidateCount;

    CorePairingOperationState operationState;
    uint32_t operationId;
    uint32_t operationSessionId;
    uint32_t operationDeviceId;
    CorePairingResultCode operationResult;
    uint32_t operationStateAt;
    char operationHardwareId[32];
    char operationName[32];

public:
    CorePairingState();

    bool beginSession(uint32_t newSessionId, uint32_t targetMainLocalId,
                      uint16_t duration, uint32_t now);
    bool beginClose(uint32_t targetSessionId, uint32_t now);
    bool beginOperation(uint32_t targetSessionId, uint32_t newOperationId,
                       const char* hardwareId, const char* name, uint32_t now);
    bool addCandidate(uint32_t targetSessionId, uint32_t sourceMainLocalId,
                      const char* hardwareId, uint32_t capabilities,
                      uint32_t now);
    bool applySessionResult(uint32_t targetSessionId,
                            CorePairingResultCode result, uint32_t now);
    bool applyOperationResult(uint32_t targetSessionId, uint32_t targetOperationId,
                              CorePairingResultCode result, uint32_t localDeviceId,
                              const char* hardwareId, uint32_t now);
    bool markCandidateLimit(uint32_t targetSessionId);
    void failSessionRequest(uint32_t targetSessionId, bool uncertain, uint32_t now);
    void failOperationRequest(uint32_t targetOperationId, uint32_t now);
    void markCommunicationLost(uint32_t now);
    void poll(uint32_t now);

    CorePairingSessionState getSessionState() const;
    uint32_t getSessionId() const;
    uint32_t getMainLocalId() const;
    uint16_t getDurationSeconds() const;
    bool hasCandidateLimit() const;
    uint8_t getCandidateCount() const;
    const CorePairingCandidate* getCandidate(uint8_t index) const;
    const CorePairingCandidate* findCandidate(const char* hardwareId) const;

    CorePairingOperationState getOperationState() const;
    uint32_t getOperationId() const;
    uint32_t getOperationSessionId() const;
    uint32_t getOperationDeviceId() const;
    CorePairingResultCode getOperationResult() const;
    const char* getOperationHardwareId() const;
    const char* getOperationName() const;
};

const char* corePairingSessionStateName(CorePairingSessionState state);
const char* corePairingOperationStateName(CorePairingOperationState state);
const char* corePairingResultCodeName(CorePairingResultCode result);
bool isValidCorePairingHardwareId(const char* hardwareId);
bool isValidCorePairingName(const char* name);
