#include <new>
#include <string.h>

#include <Arduino.h>

#include "action.h"
#include "device_manager.h"
#include "device_hardware_id.h"
#include "core_module_service.h"
#include "core_topology_protocol.h"
#include "core_command_protocol.h"
#include "message_id_generator.h"
#include "message_router.h"
#include "v5_provisioning_runtime.h"
#include "group_manager.h"
#include "lamp_controller.h"
#include "lamp_hardware.h"
#include "zigbee_transport.h"

namespace {
constexpr uint32_t GROUP_POWER_CHILD_TIMEOUT_MS = 5000;
constexpr uint32_t GROUP_POWER_CHILD_DEADLINE_MS =
    GROUP_POWER_CHILD_TIMEOUT_MS * (MAX_MESSAGE_RETRIES + 1U);
constexpr uint32_t GROUP_POWER_RESULT_RETENTION_MS = 2U * 60U * 1000U;
constexpr uint32_t GROUP_POWER_TOMBSTONE_RETENTION_MS = 10U * 60U * 1000U;
constexpr uint32_t GROUP_POWER_STATUS_REPLAY_MS = 5000;

uint32_t stableLogicalId(const char* hardwareId) {
    uint32_t hash = 2166136261UL;
    for (const unsigned char* p =
             reinterpret_cast<const unsigned char*>(hardwareId);
         *p != 0; ++p) {
        hash ^= *p;
        hash *= 16777619UL;
    }
    return hash == 0 ? 1 : hash;
}

void copyText(char* destination, size_t capacity, const char* source) {
    if (capacity == 0) {
        return;
    }
    if (source == nullptr) {
        destination[0] = '\0';
        return;
    }
    strncpy(destination, source, capacity - 1);
    destination[capacity - 1] = '\0';
}

bool isProvisioningMessage(MessageType type) {
    return type == MessageType::DEVICE_ANNOUNCE ||
           type == MessageType::PAIR_REQUEST ||
           type == MessageType::PAIR_ACCEPT ||
           type == MessageType::PAIR_CONFIRM ||
           type == MessageType::PAIR_REJECT ||
           type == MessageType::UNPAIR_REQUEST ||
           type == MessageType::UNPAIR_CONFIRM ||
           type == MessageType::UNPAIR_COMMIT ||
           type == MessageType::UNPAIR_REJECT;
}

bool sameText(const char* left, const char* right) {
    return left != nullptr && right != nullptr && strcmp(left, right) == 0;
}

bool sameGroupPowerRequest(const GroupProtocolPayload& left,
                           const GroupProtocolPayload& right) {
    return left.version == right.version &&
        left.action == GroupProtocolAction::SET_POWER &&
        (right.action == GroupProtocolAction::SET_POWER ||
         right.action == GroupProtocolAction::QUERY_OPERATION) &&
        left.operationId == right.operationId &&
        left.mainLocalId == right.mainLocalId &&
        left.mainCoreId == right.mainCoreId &&
        left.groupId == right.groupId && left.power == right.power;
}

bool groupPowerMemberTerminal(GroupProtocolResult result) {
    return result == GroupProtocolResult::EXECUTED ||
        result == GroupProtocolResult::FAILED ||
        result == GroupProtocolResult::OFFLINE ||
        result == GroupProtocolResult::UNSUPPORTED ||
        result == GroupProtocolResult::UNCERTAIN ||
        result == GroupProtocolResult::SUPERSEDED;
}

bool groupPowerOperationReusable(const MainGroupPowerOperation& operation,
                                 uint32_t now) {
    if (!operation.valid) return true;
    if (operation.active) return false;
    if (operation.result == GroupProtocolResult::UNCERTAIN) {
        // Unknown-operation tombstones have no child to await. Keep them for
        // the idempotency window; real uncertain fan-outs stay until resolved.
        return operation.memberCount == 0 &&
            now - operation.completedAt >= GROUP_POWER_TOMBSTONE_RETENTION_MS;
    }
    return now - operation.completedAt >= GROUP_POWER_RESULT_RETENTION_MS;
}

Message makeGroupPowerChild(const MainGroupPowerOperation& operation,
                            const MainGroupPowerMember& member) {
    Message command = {};
    command.id = member.childCommandId;
    command.sourceId = CORE_LOGICAL_ID;
    command.destinationId = member.destinationId;
    command.type = MessageType::COMMAND;
    command.timestamp = operation.startedAt;
    command.commandType = static_cast<int32_t>(ActionType::SET_LAMP_POWER);
    command.value = operation.request.power;
    command.status = MessageStatus::PENDING;
    command.executionStatus = ExecutionStatus::NOT_EXECUTED;
    command.parentMainId = operation.request.mainCoreId;
    command.provisioningDeviceId = member.localId;
    return command;
}

bool validGroupChildAck(const MainGroupPowerOperation& operation,
                        const MainGroupPowerMember& member,
                        const Message& ack) {
    if (ack.type != MessageType::ACK ||
        static_cast<uint32_t>(ack.value2) != member.childCommandId ||
        ack.destinationId != CORE_LOGICAL_ID ||
        ack.commandType != static_cast<int32_t>(ActionType::SET_LAMP_POWER) ||
        ack.parentMainId != operation.request.mainCoreId ||
        ack.provisioningDeviceId != member.localId) return false;
    if (ack.executionStatus == ExecutionStatus::NOT_EXECUTED)
        return ack.sourceId == operation.request.mainCoreId && ack.value == 0;
    if (ack.executionStatus == ExecutionStatus::EXECUTED)
        return ack.sourceId == member.destinationId &&
            ack.value == static_cast<int32_t>(ExecutionStatus::EXECUTED);
    if (ack.executionStatus == ExecutionStatus::FAILED)
        return (ack.sourceId == member.destinationId ||
                ack.sourceId == operation.request.mainCoreId) &&
            (ack.value == static_cast<int32_t>(ExecutionStatus::FAILED) ||
             (ack.value >= static_cast<int32_t>(CoreCommandError::UNKNOWN_DESTINATION) &&
              ack.value <= static_cast<int32_t>(CoreCommandError::COMMAND_IN_PROGRESS)));
    return false;
}

MainIndividualOutputGuard* findOutputGuard(
    MainIndividualOutputGuard (&guards)[MAX_PENDING_MESSAGES], uint32_t messageId) {
    for (uint8_t index = 0; index < MAX_PENDING_MESSAGES; ++index)
        if (guards[index].active && guards[index].messageId == messageId)
            return &guards[index];
    return nullptr;
}

MainIndividualOutputGuard* findLampOutputGuard(
    MainIndividualOutputGuard (&guards)[MAX_PENDING_MESSAGES], uint32_t localId) {
    for (uint8_t index = 0; index < MAX_PENDING_MESSAGES; ++index)
        if (guards[index].active && guards[index].localId == localId)
            return &guards[index];
    return nullptr;
}

MainIndividualOutputGuard* reserveOutputGuard(
    MainIndividualOutputGuard (&guards)[MAX_PENDING_MESSAGES],
    const Message& command, uint32_t fencesMessageId = 0) {
    for (uint8_t index = 0; index < MAX_PENDING_MESSAGES; ++index) {
        if (guards[index].active && guards[index].messageId == command.id)
            return &guards[index];
    }
    for (uint8_t index = 0; index < MAX_PENDING_MESSAGES; ++index) {
        if (guards[index].active) continue;
        guards[index] = {command.id, command.provisioningDeviceId,
            command.destinationId, millis(), fencesMessageId,
            command.commandType, command.value, true, false, false};
        return &guards[index];
    }
    return nullptr;
}

void clearOutputGuard(MainIndividualOutputGuard (&guards)[MAX_PENDING_MESSAGES],
                      uint32_t messageId) {
    if (messageId == 0) return;
    MainIndividualOutputGuard* guard = findOutputGuard(guards, messageId);
    if (guard != nullptr) *guard = {};
}

void makeUnpairReject(const ProvisioningPacket& request,
                      const ProvisioningPacket& identity,
                      PairingState state,
                      ProvisioningPacket& response) {
    response = {};
    response.type = ProvisioningPacketType::UNPAIR_REJECT;
    response.role = DeviceRole::LAMP;
    response.pairingState = state;
    response.deviceId = request.deviceId;
    response.parentMainId = request.parentMainId;
    response.transactionId = request.transactionId;
    copyText(response.hardwareId, sizeof(response.hardwareId),
             identity.hardwareId);
}
}  // namespace

V5ProvisioningRuntime::V5ProvisioningRuntime(
    Communication& communicationValue,
    LampRegistry& lampsValue,
    MessageTracker& trackerValue,
    MessageDeduplicator& deduplicatorValue
)
    : communication(communicationValue),
      lamps(lampsValue),
      tracker(trackerValue),
      deduplicator(deduplicatorValue),
      role(DeviceRole::LAMP),
      lampStore(nullptr),
      reservationStore(nullptr),
      groupStore(nullptr),
      groupRegistry(nullptr),
      lampPairing(nullptr),
      mainPairing(nullptr),
      mainLogicalId(0),
      nextAnnounceAt(0),
      activePairingSessionId(0),
      lastClosedPairingSessionId(0),
      pairingSessionStartedAt(0),
      pairingSessionDurationSeconds(0),
      pairingSessionExpired(false),
      nextPairingCloseAttemptAt(0),
      activePairingOperationId(0),
      activePairingOperationSessionId(0),
      pairingOperationStartedAt(0), groupReadIndex(0),
      groupReadMemberOffset(0), groupReadPhase(0), groupReadActive(false),
      groupReadIsList(false), nextGroupPowerOperationSlot(0) {
    memset(groupPowerOperations, 0, sizeof(groupPowerOperations));
    memset(individualOutputGuards, 0, sizeof(individualOutputGuards));
    hardwareId[0] = '\0';
    activePairingHardwareId[0] = '\0';
    nextRemovalRetryAt = 0;
    lastRemovalContactAt = 0;
}

