#include <string.h>

#include "core_pairing_state.h"

bool isValidCorePairingHardwareId(const char* hardwareId) {
    if (hardwareId == nullptr || strlen(hardwareId) != 19 ||
        hardwareId[0] != 'C' || hardwareId[1] != '6' || hardwareId[2] != '-')
        return false;
    for (size_t index = 3; index < 19; ++index) {
        const char value = hardwareId[index];
        if (!((value >= '0' && value <= '9') ||
              (value >= 'A' && value <= 'F') ||
              (value >= 'a' && value <= 'f'))) return false;
    }
    return true;
}

bool isValidCorePairingName(const char* name) {
    if (name == nullptr) return false;
    const size_t length = strlen(name);
    if (length == 0 || length > 31) return false;
    for (size_t index = 0; index < length; ++index) {
        const char value = name[index];
        if (!((value >= 'A' && value <= 'Z') ||
              (value >= 'a' && value <= 'z') ||
              (value >= '0' && value <= '9') || value == ' ' ||
              value == '_' || value == '-' || value == '.' || value == '\''))
            return false;
    }
    return true;
}

namespace {
bool validText(const char* text, size_t capacity) {
    if (text == nullptr || text[0] == '\0') return false;
    size_t length = 0;
    while (length < capacity && text[length] != '\0') ++length;
    return length != 0 && length < capacity;
}

void copyText(char* destination, size_t capacity, const char* source) {
    const size_t length = strlen(source);
    memcpy(destination, source, length + 1);
    if (length + 1 < capacity) {
        memset(destination + length + 1, 0, capacity - length - 1);
    }
}

bool operationIsOpen(CorePairingOperationState state) {
    return state == CorePairingOperationState::PENDING ||
           state == CorePairingOperationState::PROCESSING;
}

bool sessionIsOpen(CorePairingSessionState state) {
    return state == CorePairingSessionState::OPENING ||
           state == CorePairingSessionState::ACTIVE ||
           state == CorePairingSessionState::CLOSING ||
           state == CorePairingSessionState::UNCERTAIN;
}
}  // namespace

CorePairingState::CorePairingState()
    : sessionState(CorePairingSessionState::NONE), sessionId(0), mainLocalId(0),
      durationSeconds(0), sessionStateAt(0), candidateLimitReached(false),
      candidates{}, candidateCount(0), operationState(CorePairingOperationState::NONE),
      operationId(0), operationSessionId(0), operationDeviceId(0),
      operationResult(CorePairingResultCode::NONE),
      operationStateAt(0), operationHardwareId{}, operationName{} {}

bool CorePairingState::beginSession(uint32_t newSessionId,
                                    uint32_t targetMainLocalId,
                                    uint16_t duration, uint32_t now) {
    if (newSessionId == 0 || targetMainLocalId == 0 || duration < 15 ||
        duration > 180 || sessionIsOpen(sessionState) ||
        operationIsOpen(operationState)) return false;
    sessionId = newSessionId;
    mainLocalId = targetMainLocalId;
    durationSeconds = duration;
    sessionState = CorePairingSessionState::OPENING;
    sessionStateAt = now;
    candidateLimitReached = false;
    candidateCount = 0;
    memset(candidates, 0, sizeof(candidates));
    operationState = CorePairingOperationState::NONE;
    operationId = 0;
    operationSessionId = 0;
    operationDeviceId = 0;
    operationResult = CorePairingResultCode::NONE;
    operationStateAt = 0;
    operationHardwareId[0] = '\0';
    operationName[0] = '\0';
    return true;
}

bool CorePairingState::beginClose(uint32_t targetSessionId, uint32_t now) {
    if (targetSessionId == 0 || targetSessionId != sessionId ||
        (sessionState != CorePairingSessionState::ACTIVE &&
         sessionState != CorePairingSessionState::UNCERTAIN &&
         sessionState != CorePairingSessionState::CLOSING) ||
        operationIsOpen(operationState)) return false;
    sessionState = CorePairingSessionState::CLOSING;
    sessionStateAt = now;
    return true;
}

