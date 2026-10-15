#pragma once

#include "core_link_codec.h"
#include "message.h"
#include "provisioning.h"

constexpr int32_t CORE_PAIRING_CONTROL_MESSAGE = 0x43525001;
constexpr int32_t CORE_PAIRING_CANDIDATE_MESSAGE = 0x43525002;
constexpr int32_t CORE_PAIRING_SESSION_RESULT_MESSAGE = 0x43525003;
constexpr int32_t CORE_PAIRING_OPERATION_RESULT_MESSAGE = 0x43525004;
constexpr int32_t CORE_PAIRING_CANDIDATE_LIMIT_MESSAGE = 0x43525005;
constexpr int32_t CORE_REMOVAL_CONTROL_MESSAGE = 0x43525006;
constexpr int32_t CORE_REMOVAL_RESULT_MESSAGE = 0x43525007;

enum class CorePairingCommand : uint8_t {
    OPEN_SESSION = 1,
    CLOSE_SESSION = 2,
    PAIR_CANDIDATE = 3
};

enum class CoreRemovalCommand : uint8_t { REMOVE = 1, QUERY = 2 };

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

bool makeCoreRemovalControlMessage(uint32_t mainLocalId,
                                   uint32_t mainCoreId,
                                   CoreRemovalCommand command,
                                   uint32_t operationId,
                                   uint32_t lampLocalId,
                                   uint32_t lampCoreId,
                                   Message& message);
bool readCoreRemovalControlMessage(const Message& message,
                                  uint32_t expectedMainLocalId,
                                  uint32_t expectedMainCoreId,
                                  CoreRemovalCommand& command,
                                  uint32_t& operationId,
                                  uint32_t& lampLocalId,
                                  uint32_t& lampCoreId);
bool makeCoreRemovalResultMessage(uint32_t mainLocalId,
                                  uint32_t mainCoreId,
                                  uint32_t operationId,
                                  CorePairingResultCode result,
                                  uint32_t lampLocalId,
                                  const char* hardwareId,
                                  Message& message);
bool readCoreRemovalResultMessage(const Message& message,
                                  uint32_t expectedMainLocalId,
                                  uint32_t expectedMainCoreId,
                                  uint32_t& operationId,
                                  CorePairingResultCode& result,
                                  uint32_t& lampLocalId,
                                  char* hardwareId,
                                  size_t hardwareCapacity);