bool V5ProvisioningRuntime::begin(
    DeviceRole roleValue,
    ProvisioningStore* lampStoreValue,
    ProvisioningReservationStore* reservationStoreValue,
    GroupStore* groupStoreValue,
    GroupRegistry* groupRegistryValue
) {
    role = roleValue;
    lampStore = lampStoreValue;
    reservationStore = reservationStoreValue;
    groupStore = groupStoreValue;
    groupRegistry = groupRegistryValue;
    if (!readDeviceHardwareId(hardwareId, sizeof(hardwareId))) {
        Serial.println("[IDENTITY] failed to read IEEE 802.15.4 hardware ID");
        return false;
    }

    mainLogicalId = stableLogicalId(hardwareId);
    if (role == DeviceRole::MAIN) {
        mainPairing = new (std::nothrow)
            MainProvisioning(mainLogicalId, lamps, reservationStore,
                             groupRegistryValue, groupStoreValue);
        if (mainPairing == nullptr) {
            return false;
        }
        if (groupRegistryValue != nullptr && groupStoreValue != nullptr) {
            const GroupStoreLoadResult groupLoad = restoreGroupRegistry(
                *groupRegistryValue, mainLogicalId, *groupStoreValue);
            if (groupLoad == GroupStoreLoadResult::FOUND) {
                Serial.print("[GROUPS] restored groups=");
                Serial.println(groupRegistryValue->count);
            } else if (groupLoad == GroupStoreLoadResult::NOT_FOUND) {
                Serial.println("[GROUPS] no saved groups; starting empty");
            } else {
                Serial.print("[GROUPS] storage unavailable/corrupt, writes blocked status=");
                Serial.println(static_cast<uint8_t>(groupLoad));
            }
        }
        Serial.print("[IDENTITY] MAIN hardwareId=");
        Serial.print(hardwareId);
        Serial.print(" logicalId=");
        Serial.println(mainLogicalId);
    } else if (role == DeviceRole::LAMP && lampStore != nullptr) {
        const uint32_t capabilities =
            static_cast<uint32_t>(DEVICE_CAP_POWER) |
            (lampHardwareSupportsBrightness()
                ? static_cast<uint32_t>(DEVICE_CAP_BRIGHTNESS) : 0U);
        lampPairing = new (std::nothrow)
            ProvisioningLamp(hardwareId, "5.1.0", capabilities, *lampStore);
        if (lampPairing == nullptr) {
            return false;
        }
        Serial.print("[IDENTITY] LAMP hardwareId=");
        Serial.println(hardwareId);
        restoreLocalLamp();
    } else {
        Serial.println("[V5] this commissioning runtime supports MAIN and LAMP only");
        return false;
    }

    registerApplicationMessageHandler(handleMessage, this);
    return true;
}

void V5ProvisioningRuntime::poll() {
    const uint32_t now = millis();
    if (role == DeviceRole::MAIN) {
        pollGroupRead();
        pollGroupPowerOperations(now);
    }
    if (role == DeviceRole::MAIN && mainPairing != nullptr) {
        mainPairing->expirePendingPairing(now);
        mainPairing->expireUnpairedCandidates(now);
        if (activePairingOperationId != 0 &&
            now - pairingOperationStartedAt >=
                PROVISIONING_TRANSACTION_TIMEOUT_MS) {
            sendPairingOperationResult(activePairingOperationSessionId,
                activePairingOperationId, CorePairingResultCode::EXPIRED, 0,
                activePairingHardwareId);
            clearPairingOperation();
        }
        if (activePairingSessionId != 0 && !pairingSessionExpired &&
            now - pairingSessionStartedAt >=
                static_cast<uint32_t>(pairingSessionDurationSeconds) * 1000U) {
            pairingSessionExpired = true;
            mainPairing->stopCommissioning();
            mainPairing->clearUnpairedCandidates();
            nextPairingCloseAttemptAt = now;
        }
        if (activePairingSessionId != 0 && pairingSessionExpired &&
            static_cast<int32_t>(now - nextPairingCloseAttemptAt) >= 0) {
            const uint32_t expiredSession = activePairingSessionId;
            if (closeZigbeeJoinWindow()) {
                activePairingSessionId = 0;
                lastClosedPairingSessionId = expiredSession;
                pairingSessionDurationSeconds = 0;
                pairingSessionExpired = false;
                nextPairingCloseAttemptAt = 0;
                sendPairingSessionResult(expiredSession, CorePairingResultCode::EXPIRED);
            } else {
                nextPairingCloseAttemptAt = now + 5000;
                sendPairingSessionResult(expiredSession, CorePairingResultCode::UNCERTAIN);
            }
        }
        if (mainPairing->hasActiveRemoval() &&
            static_cast<int32_t>(now - nextRemovalRetryAt) >= 0) {
            ProvisioningPacket request = {};
            if (mainPairing->buildRemovalPacket(request)) {
                (void)sendPacket(request, mainLogicalId, request.deviceId);
            }
            nextRemovalRetryAt = now + 5000;
        }
        return;
    }
    if (role != DeviceRole::LAMP || lampPairing == nullptr) {
        return;
    }
    lampPairing->expirePendingRequest(now);

    const ProvisioningPacket announcement = lampPairing->announce();
    if (announcement.pairingState == PairingState::PAIRED &&
        announcement.deviceId != 0) {
        // The local LAMP registry entry is not refreshed by receiving its own traffic.
        Lamp* localLamp = findLampByHardwareId(lamps, announcement.hardwareId);
        if (localLamp != nullptr &&
            localLamp->identity.parentMainId == announcement.parentMainId) {
            updateDeviceSeen(localLamp->device);
        }
    }

    if (!communicationReady(communication)) {
        return;
    }

    if (static_cast<int32_t>(now - nextAnnounceAt) < 0) {
        return;
    }
    const uint32_t sourceId = announcement.deviceId;
    if (sendPacket(announcement, sourceId, 0)) {
        nextAnnounceAt = now + 5000;
        if (announcement.pairingState == PairingState::UNPAIRING) {
            ProvisioningPacket confirmation = {};
            if (lampPairing->retryPreparedRemoval(confirmation)) {
                (void)sendPacket(confirmation, confirmation.deviceId,
                                 confirmation.parentMainId);
            }
        }
    } else {
        nextAnnounceAt = now + 1000;
    }
}

bool V5ProvisioningRuntime::commissionLamp(
    const char* hardwareIdValue,
    const char* requestedName
) {
    if (role != DeviceRole::MAIN || mainPairing == nullptr ||
        !communicationReady(communication)) {
        return false;
    }

    const bool ownsCommissioningWindow =
        !mainPairing->commissioningIsEnabled();
    ProvisioningPacket request = {};
    if (ownsCommissioningWindow) mainPairing->startCommissioning();
    const bool requestCreated =
        mainPairing->createPairRequest(hardwareIdValue, request);
    if (!requestCreated) {
        if (ownsCommissioningWindow) mainPairing->stopCommissioning();
        return false;
    }
    const bool requestSent = sendPacket(request, mainLogicalId, 0);
    if (!requestSent) {
        mainPairing->abortPendingPairing();
        if (ownsCommissioningWindow) mainPairing->stopCommissioning();
        return false;
    }

    ProvisioningPacket acceptance = {};
    const bool acceptanceCreated = mainPairing->createPairAcceptance(
        hardwareIdValue, requestedName, acceptance);
    if (!acceptanceCreated) {
        mainPairing->abortPendingPairing();
        if (ownsCommissioningWindow) mainPairing->stopCommissioning();
        return false;
    }
    const bool acceptanceSent = sendPacket(acceptance, mainLogicalId, 0);
    if (!acceptanceSent) {
        mainPairing->abortPendingPairing();
    }
    if (ownsCommissioningWindow) mainPairing->stopCommissioning();
    return acceptanceSent;
}

void V5ProvisioningRuntime::sendPairingSessionResult(
    uint32_t sessionId,
    CorePairingResultCode result
) {
    if (role != DeviceRole::MAIN || sessionId == 0) return;
    Message response = {};
    if (makeCorePairingSessionResultMessage(mainLogicalId, sessionId,
                                            result, response)) {
        response.timestamp = millis();
        (void)sendMessage(communication, response);
    }
}

void V5ProvisioningRuntime::sendPairingOperationResult(
    uint32_t sessionId,
    uint32_t operationId,
    CorePairingResultCode result,
    uint32_t deviceId,
    const char* hardwareIdValue
) {
    if (role != DeviceRole::MAIN || sessionId == 0 || operationId == 0) return;
    Message response = {};
    if (makeCorePairingOperationResultMessage(mainLogicalId, sessionId,
            operationId, result, deviceId, hardwareIdValue, response)) {
        response.timestamp = millis();
        (void)sendMessage(communication, response);
    }
}

void V5ProvisioningRuntime::sendRemovalResult(
    uint32_t operationId,
    CorePairingResultCode result,
    const MainProvisioningReservationState& state
) {
    if (role != DeviceRole::MAIN || operationId == 0) return;
    const uint32_t mainCoreId = makeCoreModuleId(CORE_LOGICAL_ID, mainLogicalId);
    Message response = {};
    const bool hasIdentity = state.removalOperationId == operationId &&
        state.removalDeviceId != 0 && state.removalHardwareId[0] != '\0';
    if (makeCoreRemovalResultMessage(mainLogicalId, mainCoreId, operationId,
            result, hasIdentity ? state.removalDeviceId : 0,
            hasIdentity ? state.removalHardwareId : nullptr, response)) {
        response.timestamp = millis();
        (void)sendMessage(communication, response);
    }
}

void V5ProvisioningRuntime::announceAcceptedLamp(
    const ProvisioningPacket& lamp
) {
    if (role != DeviceRole::MAIN) return;
    ProvisioningPacket announcementLamp = lamp;
    Lamp* registered = findLamp(lamps, lamp.deviceId);
    if (registered != nullptr &&
        registered->identity.parentMainId == mainLogicalId) {
        announcementLamp.capabilities = registered->device.capabilities;
    }
    Message announcement = {};
    if (!makeCoreLampAnnouncementFromMain(mainLogicalId, announcementLamp,
                                          announcement)) return;
    announcement.id = generateMessageId();
    announcement.timestamp = millis();
    (void)sendMessage(communication, announcement);
}