bool CorePairingState::beginOperation(uint32_t targetSessionId,
                                      uint32_t newOperationId,
                                      const char* hardwareId, const char* name,
                                      uint32_t now) {
    if (sessionState != CorePairingSessionState::ACTIVE ||
        targetSessionId != sessionId || newOperationId == 0 ||
        operationIsOpen(operationState) || !validText(hardwareId, 32) ||
        !validText(name, 32) || !isValidCorePairingHardwareId(hardwareId) ||
        !isValidCorePairingName(name) || findCandidate(hardwareId) == nullptr)
        return false;
    operationId = newOperationId;
    operationSessionId = targetSessionId;
    operationDeviceId = 0;
    operationResult = CorePairingResultCode::NONE;
    operationState = CorePairingOperationState::PENDING;
    operationStateAt = now;
    copyText(operationHardwareId, sizeof(operationHardwareId), hardwareId);
    copyText(operationName, sizeof(operationName), name);
    return true;
}

bool CorePairingState::addCandidate(uint32_t targetSessionId,
                                    uint32_t sourceMainLocalId,
                                    const char* hardwareId,
                                    uint32_t capabilities, uint32_t now) {
    if (sessionState != CorePairingSessionState::ACTIVE ||
        targetSessionId != sessionId || sourceMainLocalId != mainLocalId ||
        !validText(hardwareId, 32) ||
        !isValidCorePairingHardwareId(hardwareId)) return false;
    for (uint8_t index = 0; index < candidateCount; ++index) {
        if (strcmp(candidates[index].hardwareId, hardwareId) == 0) {
            candidates[index].capabilities = capabilities;
            candidates[index].lastSeen = now;
            return true;
        }
    }
    if (candidateCount >= CORE_PAIRING_MAX_CANDIDATES) {
        candidateLimitReached = true;
        return false;
    }
    CorePairingCandidate& candidate = candidates[candidateCount++];
    memset(&candidate, 0, sizeof(candidate));
    copyText(candidate.hardwareId, sizeof(candidate.hardwareId), hardwareId);
    candidate.capabilities = capabilities;
    candidate.lastSeen = now;
    return true;
}

bool CorePairingState::applySessionResult(uint32_t targetSessionId,
                                          CorePairingResultCode result,
                                          uint32_t now) {
    if (targetSessionId == 0 || targetSessionId != sessionId) return false;
    switch (result) {
        case CorePairingResultCode::ACTIVE:
            if (sessionState != CorePairingSessionState::OPENING &&
                sessionState != CorePairingSessionState::ACTIVE) return false;
            sessionState = CorePairingSessionState::ACTIVE;
            sessionStateAt = now;
            return true;
        case CorePairingResultCode::CLOSED:
            if (sessionState != CorePairingSessionState::CLOSING &&
                sessionState != CorePairingSessionState::ACTIVE &&
                sessionState != CorePairingSessionState::UNCERTAIN) return false;
            sessionState = CorePairingSessionState::CLOSED;
            break;
        case CorePairingResultCode::EXPIRED:
            sessionState = CorePairingSessionState::EXPIRED;
            break;
        case CorePairingResultCode::FAILED:
        case CorePairingResultCode::BUSY:
            if (sessionState == CorePairingSessionState::CLOSING ||
                sessionState == CorePairingSessionState::UNCERTAIN) {
                sessionState = CorePairingSessionState::UNCERTAIN;
            } else {
                sessionState = CorePairingSessionState::FAILED;
            }
            sessionStateAt = now;
            return true;
        case CorePairingResultCode::UNCERTAIN:
            sessionState = CorePairingSessionState::UNCERTAIN;
            break;
        case CorePairingResultCode::CAPACITY:
            candidateLimitReached = true;
            return true;
        default:
            return false;
    }
    sessionStateAt = now;
    candidateCount = 0;
    memset(candidates, 0, sizeof(candidates));
    return true;
}

