#pragma once

#include "core_link_codec.h"
#include "message.h"
#include "provisioning.h"

constexpr int32_t CORE_PAIRING_CONTROL_MESSAGE = 0x43525001;
constexpr int32_t CORE_PAIRING_CANDIDATE_MESSAGE = 0x43525002;
constexpr int32_t CORE_PAIRING_SESSION_RESULT_MESSAGE = 0x43525003;
constexpr int32_t CORE_PAIRING_OPERATION_RESULT_MESSAGE = 0x43525004;
constexpr int32_t CORE_PAIRING_CANDIDATE_LIMIT_MESSAGE = 0x43525005;

enum class CorePairingCommand : uint8_t {
    OPEN_SESSION = 1,
    CLOSE_SESSION = 2,
    PAIR_CANDIDATE = 3
};

bool makeCorePairingSessionCommand(
    uint32_t mainLocalId,
    uint32_t sessionId,
    bool open,
    uint16_t durationSeconds,
    Message& message
);

bool readCorePairingSessionCommand(
    const Message& message,
    uint32_t expectedMainLocalId,
    CorePairingCommand& command,
    uint32_t& sessionId,
    uint16_t& durationSeconds
);

bool makeCorePairingOperationCommand(
    uint32_t mainLocalId,
    uint32_t sessionId,
    uint32_t operationId,
    const char* hardwareId,
    const char* name,
    Message& message
);

bool readCorePairingOperationCommand(
    const Message& message,
    uint32_t expectedMainLocalId,
    uint32_t& sessionId,
    uint32_t& operationId
);

bool makeCorePairingCandidateMessage(
    uint32_t mainLocalId,
    uint32_t sessionId,
    const ProvisioningPacket& candidate,
    Message& message
);

bool readCorePairingCandidateMessage(
    const Message& message,
    uint32_t& mainLocalId,
    uint32_t& sessionId
);

bool makeCorePairingSessionResultMessage(
    uint32_t mainLocalId,
    uint32_t sessionId,
    CorePairingResultCode result,
    Message& message
);

bool readCorePairingSessionResultMessage(
    const Message& message,
    uint32_t expectedMainLocalId,
    uint32_t& sessionId,
    CorePairingResultCode& result
);

bool makeCorePairingOperationResultMessage(
    uint32_t mainLocalId,
    uint32_t sessionId,
    uint32_t operationId,
    CorePairingResultCode result,
    uint32_t deviceId,
    const char* hardwareId,
    Message& message
);

bool readCorePairingOperationResultMessage(
    const Message& message,
    uint32_t expectedMainLocalId,
    uint32_t& sessionId,
    uint32_t& operationId,
    CorePairingResultCode& result,
    uint32_t& deviceId
);

bool makeCorePairingCandidateLimitMessage(
    uint32_t mainLocalId,
    uint32_t sessionId,
    Message& message
);

bool readCorePairingCandidateLimitMessage(
    const Message& message,
    uint32_t expectedMainLocalId,
    uint32_t& sessionId
);