void V5ProvisioningRuntime::processRemovalEvidence(
    const ProvisioningPacket& packet
) {
    if (mainPairing == nullptr) return;
    ProvisioningPacket response = {};
    bool completed = false;
    if (!mainPairing->reconcileRemovalPacket(packet, response, completed)) return;
    lastRemovalContactAt = millis();
    MainProvisioningReservationState state = {};
    const uint32_t operationId = packet.transactionId;
    if (!mainPairing->getRemovalStatus(operationId, state)) return;
    if (state.removalState == MAIN_REMOVAL_FAILED) {
        sendRemovalResult(operationId, CorePairingResultCode::FAILED, state);
        return;
    }
    if (state.removalState != MAIN_REMOVAL_SUCCEEDED) return;

    const bool commitSent = sendPacket(response, mainLogicalId, response.deviceId);
    if (commitSent) {
        (void)forgetZigbeeRoute(state.removalDeviceId,
            state.removalHardwareId, CommunicationRouteScope::LOCAL_DEVICE);
    }
    sendRemovalResult(operationId, CorePairingResultCode::SUCCEEDED, state);
    nextRemovalRetryAt = 0;
}

bool V5ProvisioningRuntime::handleCoreRemovalControl(const Message& message) {
    if (role != DeviceRole::MAIN || mainPairing == nullptr ||
        message.type != MessageType::STATE ||
        message.commandType != CORE_REMOVAL_CONTROL_MESSAGE) return false;
    const uint32_t mainCoreId = makeCoreModuleId(CORE_LOGICAL_ID, mainLogicalId);
    CoreRemovalCommand command = CoreRemovalCommand::QUERY;
    uint32_t operationId = 0;
    uint32_t lampLocalId = 0;
    uint32_t lampCoreId = 0;
    if (!readCoreRemovalControlMessage(message, mainLogicalId, mainCoreId,
            command, operationId, lampLocalId, lampCoreId)) return true;

    MainProvisioningReservationState state = {};
    if (command == CoreRemovalCommand::QUERY) {
        if (!mainPairing->getRemovalStatus(operationId, state)) {
            sendRemovalResult(operationId, CorePairingResultCode::NOT_FOUND, state);
            return true;
        }
        CorePairingResultCode result = CorePairingResultCode::FAILED;
        if (state.removalState == MAIN_REMOVAL_REQUESTED) {
            result = lastRemovalContactAt != 0 &&
                     millis() - lastRemovalContactAt <
                         PROVISIONING_TRANSACTION_TIMEOUT_MS
                ? CorePairingResultCode::PROCESSING
                : CorePairingResultCode::UNCERTAIN;
        } else if (state.removalState == MAIN_REMOVAL_SUCCEEDED) {
            result = CorePairingResultCode::SUCCEEDED;
        } else if (state.removalState == MAIN_REMOVAL_FAILED) {
            result = CorePairingResultCode::FAILED;
        }
        sendRemovalResult(operationId, result, state);
        return true;
    }

    if (hasActiveGroupPowerOperation()) {
        state.removalOperationId = operationId;
        state.removalDeviceId = lampLocalId;
        state.removalParentMainId = mainLogicalId;
        state.removalCoreDeviceId = lampCoreId;
        Lamp* target = findLamp(lamps, lampLocalId);
        if (target != nullptr) {
            copyText(state.removalHardwareId, sizeof(state.removalHardwareId),
                     target->identity.hardwareId);
        }
        sendRemovalResult(operationId, CorePairingResultCode::BUSY, state);
        return true;
    }

    if (mainPairing->getRemovalStatus(operationId, state)) {
        if (state.removalDeviceId != lampLocalId ||
            state.removalCoreDeviceId != lampCoreId) {
            sendRemovalResult(operationId, CorePairingResultCode::BUSY, state);
            return true;
        }
        if (state.removalState == MAIN_REMOVAL_SUCCEEDED) {
            sendRemovalResult(operationId, CorePairingResultCode::SUCCEEDED, state);
            return true;
        }
        if (state.removalState == MAIN_REMOVAL_FAILED) {
            sendRemovalResult(operationId, CorePairingResultCode::FAILED, state);
            return true;
        }
    } else if (mainPairing->hasActiveRemoval()) {
        Lamp* target = findLamp(lamps, lampLocalId);
        state.removalOperationId = operationId;
        state.removalDeviceId = lampLocalId;
        state.removalParentMainId = mainLogicalId;
        state.removalCoreDeviceId = lampCoreId;
        if (target != nullptr)
            copyText(state.removalHardwareId, sizeof(state.removalHardwareId),
                     target->identity.hardwareId);
        sendRemovalResult(operationId, CorePairingResultCode::BUSY, state);
        return true;
    } else {
        Lamp* target = findLamp(lamps, lampLocalId);
        if (target == nullptr || activePairingOperationId != 0 ||
            !mainPairing->beginRemoval(operationId, lampLocalId, lampCoreId)) {
            MainProvisioningReservationState failed = {};
            failed.removalOperationId = operationId;
            failed.removalDeviceId = lampLocalId;
            failed.removalParentMainId = mainLogicalId;
            failed.removalCoreDeviceId = lampCoreId;
            if (target != nullptr)
                copyText(failed.removalHardwareId,
                         sizeof(failed.removalHardwareId),
                         target->identity.hardwareId);
            sendRemovalResult(operationId,
                target == nullptr ? CorePairingResultCode::NOT_FOUND
                                  : CorePairingResultCode::FAILED,
                failed);
            return true;
        }
        (void)mainPairing->getRemovalStatus(operationId, state);
        lastRemovalContactAt = millis();
        nextRemovalRetryAt = 0;
    }

    ProvisioningPacket request = {};
    const bool sent = mainPairing->buildRemovalPacket(request) &&
        request.transactionId == operationId &&
        sendPacket(request, mainLogicalId, request.deviceId);
    nextRemovalRetryAt = millis() + 5000;
    sendRemovalResult(operationId, sent ? CorePairingResultCode::PROCESSING
                                        : CorePairingResultCode::UNCERTAIN,
                      state);
    return true;
}

void V5ProvisioningRuntime::clearPairingOperation() {
    activePairingOperationId = 0;
    activePairingOperationSessionId = 0;
    pairingOperationStartedAt = 0;
    activePairingHardwareId[0] = '\0';
}

bool V5ProvisioningRuntime::handleCorePairingControl(const Message& message) {
    if (role != DeviceRole::MAIN || mainPairing == nullptr ||
        message.type != MessageType::STATE ||
        message.commandType != CORE_PAIRING_CONTROL_MESSAGE) return false;

    CorePairingCommand sessionCommand = CorePairingCommand::OPEN_SESSION;
    uint32_t sessionId = 0;
    uint16_t duration = 0;
    if (readCorePairingSessionCommand(message, mainLogicalId,
            sessionCommand, sessionId, duration)) {
        if (sessionCommand == CorePairingCommand::OPEN_SESSION) {
            if (activePairingSessionId == sessionId) {
                sendPairingSessionResult(sessionId, pairingSessionExpired
                    ? CorePairingResultCode::UNCERTAIN
                    : CorePairingResultCode::ACTIVE);
            } else if (activePairingSessionId != 0) {
                sendPairingSessionResult(sessionId, CorePairingResultCode::BUSY);
            } else if (!openZigbeeJoinWindow(static_cast<uint8_t>(duration))) {
                sendPairingSessionResult(sessionId, CorePairingResultCode::UNCERTAIN);
            } else {
                activePairingSessionId = sessionId;
                pairingSessionStartedAt = millis();
                pairingSessionDurationSeconds = duration;
                mainPairing->startCommissioning();
                sendPairingSessionResult(sessionId, CorePairingResultCode::ACTIVE);
                Serial.println("[PAIRING] radio admission window opened");
            }
            return true;
        }

        if (activePairingSessionId != sessionId) {
            if (activePairingSessionId != 0) {
                sendPairingSessionResult(sessionId, CorePairingResultCode::BUSY);
            } else if (lastClosedPairingSessionId == sessionId) {
                sendPairingSessionResult(sessionId, CorePairingResultCode::CLOSED);
            } else if (closeZigbeeJoinWindow()) {
                lastClosedPairingSessionId = sessionId;
                mainPairing->stopCommissioning();
                mainPairing->clearUnpairedCandidates();
                sendPairingSessionResult(sessionId, CorePairingResultCode::CLOSED);
            } else {
                sendPairingSessionResult(sessionId, CorePairingResultCode::UNCERTAIN);
            }
        } else if (activePairingOperationId != 0) {
            sendPairingSessionResult(sessionId, CorePairingResultCode::BUSY);
        } else if (!closeZigbeeJoinWindow()) {
            if (pairingSessionExpired) nextPairingCloseAttemptAt = millis() + 5000;
            sendPairingSessionResult(sessionId, CorePairingResultCode::UNCERTAIN);
        } else {
            mainPairing->stopCommissioning();
            mainPairing->clearUnpairedCandidates();
            activePairingSessionId = 0;
            lastClosedPairingSessionId = sessionId;
            pairingSessionDurationSeconds = 0;
            pairingSessionExpired = false;
            nextPairingCloseAttemptAt = 0;
            sendPairingSessionResult(sessionId, CorePairingResultCode::CLOSED);
            Serial.println("[PAIRING] radio admission window closed");
        }
        return true;
    }

    uint32_t operationSessionId = 0;
    uint32_t operationId = 0;
    if (!readCorePairingOperationCommand(message, mainLogicalId,
            operationSessionId, operationId)) {
        sendPairingOperationResult(message.parentMainId, message.id,
            CorePairingResultCode::FAILED, 0, message.hardwareId);
        return true;
    }
    if (operationSessionId != activePairingSessionId ||
        activePairingSessionId == 0 || pairingSessionExpired) {
        sendPairingOperationResult(operationSessionId, operationId,
            CorePairingResultCode::NOT_FOUND, 0, message.hardwareId);
        return true;
    }
    if (activePairingOperationId != 0 || hasActiveGroupPowerOperation() ||
        !mainPairing->hasUnpairedCandidate(message.hardwareId)) {
        sendPairingOperationResult(operationSessionId, operationId,
            (activePairingOperationId != 0 || hasActiveGroupPowerOperation())
                ? CorePairingResultCode::BUSY : CorePairingResultCode::NOT_FOUND,
            0, message.hardwareId);
        return true;
    }
    if (!mainPairing->canPairCandidate(message.hardwareId)) {
        sendPairingOperationResult(operationSessionId, operationId,
            mainPairing->pairingCapacityReached(message.hardwareId)
                ? CorePairingResultCode::CAPACITY
                : CorePairingResultCode::FAILED,
            0, message.hardwareId);
        return true;
    }
    if (!hasZigbeeRouteForHardwareId(message.hardwareId)) {
        sendPairingOperationResult(operationSessionId, operationId,
            CorePairingResultCode::ROUTE_UNAVAILABLE, 0, message.hardwareId);
        return true;
    }

    activePairingOperationId = operationId;
    activePairingOperationSessionId = operationSessionId;
    pairingOperationStartedAt = millis();
    strncpy(activePairingHardwareId, message.hardwareId,
            sizeof(activePairingHardwareId) - 1);
    activePairingHardwareId[sizeof(activePairingHardwareId) - 1] = '\0';
    if (!commissionLamp(message.hardwareId, message.name)) {
        sendPairingOperationResult(operationSessionId, operationId,
            CorePairingResultCode::FAILED, 0, message.hardwareId);
        clearPairingOperation();
    } else {
        sendPairingOperationResult(operationSessionId, operationId,
            CorePairingResultCode::PROCESSING, 0, message.hardwareId);
    }
    return true;
}