bool CorePairingState::applyOperationResult(uint32_t targetSessionId,
                                            uint32_t targetOperationId,
                                            CorePairingResultCode result,
                                            uint32_t localDeviceId,
                                            const char* hardwareId,
                                            uint32_t now) {
    if (targetSessionId != operationSessionId || targetOperationId == 0 ||
        targetOperationId != operationId || !operationIsOpen(operationState) ||
        (hardwareId != nullptr && hardwareId[0] != '\0' &&
         strcmp(hardwareId, operationHardwareId) != 0)) return false;
    switch (result) {
        case CorePairingResultCode::PROCESSING:
            operationState = CorePairingOperationState::PROCESSING;
            operationResult = result;
            operationStateAt = now;
            return true;
        case CorePairingResultCode::SUCCEEDED:
            if (localDeviceId == 0) return false;
            operationState = CorePairingOperationState::SUCCEEDED;
            operationResult = result;
            operationDeviceId = localDeviceId;
            break;
        case CorePairingResultCode::REJECTED:
            operationState = CorePairingOperationState::REJECTED;
            operationResult = result;
            break;
        case CorePairingResultCode::EXPIRED:
            operationState = CorePairingOperationState::EXPIRED;
            operationResult = result;
            break;
        case CorePairingResultCode::UNCERTAIN:
            operationState = CorePairingOperationState::UNCERTAIN;
            operationResult = result;
            break;
        case CorePairingResultCode::FAILED:
        case CorePairingResultCode::NOT_FOUND:
        case CorePairingResultCode::CAPACITY:
        case CorePairingResultCode::ROUTE_UNAVAILABLE:
        case CorePairingResultCode::BUSY:
            operationState = CorePairingOperationState::FAILED;
            operationResult = result;
            break;
        default:
            return false;
    }
    operationStateAt = now;
    if (operationState == CorePairingOperationState::SUCCEEDED) {
        for (uint8_t index = 0; index < candidateCount; ++index) {
            if (strcmp(candidates[index].hardwareId, operationHardwareId) != 0)
                continue;
            for (uint8_t move = index; move + 1 < candidateCount; ++move)
                candidates[move] = candidates[move + 1];
            --candidateCount;
            memset(&candidates[candidateCount], 0, sizeof(candidates[0]));
            break;
        }
    }
    return true;
}

bool CorePairingState::markCandidateLimit(uint32_t targetSessionId) {
    if (targetSessionId == 0 || targetSessionId != sessionId ||
        sessionState != CorePairingSessionState::ACTIVE) return false;
    candidateLimitReached = true;
    return true;
}

void CorePairingState::failSessionRequest(uint32_t targetSessionId,
                                          bool uncertain, uint32_t now) {
    if (targetSessionId != sessionId || targetSessionId == 0) return;
    sessionState = uncertain ? CorePairingSessionState::UNCERTAIN
                             : CorePairingSessionState::FAILED;
    sessionStateAt = now;
    candidateCount = 0;
}

void CorePairingState::failOperationRequest(uint32_t targetOperationId,
                                            uint32_t now) {
    if (targetOperationId != operationId || targetOperationId == 0) return;
    operationState = CorePairingOperationState::UNCERTAIN;
    operationResult = CorePairingResultCode::UNCERTAIN;
    operationStateAt = now;
}

void CorePairingState::markCommunicationLost(uint32_t now) {
    if (sessionIsOpen(sessionState)) {
        sessionState = CorePairingSessionState::UNCERTAIN;
        sessionStateAt = now;
        candidateCount = 0;
    }
    if (operationIsOpen(operationState)) {
        operationState = CorePairingOperationState::UNCERTAIN;
        operationResult = CorePairingResultCode::UNCERTAIN;
        operationStateAt = now;
    }
}

void CorePairingState::poll(uint32_t now) {
    for (uint8_t index = 0; index < candidateCount;) {
        if (now - candidates[index].lastSeen <= CORE_PAIRING_CANDIDATE_TTL_MS) {
            ++index;
            continue;
        }
        for (uint8_t move = index; move + 1 < candidateCount; ++move)
            candidates[move] = candidates[move + 1];
        --candidateCount;
        memset(&candidates[candidateCount], 0, sizeof(candidates[0]));
    }

    if (sessionState == CorePairingSessionState::OPENING &&
        now - sessionStateAt > CORE_PAIRING_RESPONSE_TIMEOUT_MS) {
        sessionState = CorePairingSessionState::UNCERTAIN;
        sessionStateAt = now;
    } else if (sessionState == CorePairingSessionState::CLOSING &&
               now - sessionStateAt > CORE_PAIRING_RESPONSE_TIMEOUT_MS) {
        sessionState = CorePairingSessionState::UNCERTAIN;
        sessionStateAt = now;
    } else if (sessionState == CorePairingSessionState::ACTIVE &&
               now - sessionStateAt >
                   static_cast<uint32_t>(durationSeconds) * 1000U +
                       CORE_PAIRING_RESPONSE_TIMEOUT_MS) {
        sessionState = CorePairingSessionState::UNCERTAIN;
        sessionStateAt = now;
    }
    if (operationIsOpen(operationState) &&
        now - operationStateAt > CORE_PAIRING_OPERATION_TIMEOUT_MS) {
        operationState = CorePairingOperationState::UNCERTAIN;
        operationStateAt = now;
    }
}