bool V5ProvisioningRuntime::setPower(uint32_t deviceId, bool enabled) {
    if (role != DeviceRole::MAIN || deviceId == 0 ||
        !communicationReady(communication)) {
        return false;
    }

    Message command = {};
    command.id = generateMessageId();
    command.sourceId = mainLogicalId;
    command.destinationId = deviceId;
    command.type = MessageType::COMMAND;
    command.timestamp = millis();
    command.commandType = static_cast<int32_t>(ActionType::SET_LAMP_POWER);
    command.value = enabled ? 1 : 0;
    command.status = MessageStatus::PENDING;
    command.executionStatus = ExecutionStatus::NOT_EXECUTED;

    if (!trackMessage(tracker, command)) {
        return false;
    }
    if (!sendMessage(communication, command)) {
        untrackMessage(tracker, command.id);
        return false;
    }
    return true;
}

uint32_t V5ProvisioningRuntime::mainId() const {
    return mainLogicalId;
}

const char* V5ProvisioningRuntime::localHardwareId() const {
    return hardwareId;
}

bool V5ProvisioningRuntime::sendGroupResponse(
    const GroupProtocolPayload& group
) {
    Message response = {};
    response.type = MessageType::GROUP_CONTROL;
    response.id = group.operationId;
    response.sourceId = mainLogicalId;
    response.destinationId = CORE_LOGICAL_ID;
    response.parentMainId = makeCoreModuleId(CORE_LOGICAL_ID, mainLogicalId);
    response.status = MessageStatus::SENT;
    response.executionStatus = ExecutionStatus::NOT_EXECUTED;
    response.group = group;
    return sendMessage(communication, response);
}

bool V5ProvisioningRuntime::hasActiveGroupPowerOperation() const {
    for (uint8_t index = 0; index < MAIN_GROUP_POWER_OPERATION_SLOTS; ++index)
        if (groupPowerOperations[index].valid &&
            groupPowerOperations[index].active) return true;
    return false;
}

void V5ProvisioningRuntime::sendGroupPowerMemberResult(
    MainGroupPowerOperation& operation, uint8_t memberIndex
) {
    if (memberIndex >= operation.memberCount) return;
    const MainGroupPowerMember& member = operation.members[memberIndex];
    GroupProtocolPayload update = {};
    update.version = GROUP_POWER_PROTOCOL_VERSION;
    update.action = GroupProtocolAction::MEMBER_RESULT;
    update.result = member.result;
    update.operationId = operation.id;
    update.mainLocalId = mainLogicalId;
    update.mainCoreId = makeCoreModuleId(CORE_LOGICAL_ID, mainLogicalId);
    update.groupId = operation.request.groupId;
    update.memberLocalId = member.localId;
    update.childCommandId = member.childCommandId;
    update.groupCount = operation.memberCount;
    update.memberOffset = memberIndex;
    update.power = operation.request.power;
    (void)sendGroupResponse(update);
}

void V5ProvisioningRuntime::finishGroupPowerIfReady(
    MainGroupPowerOperation& operation, uint32_t now
) {
    if (!operation.valid ||
        (!operation.active && operation.result != GroupProtocolResult::UNCERTAIN))
        return;
    bool allTerminal = true;
    bool anyExecuted = false;
    bool anyFailure = false;
    bool anyUncertain = false;
    uint8_t completedCount = 0;
    for (uint8_t index = 0; index < operation.memberCount; ++index) {
        const GroupProtocolResult result = operation.members[index].result;
        const bool terminal = groupPowerMemberTerminal(result);
        if (!terminal) allTerminal = false;
        else ++completedCount;
        if (result == GroupProtocolResult::EXECUTED) anyExecuted = true;
        if (result == GroupProtocolResult::FAILED ||
            result == GroupProtocolResult::OFFLINE ||
            result == GroupProtocolResult::UNSUPPORTED) anyFailure = true;
        if (result == GroupProtocolResult::UNCERTAIN ||
            result == GroupProtocolResult::SUPERSEDED) anyUncertain = true;
    }
    if (!allTerminal) return;

    GroupProtocolResult aggregate = GroupProtocolResult::COMPLETED;
    if (anyUncertain) aggregate = GroupProtocolResult::UNCERTAIN;
    else if (anyFailure && anyExecuted) aggregate = GroupProtocolResult::PARTIAL;
    else if (anyFailure) aggregate = GroupProtocolResult::FAILED;

    if (!operation.active && aggregate == GroupProtocolResult::UNCERTAIN) return;
    if (!operation.active && operation.result == aggregate) return;
    operation.result = aggregate;
    operation.active = false;
    operation.completedAt = now;
    GroupProtocolPayload response = {};
    response.version = GROUP_POWER_PROTOCOL_VERSION;
    response.action = GroupProtocolAction::RESULT;
    response.result = aggregate;
    response.operationId = operation.id;
    response.mainLocalId = mainLogicalId;
    response.mainCoreId = makeCoreModuleId(CORE_LOGICAL_ID, mainLogicalId);
    response.groupId = operation.request.groupId;
    response.groupCount = operation.memberCount;
    response.memberCount = completedCount;
    response.power = operation.request.power;
    (void)sendGroupResponse(response);
}

bool V5ProvisioningRuntime::handleGroupPowerRequest(const Message& message) {
    const GroupProtocolPayload& request = message.group;
    const bool statusQuery =
        request.action == GroupProtocolAction::QUERY_OPERATION;
    GroupProtocolPayload response = {};
    response.version = GROUP_POWER_PROTOCOL_VERSION;
    response.action = GroupProtocolAction::RESULT;
    response.operationId = request.operationId;
    response.mainLocalId = mainLogicalId;
    response.mainCoreId = makeCoreModuleId(CORE_LOGICAL_ID, mainLogicalId);
    response.groupId = request.groupId;
    response.power = request.power;

    auto reject = [&](GroupProtocolResult result) {
        response.result = result;
        response.groupCount = 0;
        response.memberCount = 0;
        (void)sendGroupResponse(response);
        return true;
    };

    if (!isValidGroupProtocolPayload(request, GROUP_ZIGBEE_MEMBER_CHUNK) ||
        request.version != GROUP_POWER_PROTOCOL_VERSION ||
        (request.action != GroupProtocolAction::SET_POWER && !statusQuery) ||
        request.result != GroupProtocolResult::NONE ||
        message.sourceId != CORE_LOGICAL_ID ||
        message.destinationId != mainLogicalId ||
        message.parentMainId != request.mainCoreId) {
        return reject(GroupProtocolResult::INVALID);
    }

    for (uint8_t index = 0; index < MAIN_GROUP_POWER_OPERATION_SLOTS; ++index) {
        MainGroupPowerOperation& existing = groupPowerOperations[index];
        if (!existing.valid || existing.id != request.operationId) continue;
        if (!sameGroupPowerRequest(existing.request, request))
            return reject(GroupProtocolResult::INVALID);

        GroupProtocolPayload snapshot = {};
        snapshot.version = GROUP_POWER_PROTOCOL_VERSION;
        snapshot.action = GroupProtocolAction::RESULT;
        snapshot.result = existing.active ? GroupProtocolResult::ACCEPTED
                                          : existing.result;
        snapshot.operationId = existing.id;
        snapshot.mainLocalId = mainLogicalId;
        snapshot.mainCoreId = makeCoreModuleId(CORE_LOGICAL_ID, mainLogicalId);
        snapshot.groupId = existing.request.groupId;
        snapshot.groupCount = existing.memberCount;
        snapshot.power = existing.request.power;
        for (uint8_t memberIndex = 0; memberIndex < existing.memberCount;
             ++memberIndex) {
            if (groupPowerMemberTerminal(existing.members[memberIndex].result))
                ++snapshot.memberCount;
        }
        (void)sendGroupResponse(snapshot);
        for (uint8_t memberIndex = 0; memberIndex < existing.memberCount;
             ++memberIndex) sendGroupPowerMemberResult(existing, memberIndex);
        return true;
    }

    // A status query must never create an operation or fan commands out. Keep
    // a bounded uncertain tombstone so a duplicate SET_POWER with this ID cannot
    // execute during this boot after the original history was lost.
    if (statusQuery) {
        const uint32_t now = millis();
        int tombstoneSlot = -1;
        for (uint8_t attempt = 0;
             attempt < MAIN_GROUP_POWER_OPERATION_SLOTS; ++attempt) {
            const uint8_t candidate = static_cast<uint8_t>(
                (nextGroupPowerOperationSlot + attempt) %
                MAIN_GROUP_POWER_OPERATION_SLOTS);
            const MainGroupPowerOperation& current =
                groupPowerOperations[candidate];
            if (groupPowerOperationReusable(current, now)) {
                tombstoneSlot = candidate;
                nextGroupPowerOperationSlot = static_cast<uint8_t>(
                    (candidate + 1) % MAIN_GROUP_POWER_OPERATION_SLOTS);
                break;
            }
        }
        if (tombstoneSlot >= 0) {
            MainGroupPowerOperation& tombstone =
                groupPowerOperations[tombstoneSlot];
            tombstone = {};
            tombstone.id = request.operationId;
            tombstone.startedAt = now;
            tombstone.completedAt = now;
            tombstone.request = request;
            tombstone.request.action = GroupProtocolAction::SET_POWER;
            tombstone.result = GroupProtocolResult::UNCERTAIN;
            tombstone.valid = true;
        }
        return reject(GroupProtocolResult::UNCERTAIN);
    }

    if (groupReadActive || hasActiveGroupPowerOperation() ||
        activePairingOperationId != 0 ||
        (mainPairing != nullptr && mainPairing->hasActiveRemoval()))
        return reject(GroupProtocolResult::BUSY);
    if (groupRegistry == nullptr || groupStore == nullptr ||
        !groupRegistry->persistenceReady)
        return reject(GroupProtocolResult::STORAGE_UNAVAILABLE);

    LampGroup* group = findGroup(*groupRegistry, request.groupId);
    if (group == nullptr) return reject(GroupProtocolResult::NOT_FOUND);
    if (!validateGroup(*group)) return reject(GroupProtocolResult::INVALID);
    if (group->lampCount == 0) return reject(GroupProtocolResult::EMPTY_GROUP);
    for (uint8_t memberIndex = 0; memberIndex < group->lampCount; ++memberIndex)
        if (findLampOutputGuard(individualOutputGuards,
                                group->lampIds[memberIndex]) != nullptr)
            return reject(GroupProtocolResult::BUSY);

    const uint32_t now = millis();
    int slot = -1;
    for (uint8_t attempt = 0; attempt < MAIN_GROUP_POWER_OPERATION_SLOTS; ++attempt) {
        const uint8_t candidate = static_cast<uint8_t>(
            (nextGroupPowerOperationSlot + attempt) % MAIN_GROUP_POWER_OPERATION_SLOTS);
        const MainGroupPowerOperation& current = groupPowerOperations[candidate];
        if (groupPowerOperationReusable(current, now)) {
            slot = candidate;
            nextGroupPowerOperationSlot = static_cast<uint8_t>(
                (candidate + 1) % MAIN_GROUP_POWER_OPERATION_SLOTS);
            break;
        }
    }
    if (slot < 0) return reject(GroupProtocolResult::BUSY);

    MainGroupPowerOperation& operation = groupPowerOperations[slot];
    operation = {};
    operation.id = request.operationId;
    operation.startedAt = now;
    operation.request = request;
    operation.memberCount = group->lampCount;
    operation.result = GroupProtocolResult::ACCEPTED;
    operation.valid = true;
    operation.active = true;
    response.result = GroupProtocolResult::ACCEPTED;
    response.groupCount = operation.memberCount;
    response.memberCount = 0;
    (void)sendGroupResponse(response);

    const uint32_t mainCoreId = makeCoreModuleId(CORE_LOGICAL_ID, mainLogicalId);
    for (uint8_t memberIndex = 0; memberIndex < operation.memberCount;
         ++memberIndex) {
        const uint32_t localId = group->lampIds[memberIndex];
        MainGroupPowerMember& member = operation.members[memberIndex];
        member.localId = localId;
        member.destinationId = makeCoreModuleId(mainCoreId, localId);
        for (uint8_t attempt = 0; attempt < 32; ++attempt) {
            const uint32_t candidate = generateMessageId() & 0x7fffffffUL;
            bool inUse = candidate == 0 ||
                findPendingMessage(tracker, candidate) != nullptr ||
                findOutputGuard(individualOutputGuards, candidate) != nullptr;
            for (uint8_t opIndex = 0; !inUse &&
                 opIndex < MAIN_GROUP_POWER_OPERATION_SLOTS; ++opIndex) {
                const MainGroupPowerOperation& other =
                    groupPowerOperations[opIndex];
                if (!other.valid) continue;
                for (uint8_t prior = 0; prior < other.memberCount; ++prior)
                    if (other.members[prior].childCommandId == candidate)
                        inUse = true;
            }
            if (!inUse) {
                member.childCommandId = candidate;
                break;
            }
        }
        if (member.childCommandId == 0) {
            member.result = GroupProtocolResult::FAILED;
            continue;
        }
        member.result = GroupProtocolResult::IN_PROGRESS;

        Lamp* lamp = findLamp(lamps, localId);
        if (lamp == nullptr || lamp->identity.pairingState != PairingState::PAIRED ||
            lamp->identity.parentMainId != mainLogicalId ||
            lamp->identity.removalPending || lamp->device.id != localId) {
            member.result = GroupProtocolResult::FAILED;
        } else if (lamp->device.status != DeviceStatus::ONLINE) {
            member.result = GroupProtocolResult::OFFLINE;
        } else if (!hasDeviceCapability(lamp->device, DEVICE_CAP_POWER)) {
            member.result = GroupProtocolResult::UNSUPPORTED;
        } else {
            Message child = makeGroupPowerChild(operation, member);
            MainIndividualOutputGuard* guard = reserveOutputGuard(
                individualOutputGuards, child);
            if (guard == nullptr) {
                member.result = GroupProtocolResult::FAILED;
            } else if (sendMessageVia(communication, child, localId)) {
                member.waiting = true;
                member.sentAt = now;
                member.firstSentAt = now;
            } else {
                *guard = {};
                member.result = GroupProtocolResult::FAILED;
            }
        }
        sendGroupPowerMemberResult(operation, memberIndex);
    }
    finishGroupPowerIfReady(operation, now);
    return true;
}

void V5ProvisioningRuntime::pollGroupPowerOperations(uint32_t now) {
    for (uint8_t index = 0; index < MAX_PENDING_MESSAGES; ++index) {
        MainIndividualOutputGuard& guard = individualOutputGuards[index];
        if (guard.active && !guard.uncertain &&
            now - guard.startedAt >= GROUP_POWER_CHILD_DEADLINE_MS) {
            guard.uncertain = true;
        }
    }

    for (uint8_t operationIndex = 0;
         operationIndex < MAIN_GROUP_POWER_OPERATION_SLOTS; ++operationIndex) {
        MainGroupPowerOperation& operation = groupPowerOperations[operationIndex];
        if (!operation.valid || !operation.active) continue;
        for (uint8_t memberIndex = 0; memberIndex < operation.memberCount;
             ++memberIndex) {
            MainGroupPowerMember& member = operation.members[memberIndex];
            if (!member.waiting) continue;
            if (now - member.firstSentAt >= GROUP_POWER_CHILD_DEADLINE_MS) {
                member.waiting = false;
                member.result = member.supersededByPriorityOff
                    ? GroupProtocolResult::SUPERSEDED
                    : GroupProtocolResult::UNCERTAIN;
                MainIndividualOutputGuard* guard = findOutputGuard(
                    individualOutputGuards, member.childCommandId);
                if (guard != nullptr) {
                    if (member.supersededByPriorityOff) *guard = {};
                    else guard->uncertain = true;
                }
                sendGroupPowerMemberResult(operation, memberIndex);
                continue;
            }
            if (member.supersededByPriorityOff ||
                now - member.sentAt < GROUP_POWER_CHILD_TIMEOUT_MS ||
                member.retries >= MAX_MESSAGE_RETRIES) continue;
            ++member.retries;
            member.sentAt = now;
            const Message retry = makeGroupPowerChild(operation, member);
            (void)sendMessageVia(communication, retry, member.localId);
        }
        finishGroupPowerIfReady(operation, now);
    }
}

bool V5ProvisioningRuntime::handleGroupPowerAck(const Message& message) {
    for (uint8_t operationIndex = 0;
         operationIndex < MAIN_GROUP_POWER_OPERATION_SLOTS; ++operationIndex) {
        MainGroupPowerOperation& operation = groupPowerOperations[operationIndex];
        if (!operation.valid) continue;
        for (uint8_t memberIndex = 0; memberIndex < operation.memberCount;
             ++memberIndex) {
            MainGroupPowerMember& member = operation.members[memberIndex];
            if (static_cast<uint32_t>(message.value2) != member.childCommandId)
                continue;
            if (!validGroupChildAck(operation, member, message)) return false;
            if (message.executionStatus == ExecutionStatus::NOT_EXECUTED)
                return true;
            member.waiting = false;
            if (member.supersededByPriorityOff) {
                member.result = GroupProtocolResult::SUPERSEDED;
            } else if (message.executionStatus == ExecutionStatus::EXECUTED) {
                member.result = GroupProtocolResult::EXECUTED;
            } else if (message.value == static_cast<int32_t>(CoreCommandError::OFFLINE)) {
                member.result = GroupProtocolResult::OFFLINE;
            } else if (message.value ==
                       static_cast<int32_t>(CoreCommandError::UNSUPPORTED_CAPABILITY)) {
                member.result = GroupProtocolResult::UNSUPPORTED;
            } else {
                member.result = GroupProtocolResult::FAILED;
            }
            clearOutputGuard(individualOutputGuards, member.childCommandId);
            sendGroupPowerMemberResult(operation, memberIndex);
            finishGroupPowerIfReady(operation, millis());
            return true;
        }
    }

    const uint32_t originalId = static_cast<uint32_t>(message.value2);
    MainIndividualOutputGuard* guard = findOutputGuard(
        individualOutputGuards, originalId);
    if (guard == nullptr || message.type != MessageType::ACK ||
        message.destinationId != CORE_LOGICAL_ID ||
        message.commandType != guard->commandType ||
        message.parentMainId != makeCoreModuleId(CORE_LOGICAL_ID, mainLogicalId) ||
        message.provisioningDeviceId != guard->localId ||
        message.sourceId != guard->destinationId ||
        (message.executionStatus != ExecutionStatus::EXECUTED &&
         message.executionStatus != ExecutionStatus::FAILED)) return false;
    const uint32_t fencedId = guard->fencesMessageId;
    const bool succeeded = message.executionStatus == ExecutionStatus::EXECUTED &&
        message.value == static_cast<int32_t>(ExecutionStatus::EXECUTED);
    *guard = {};
    if (succeeded) clearOutputGuard(individualOutputGuards, fencedId);
    return false;
}