CorePairingSessionState CorePairingState::getSessionState() const { return sessionState; }
uint32_t CorePairingState::getSessionId() const { return sessionId; }
uint32_t CorePairingState::getMainLocalId() const { return mainLocalId; }
uint16_t CorePairingState::getDurationSeconds() const { return durationSeconds; }
bool CorePairingState::hasCandidateLimit() const { return candidateLimitReached; }
uint8_t CorePairingState::getCandidateCount() const { return candidateCount; }
const CorePairingCandidate* CorePairingState::getCandidate(uint8_t index) const {
    return index < candidateCount ? &candidates[index] : nullptr;
}
const CorePairingCandidate* CorePairingState::findCandidate(const char* hardwareId) const {
    if (hardwareId == nullptr) return nullptr;
    for (uint8_t index = 0; index < candidateCount; ++index) {
        if (strcmp(candidates[index].hardwareId, hardwareId) == 0)
            return &candidates[index];
    }
    return nullptr;
}
CorePairingOperationState CorePairingState::getOperationState() const {
    return operationState;
}
uint32_t CorePairingState::getOperationId() const { return operationId; }
uint32_t CorePairingState::getOperationSessionId() const { return operationSessionId; }
uint32_t CorePairingState::getOperationDeviceId() const { return operationDeviceId; }
CorePairingResultCode CorePairingState::getOperationResult() const {
    return operationResult;
}
const char* CorePairingState::getOperationHardwareId() const {
    return operationHardwareId;
}
const char* CorePairingState::getOperationName() const { return operationName; }

const char* corePairingSessionStateName(CorePairingSessionState state) {
    switch (state) {
        case CorePairingSessionState::NONE: return "closed";
        case CorePairingSessionState::OPENING: return "opening";
        case CorePairingSessionState::ACTIVE: return "active";
        case CorePairingSessionState::CLOSING: return "closing";
        case CorePairingSessionState::CLOSED: return "closed";
        case CorePairingSessionState::EXPIRED: return "expired";
        case CorePairingSessionState::FAILED: return "failed";
        case CorePairingSessionState::UNCERTAIN: return "uncertain";
    }
    return "uncertain";
}

const char* corePairingOperationStateName(CorePairingOperationState state) {
    switch (state) {
        case CorePairingOperationState::NONE: return "none";
        case CorePairingOperationState::PENDING: return "pending";
        case CorePairingOperationState::PROCESSING: return "processing";
        case CorePairingOperationState::SUCCEEDED: return "succeeded";
        case CorePairingOperationState::FAILED: return "failed";
        case CorePairingOperationState::REJECTED: return "rejected";
        case CorePairingOperationState::EXPIRED: return "expired";
        case CorePairingOperationState::UNCERTAIN: return "uncertain";
    }
    return "uncertain";
}

const char* corePairingResultCodeName(CorePairingResultCode result) {
    switch (result) {
        case CorePairingResultCode::NONE: return "none";
        case CorePairingResultCode::ACTIVE: return "active";
        case CorePairingResultCode::CLOSED: return "closed";
        case CorePairingResultCode::EXPIRED: return "expired";
        case CorePairingResultCode::FAILED: return "failed";
        case CorePairingResultCode::UNCERTAIN: return "uncertain";
        case CorePairingResultCode::BUSY: return "busy";
        case CorePairingResultCode::PROCESSING: return "processing";
        case CorePairingResultCode::SUCCEEDED: return "succeeded";
        case CorePairingResultCode::REJECTED: return "rejected";
        case CorePairingResultCode::CAPACITY: return "capacity";
        case CorePairingResultCode::NOT_FOUND: return "not_found";
        case CorePairingResultCode::ROUTE_UNAVAILABLE: return "route_unavailable";
    }
    return "failed";
}