bool V5ProvisioningRuntime::handleGroupOutputCommand(const Message& incoming) {
    if (role != DeviceRole::MAIN || !validCoreOutputCommand(incoming)) return false;
    const uint32_t localId = incoming.provisioningDeviceId;
    const uint32_t mainCoreId = makeCoreModuleId(CORE_LOGICAL_ID, mainLogicalId);
    if (incoming.parentMainId != mainCoreId ||
        incoming.destinationId != makeCoreModuleId(mainCoreId, localId)) return false;

    MainGroupPowerOperation* activeOperation = nullptr;
    MainGroupPowerMember* groupMember = nullptr;
    for (uint8_t opIndex = 0; opIndex < MAIN_GROUP_POWER_OPERATION_SLOTS; ++opIndex) {
        MainGroupPowerOperation& operation = groupPowerOperations[opIndex];
        if (!operation.valid || !operation.active) continue;
        activeOperation = &operation;
        for (uint8_t memberIndex = 0; memberIndex < operation.memberCount;
             ++memberIndex) {
            if (operation.members[memberIndex].localId == localId &&
                operation.members[memberIndex].destinationId == incoming.destinationId) {
                groupMember = &operation.members[memberIndex];
                break;
            }
        }
        if (groupMember != nullptr) break;
        activeOperation = nullptr;
    }

    const bool isPriorityOff = incoming.commandType ==
        static_cast<int32_t>(ActionType::SET_LAMP_POWER) && incoming.value == 0;
    if (activeOperation != nullptr && groupMember != nullptr) {
        if (!isPriorityOff) {
            const Message rejected = makeCoreCommandAck(incoming, mainCoreId,
                ExecutionStatus::FAILED, CoreCommandError::COMMAND_IN_PROGRESS);
            (void)sendMessageVia(communication, rejected, CORE_LOGICAL_ID,
                CommunicationRouteScope::CORE);
            return true;
        }

        Message priority = incoming;
        priority.value2 = static_cast<int32_t>(groupMember->childCommandId);
        for (uint8_t index = 0; index < MAX_PENDING_MESSAGES; ++index) {
            const MainIndividualOutputGuard& guard = individualOutputGuards[index];
            if (guard.active && guard.localId == localId &&
                guard.messageId != groupMember->childCommandId) {
                const Message rejected = makeCoreCommandAck(incoming, mainCoreId,
                    ExecutionStatus::FAILED, CoreCommandError::COMMAND_IN_PROGRESS);
                (void)sendMessageVia(communication, rejected, CORE_LOGICAL_ID,
                    CommunicationRouteScope::CORE);
                return true;
            }
        }
        MainIndividualOutputGuard* offGuard = reserveOutputGuard(
            individualOutputGuards, priority, groupMember->childCommandId);
        if (offGuard == nullptr) {
            const Message rejected = makeCoreCommandAck(incoming, mainCoreId,
                ExecutionStatus::FAILED, CoreCommandError::COMMAND_IN_PROGRESS);
            (void)sendMessageVia(communication, rejected, CORE_LOGICAL_ID,
                CommunicationRouteScope::CORE);
            return true;
        }
        groupMember->supersededByPriorityOff = true;
        MainIndividualOutputGuard* groupGuard = findOutputGuard(
            individualOutputGuards, groupMember->childCommandId);
        if (groupGuard != nullptr) groupGuard->supersededByPriorityOff = true;
        Message accepted = makeCoreCommandAck(priority, mainCoreId,
                                              ExecutionStatus::NOT_EXECUTED);
        (void)sendMessageVia(communication, accepted, CORE_LOGICAL_ID,
                             CommunicationRouteScope::CORE);
        if (!sendMessageVia(communication, priority, localId)) {
            *offGuard = {};
            groupMember->supersededByPriorityOff = false;
            const Message failed = makeCoreCommandAck(priority, mainCoreId,
                ExecutionStatus::FAILED, CoreCommandError::TRANSPORT_FAILURE);
            (void)sendMessageVia(communication, failed, CORE_LOGICAL_ID,
                                 CommunicationRouteScope::CORE);
        } else {
            groupMember->waiting = false;
            groupMember->result = GroupProtocolResult::SUPERSEDED;
            clearOutputGuard(individualOutputGuards,
                             groupMember->childCommandId);
            sendGroupPowerMemberResult(*activeOperation,
                static_cast<uint8_t>(groupMember - activeOperation->members));
            finishGroupPowerIfReady(*activeOperation, millis());
        }
        return true;
    }

    MainIndividualOutputGuard* existing = findLampOutputGuard(
        individualOutputGuards, localId);
    if (existing != nullptr) {
        if (!isPriorityOff || existing->value == 0 ||
            existing->supersededByPriorityOff) {
            const Message rejected = makeCoreCommandAck(incoming, mainCoreId,
                ExecutionStatus::FAILED, CoreCommandError::COMMAND_IN_PROGRESS);
            (void)sendMessageVia(communication, rejected, CORE_LOGICAL_ID,
                CommunicationRouteScope::CORE);
            return true;
        }
        Message priority = incoming;
        if (priority.value2 == 0)
            priority.value2 = static_cast<int32_t>(existing->messageId);
        MainIndividualOutputGuard* oldGuard = existing;
        MainIndividualOutputGuard* offGuard = reserveOutputGuard(
            individualOutputGuards, priority, existing->messageId);
        if (offGuard == nullptr) {
            const Message rejected = makeCoreCommandAck(incoming, mainCoreId,
                ExecutionStatus::FAILED, CoreCommandError::COMMAND_IN_PROGRESS);
            (void)sendMessageVia(communication, rejected, CORE_LOGICAL_ID,
                CommunicationRouteScope::CORE);
            return true;
        }
        oldGuard->supersededByPriorityOff = true;
        Message accepted = makeCoreCommandAck(priority, mainCoreId,
                                              ExecutionStatus::NOT_EXECUTED);
        (void)sendMessageVia(communication, accepted, CORE_LOGICAL_ID,
                             CommunicationRouteScope::CORE);
        if (!sendMessageVia(communication, priority, localId)) {
            *offGuard = {};
            oldGuard->supersededByPriorityOff = false;
            const Message failed = makeCoreCommandAck(priority, mainCoreId,
                ExecutionStatus::FAILED, CoreCommandError::TRANSPORT_FAILURE);
            (void)sendMessageVia(communication, failed, CORE_LOGICAL_ID,
                                 CommunicationRouteScope::CORE);
        }
        return true;
    }

    if (reserveOutputGuard(individualOutputGuards, incoming) == nullptr) {
        const Message rejected = makeCoreCommandAck(incoming, mainCoreId,
            ExecutionStatus::FAILED, CoreCommandError::COMMAND_IN_PROGRESS);
        (void)sendMessageVia(communication, rejected, CORE_LOGICAL_ID,
            CommunicationRouteScope::CORE);
        return true;
    }
    return false;
}

bool V5ProvisioningRuntime::handleCoreGroupControl(const Message& message) {
    const GroupProtocolPayload& request = message.group;
    if (role != DeviceRole::MAIN || groupRegistry == nullptr ||
        (request.version != GROUP_PROTOCOL_VERSION &&
         request.version != GROUP_POWER_PROTOCOL_VERSION) ||
        request.operationId == 0 || request.mainLocalId != mainLogicalId ||
        request.mainCoreId != makeCoreModuleId(CORE_LOGICAL_ID, mainLogicalId) ||
        message.sourceId != CORE_LOGICAL_ID ||
        message.destinationId != mainLogicalId ||
        message.parentMainId != request.mainCoreId) return false;

    if (request.action == GroupProtocolAction::SET_POWER ||
        request.action == GroupProtocolAction::QUERY_OPERATION)
        return handleGroupPowerRequest(message);
    if (request.version != GROUP_PROTOCOL_VERSION || groupStore == nullptr)
        return false;

    GroupProtocolPayload reply = {};
    reply.version = GROUP_PROTOCOL_VERSION;
    reply.action = GroupProtocolAction::RESULT;
    reply.operationId = request.operationId;
    reply.mainLocalId = mainLogicalId;
    reply.mainCoreId = request.mainCoreId;

    bool validRequest = isGroupProtocolRequest(request.action) &&
        isValidGroupProtocolPayload(request, GROUP_ZIGBEE_MEMBER_CHUNK) &&
        request.result == GroupProtocolResult::NONE &&
        request.groupCount == 0 && request.memberCount == 0 &&
        request.memberOffset == 0 &&
        memchr(request.name, '\0', sizeof(request.name)) != nullptr;
    const bool hasName = request.name[0] != '\0';
    switch (request.action) {
        case GroupProtocolAction::LIST:
            validRequest = validRequest && request.groupId == 0 &&
                request.expectedRevision == 0 && request.memberLocalId == 0 && !hasName;
            break;
        case GroupProtocolAction::GET:
            validRequest = validRequest && request.groupId != 0 &&
                request.expectedRevision == 0 && request.memberLocalId == 0 && !hasName;
            break;
        case GroupProtocolAction::CREATE: {
            LampGroup nameCheck = {};
            validRequest = validRequest && request.groupId == request.operationId &&
                request.expectedRevision == 0 && request.memberLocalId == 0 && hasName &&
                setGroupName(nameCheck, request.name);
            break;
        }
        case GroupProtocolAction::RENAME: {
            LampGroup nameCheck = {};
            validRequest = validRequest && request.groupId != 0 &&
                request.expectedRevision != 0 && request.memberLocalId == 0 && hasName &&
                setGroupName(nameCheck, request.name);
            break;
        }
        case GroupProtocolAction::ADD_MEMBER:
        case GroupProtocolAction::REMOVE_MEMBER:
            validRequest = validRequest && request.groupId != 0 &&
                request.expectedRevision != 0 && request.memberLocalId != 0 && !hasName;
            break;
        case GroupProtocolAction::DELETE_GROUP:
            validRequest = validRequest && request.groupId != 0 &&
                request.expectedRevision != 0 && request.memberLocalId == 0 && !hasName;
            break;
        default:
            validRequest = false;
            break;
    }
    if (!validRequest) {
        reply.result = GroupProtocolResult::INVALID;
        (void)sendGroupResponse(reply);
        return true;
    }

    if (groupReadActive || hasActiveGroupPowerOperation()) {
        reply.result = GroupProtocolResult::BUSY;
        (void)sendGroupResponse(reply);
        return true;
    }
    if (request.action == GroupProtocolAction::LIST ||
        request.action == GroupProtocolAction::GET) {
        if (!groupRegistry->persistenceReady) {
            reply.result = GroupProtocolResult::STORAGE_UNAVAILABLE;
            (void)sendGroupResponse(reply);
            return true;
        }
        groupReadIsList = request.action == GroupProtocolAction::LIST;
        groupReadSnapshot = {};
        groupReadSnapshot.mainId = groupRegistry->mainId;
        groupReadSnapshot.persistenceReady = groupRegistry->persistenceReady;
        if (groupReadIsList) {
            groupReadSnapshot.count = groupRegistry->count;
            memcpy(groupReadSnapshot.groups, groupRegistry->groups,
                   sizeof(groupReadSnapshot.groups));
        } else {
            const LampGroup* found = nullptr;
            for (uint8_t index = 0; index < groupRegistry->count; ++index) {
                if (groupRegistry->groups[index].id == request.groupId) {
                    found = &groupRegistry->groups[index];
                    break;
                }
            }
            if (found == nullptr) {
                reply.result = GroupProtocolResult::NOT_FOUND;
                reply.groupId = request.groupId;
                (void)sendGroupResponse(reply);
                return true;
            }
            groupReadSnapshot.count = 1;
            groupReadSnapshot.groups[0] = *found;
        }
        reply.result = GroupProtocolResult::ACCEPTED;
        (void)sendGroupResponse(reply);
        activeGroupRequest = request;
        groupReadIndex = 0;
        groupReadMemberOffset = 0;
        groupReadPhase = groupReadIsList ? 0 : 1;
        groupReadActive = true;
        return true;
    }

    if (!groupRegistry->persistenceReady) {
        reply.result = GroupProtocolResult::STORAGE_UNAVAILABLE;
        (void)sendGroupResponse(reply);
        return true;
    }

    reply.result = GroupProtocolResult::ACCEPTED;
    (void)sendGroupResponse(reply);
    reply.result = GroupProtocolResult::FAILED;
    if (request.action == GroupProtocolAction::CREATE) {
        LampGroup* existing = findGroup(*groupRegistry, request.operationId);
        if (existing != nullptr) {
            reply.groupId = existing->id;
            reply.expectedRevision = existing->revision;
            reply.result = strcmp(existing->name, request.name) == 0
                ? GroupProtocolResult::PERSISTED
                : GroupProtocolResult::REVISION_CONFLICT;
        } else {
            reply.groupId = request.operationId;
            if (createGroupPersisted(*groupRegistry, *groupStore,
                    request.operationId, request.name)) {
                const LampGroup* created = findGroup(*groupRegistry,
                                                      request.operationId);
                reply.result = created == nullptr
                    ? GroupProtocolResult::FAILED
                    : GroupProtocolResult::PERSISTED;
                if (created != nullptr) reply.expectedRevision = created->revision;
            } else if (!groupRegistry->persistenceReady) {
                reply.result = GroupProtocolResult::STORAGE_UNAVAILABLE;
            } else if (groupRegistry->count >= MAX_GROUPS) {
                reply.result = GroupProtocolResult::FAILED;
            }
        }
    } else {
        LampGroup* group = findGroup(*groupRegistry, request.groupId);
        reply.groupId = request.groupId;
        if (group == nullptr) {
            reply.result = request.action == GroupProtocolAction::DELETE_GROUP
                ? GroupProtocolResult::PERSISTED : GroupProtocolResult::NOT_FOUND;
        } else {
            reply.expectedRevision = group->revision;
            bool alreadyApplied = false;
            if (request.action == GroupProtocolAction::RENAME) {
                alreadyApplied = strcmp(group->name, request.name) == 0;
            } else if (request.action == GroupProtocolAction::ADD_MEMBER) {
                alreadyApplied = lampBelongsToGroup(*group, request.memberLocalId);
            } else if (request.action == GroupProtocolAction::REMOVE_MEMBER) {
                alreadyApplied = !lampBelongsToGroup(*group, request.memberLocalId);
            }
            if (alreadyApplied) {
                reply.result = GroupProtocolResult::PERSISTED;
            } else if (request.expectedRevision != group->revision) {
                reply.result = GroupProtocolResult::REVISION_CONFLICT;
            } else {
                bool applied = false;
                switch (request.action) {
                    case GroupProtocolAction::RENAME:
                        applied = renameGroupPersisted(*groupRegistry, *groupStore,
                            request.groupId, request.name, lamps);
                        break;
                    case GroupProtocolAction::ADD_MEMBER: {
                        Lamp* lamp = findLamp(lamps, request.memberLocalId);
                        if (lamp != nullptr &&
                            lamp->identity.parentMainId == mainLogicalId &&
                            lamp->identity.pairingState == PairingState::PAIRED &&
                            !lamp->identity.removalPending &&
                            group->lampCount < MAX_GROUP_LAMPS) {
                            applied = addLampToGroupPersisted(*groupRegistry, lamps,
                                *groupStore, request.groupId, request.memberLocalId);
                        }
                        break;
                    }
                    case GroupProtocolAction::REMOVE_MEMBER:
                        applied = removeLampFromGroupPersisted(*groupRegistry,
                            lamps, *groupStore, request.groupId,
                            request.memberLocalId);
                        break;
                    case GroupProtocolAction::DELETE_GROUP:
                        applied = deleteGroupPersisted(*groupRegistry,
                            *groupStore, request.groupId);
                        break;
                    default:
                        break;
                }
                if (applied) {
                    reply.result = GroupProtocolResult::PERSISTED;
                    const LampGroup* changed = findGroup(*groupRegistry,
                                                          request.groupId);
                    reply.expectedRevision = changed == nullptr
                        ? 0 : changed->revision;
                } else if (!groupRegistry->persistenceReady) {
                    reply.result = GroupProtocolResult::STORAGE_UNAVAILABLE;
                } else if (request.action == GroupProtocolAction::ADD_MEMBER &&
                           findLamp(lamps, request.memberLocalId) == nullptr) {
                    reply.result = GroupProtocolResult::INVALID;
                } else {
                    reply.result = GroupProtocolResult::FAILED;
                }
            }
        }
    }

    (void)sendGroupResponse(reply);
    return true;
}

void V5ProvisioningRuntime::pollGroupRead() {
    if (!groupReadActive || groupReadIndex >= groupReadSnapshot.count + 1) return;
    GroupProtocolPayload reply = {};
    reply.version = GROUP_PROTOCOL_VERSION;
    reply.operationId = activeGroupRequest.operationId;
    reply.mainLocalId = mainLogicalId;
    reply.mainCoreId = makeCoreModuleId(CORE_LOGICAL_ID, mainLogicalId);
    if (groupReadPhase == 0) {
        reply.action = GroupProtocolAction::LIST_BEGIN;
        reply.groupCount = groupReadSnapshot.count;
        groupReadPhase = 1;
    } else if (groupReadIndex < groupReadSnapshot.count) {
        const LampGroup& group = groupReadSnapshot.groups[groupReadIndex];
        reply.groupId = group.id;
        reply.expectedRevision = group.revision;
        if (groupReadPhase == 1) {
            reply.action = GroupProtocolAction::GROUP_META;
            reply.memberCount = group.lampCount;
            strncpy(reply.name, group.name, sizeof(reply.name) - 1);
            groupReadMemberOffset = 0;
            groupReadPhase = 2;
        } else if (groupReadMemberOffset < group.lampCount) {
            reply.action = GroupProtocolAction::MEMBER_CHUNK;
            reply.memberOffset = groupReadMemberOffset;
            const uint8_t remaining = static_cast<uint8_t>(group.lampCount -
                                                           groupReadMemberOffset);
            reply.memberCount = remaining > GROUP_ZIGBEE_MEMBER_CHUNK
                ? GROUP_ZIGBEE_MEMBER_CHUNK : remaining;
            for (uint8_t index = 0; index < reply.memberCount; ++index)
                reply.memberIds[index] = group.lampIds[groupReadMemberOffset + index];
            groupReadMemberOffset = static_cast<uint8_t>(groupReadMemberOffset +
                                                         reply.memberCount);
        } else {
            ++groupReadIndex;
            groupReadPhase = 1;
            return;
        }
    } else if (groupReadIsList) {
        reply.action = GroupProtocolAction::LIST_END;
        reply.groupCount = groupReadSnapshot.count;
        groupReadIndex = static_cast<uint8_t>(groupReadSnapshot.count + 1);
    } else {
        reply.action = GroupProtocolAction::RESULT;
        reply.result = GroupProtocolResult::COMPLETED;
        groupReadIndex = static_cast<uint8_t>(groupReadSnapshot.count + 1);
    }
    if (!sendGroupResponse(reply)) {
        Serial.println("[GROUPS] read response send failed; CORE will time out");
        groupReadActive = false;
        return;
    }
    if (reply.action == GroupProtocolAction::LIST_END ||
        (reply.action == GroupProtocolAction::RESULT &&
         reply.result == GroupProtocolResult::COMPLETED))
        groupReadActive = false;
}

bool V5ProvisioningRuntime::handleMessage(
    Communication&,
    const Message& message,
    void* context
) {
    V5ProvisioningRuntime* runtime =
        static_cast<V5ProvisioningRuntime*>(context);
    return runtime != nullptr && runtime->dispatch(message);
}

bool V5ProvisioningRuntime::dispatch(const Message& message) {
    if (message.type == MessageType::GROUP_CONTROL)
        return handleCoreGroupControl(message);
    if (message.type == MessageType::ACK && handleGroupPowerAck(message))
        return true;
    if (message.type == MessageType::COMMAND &&
        handleGroupOutputCommand(message)) return true;
    if (message.type == MessageType::STATE &&
        message.commandType == CORE_REMOVAL_CONTROL_MESSAGE) {
        return handleCoreRemovalControl(message);
    }
    if (message.type == MessageType::STATE &&
        message.commandType == CORE_PAIRING_CONTROL_MESSAGE) {
        return handleCorePairingControl(message);
    }
    if (!isProvisioningMessage(message.type)) {
        return false;
    }
    ProvisioningPacket packet = {};
    if (!decodeProvisioningPacket(message, packet)) {
        Serial.println("[PAIRING] invalid provisioning packet dropped");
        return true;
    }

    if (role == DeviceRole::MAIN && mainPairing != nullptr) {
        if (packet.type == ProvisioningPacketType::DEVICE_ANNOUNCE) {
            const DiscoveryResult result = mainPairing->discover(packet);
            if (result == DiscoveryResult::RESTORED_PAIRED ||
                result == DiscoveryResult::RECOGNIZED_PAIRED) {
                ProvisioningPacket announcement = packet;
                announcement.type = ProvisioningPacketType::DEVICE_ANNOUNCE;
                announceAcceptedLamp(announcement);
            }
            if (result == DiscoveryResult::REMOVAL_PENDING) {
                lastRemovalContactAt = millis();
                if (packet.pairingState == PairingState::UNPAIRING) {
                    processRemovalEvidence(packet);
                } else if (packet.pairingState == PairingState::PAIRED) {
                    ProvisioningPacket request = {};
                    if (mainPairing->buildRemovalPacket(request)) {
                        (void)sendPacket(request, mainLogicalId, request.deviceId);
                    }
                }
            } else if (result == DiscoveryResult::REMOVAL_RECONCILED) {
                processRemovalEvidence(packet);
            }
            if (result == DiscoveryResult::DISCOVERED_UNPAIRED) {
                Serial.print("[DISCOVERY] DEVICE_ANNOUNCE hardwareId=");
                Serial.print(packet.hardwareId);
                Serial.println(" (pairing not automatic)");
                if (activePairingSessionId != 0 && !pairingSessionExpired) {
                    Message candidate = {};
                    if (makeCorePairingCandidateMessage(mainLogicalId,
                            activePairingSessionId, packet, candidate)) {
                        candidate.timestamp = millis();
                        (void)sendMessage(communication, candidate);
                    }
                }
            } else if (result == DiscoveryResult::DUPLICATE_ANNOUNCEMENT &&
                       packet.pairingState == PairingState::UNPAIRED &&
                       activePairingSessionId != 0 && !pairingSessionExpired) {
                Message candidate = {};
                if (makeCorePairingCandidateMessage(mainLogicalId,
                        activePairingSessionId, packet, candidate)) {
                    candidate.timestamp = millis();
                    (void)sendMessage(communication, candidate);
                }
            } else if (result == DiscoveryResult::CANDIDATE_CAPACITY &&
                       activePairingSessionId != 0 && !pairingSessionExpired) {
                Message limit = {};
                if (makeCorePairingCandidateLimitMessage(mainLogicalId,
                        activePairingSessionId, limit)) {
                    limit.timestamp = millis();
                    (void)sendMessage(communication, limit);
                }
            } else if (result == DiscoveryResult::RESTORED_PAIRED) {
                Serial.print("[PAIRING] restored Device ID ");
                Serial.println(packet.deviceId);
            } else if (result == DiscoveryResult::IDENTITY_CONFLICT) {
                Serial.println("[PAIRING] identity conflict; no new pairing");
                if (mainPairing->isRemovedIdentity(packet.deviceId,
                                                   packet.hardwareId)) {
                    (void)forgetZigbeeRoute(packet.deviceId, packet.hardwareId,
                        CommunicationRouteScope::LOCAL_DEVICE);
                }
            }
            return true;
        }
        if (packet.type == ProvisioningPacketType::UNPAIR_CONFIRM ||
            packet.type == ProvisioningPacketType::UNPAIR_REJECT) {
            processRemovalEvidence(packet);
            return true;
        }
        if (packet.type == ProvisioningPacketType::PAIR_CONFIRM) {
            if (mainPairing->confirmPairing(packet)) {
                Serial.print("[PAIRING] PAIRED deviceId=");
                Serial.println(packet.deviceId);
                ProvisioningPacket announcement = packet;
                announcement.type = ProvisioningPacketType::DEVICE_ANNOUNCE;
                announceAcceptedLamp(announcement);
                if (activePairingOperationId != 0 &&
                    strncmp(activePairingHardwareId, packet.hardwareId,
                            sizeof(activePairingHardwareId)) == 0) {
                    sendPairingOperationResult(activePairingOperationSessionId,
                        activePairingOperationId, CorePairingResultCode::SUCCEEDED,
                        packet.deviceId, packet.hardwareId);
                    clearPairingOperation();
                }
            } else {
                Serial.println("[PAIRING] invalid PAIR_CONFIRM rejected");
            }
            return true;
        }
        if (packet.type == ProvisioningPacketType::PAIR_REJECT) {
            if (activePairingOperationId != 0 &&
                strncmp(activePairingHardwareId, packet.hardwareId,
                        sizeof(activePairingHardwareId)) == 0) {
                const uint32_t sessionId = activePairingOperationSessionId;
                const uint32_t operationId = activePairingOperationId;
                mainPairing->abortPendingPairing();
                sendPairingOperationResult(sessionId, operationId,
                    CorePairingResultCode::REJECTED, 0, packet.hardwareId);
                clearPairingOperation();
            }
            return true;
        }
        return true;
    }

    if (role == DeviceRole::LAMP && lampPairing != nullptr) {
        if (packet.type == ProvisioningPacketType::PAIR_REQUEST) {
            ProvisioningPacket rejection = {};
            if (lampPairing->handlePairRequest(packet, rejection)) {
                Serial.println("[PAIRING] explicit PAIR_REQUEST accepted for processing");
            } else {
                Serial.println("[PAIRING] PAIR_REQUEST rejected");
            }
            return true;
        }
        if (packet.type == ProvisioningPacketType::PAIR_ACCEPT) {
            ProvisioningPacket confirmation = {};
            if (lampPairing->handlePairAccept(packet, confirmation)) {
                restoreLocalLamp();
                if (sendPacket(confirmation, confirmation.deviceId,
                               confirmation.parentMainId)) {
                    Serial.println("[PAIRING] pairing persisted; PAIR_CONFIRM sent");
                }
            } else {
                Serial.println("[PAIRING] PAIR_ACCEPT rejected");
            }
            return true;
        }
        if (packet.type == ProvisioningPacketType::UNPAIR_REQUEST) {
            const ProvisioningPacket identity = lampPairing->announce();
            const bool exactRemoval = packet.role == DeviceRole::MAIN &&
                packet.pairingState == PairingState::UNPAIRING &&
                packet.transactionId != 0 && packet.deviceId != 0 &&
                packet.parentMainId != 0 &&
                packet.sourceId == packet.parentMainId &&
                packet.destinationId == packet.deviceId &&
                sameText(packet.hardwareId, identity.hardwareId) &&
                packet.deviceId == identity.deviceId &&
                packet.parentMainId == identity.parentMainId &&
                (identity.pairingState == PairingState::PAIRED ||
                 (identity.pairingState == PairingState::UNPAIRING &&
                  packet.transactionId == identity.transactionId));
            Lamp* localLamp = exactRemoval
                ? findLamp(lamps, packet.deviceId) : nullptr;
            const bool localIdentityMatches = localLamp == nullptr ||
                (sameText(localLamp->identity.hardwareId, packet.hardwareId) &&
                 localLamp->identity.parentMainId == packet.parentMainId);
            ProvisioningPacket response = {};
            bool outputOff = false;
            LampOutputInterruption interruption = {};
            if (exactRemoval && localIdentityMatches) {
                if (localLamp != nullptr) {
                    outputOff = forceLampOutputOff(*localLamp, millis(),
                        interruption) == LampOutputOffResult::APPLIED;
                    if (interruption.hadTransition &&
                        (interruption.transitionStopped ||
                         interruption.transitionCompleted)) {
                        finishLampOutputTransition(communication, deduplicator,
                            *localLamp, interruption.transition,
                            interruption.transitionCompleted
                                ? ExecutionStatus::EXECUTED
                                : ExecutionStatus::FAILED);
                    }
                } else {
                    // A rebooted lamp may retain an idempotent removal receipt
                    // without restoring its DeviceRegistry entry. Re-assert OFF
                    // before replaying that receipt.
                    outputOff = writeLampHardwarePower(false, 100);
                }
            }
            const bool accepted = outputOff &&
                lampPairing->handleUnpairRequest(packet, response);
            if (!accepted && response.type !=
                    ProvisioningPacketType::UNPAIR_REJECT) {
                makeUnpairReject(packet, identity, lampPairing->state(), response);
            }
            if (accepted && response.type == ProvisioningPacketType::UNPAIR_CONFIRM) {
                (void)removeLamp(lamps, response.deviceId);
                (void)sendPacket(response, response.deviceId,
                                 response.parentMainId);
                Serial.println("[UNPAIR] application binding cleared; confirmation sent");
            } else if (response.type == ProvisioningPacketType::UNPAIR_REJECT) {
                (void)sendPacket(response, response.deviceId,
                                 response.parentMainId);
                Serial.println("[UNPAIR] request rejected; association preserved");
            }
            return true;
        }
        if (packet.type == ProvisioningPacketType::UNPAIR_COMMIT) {
            if (lampPairing->handleUnpairCommit(packet)) {
                (void)removeLamp(lamps, packet.deviceId);
                Serial.println("[UNPAIR] durable receipt acknowledged; lamp unpaired");
            }
            return true;
        }
        return true;
    }
    return true;
}

bool V5ProvisioningRuntime::sendPacket(
    const ProvisioningPacket& packet,
    uint32_t sourceId,
    uint32_t destinationId
) {
    Message message = {};
    if (!encodeProvisioningPacket(packet, generateMessageId(), sourceId,
                                  destinationId, message)) {
        return false;
    }
    return sendMessage(communication, message);
}

void V5ProvisioningRuntime::restoreLocalLamp() {
    if (lampPairing == nullptr) {
        return;
    }
    const ProvisioningPacket identity = lampPairing->announce();
    if (identity.pairingState != PairingState::PAIRED || identity.deviceId == 0 ||
        identity.parentMainId == 0 || findLamp(lamps, identity.deviceId) != nullptr) {
        return;
    }

    Lamp local = {};
    local.device.id = identity.deviceId;
    local.device.name = "LAMP";
    local.device.role = DeviceRole::LAMP;
    local.device.status = DeviceStatus::ONLINE;
    local.device.lastSeen = millis();
    local.device.capabilities = identity.capabilities;
    local.state = {false, 0, false};
    if (lampStore != nullptr) {
        restoreLampSettings(local, *lampStore);
    }
    copyText(local.identity.hardwareId, sizeof(local.identity.hardwareId),
             identity.hardwareId);
    copyText(local.identity.name, sizeof(local.identity.name), identity.name);
    local.identity.parentMainId = identity.parentMainId;
    local.identity.pairingState = identity.pairingState;
    (void)addLamp(lamps, local);
}
