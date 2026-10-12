#include <new>
#include <string.h>

#include <Arduino.h>

#include "action.h"
#include "device_manager.h"
#include "device_hardware_id.h"
#include "message_id_generator.h"
#include "message_router.h"
#include "v5_provisioning_runtime.h"
#include "zigbee_transport.h"

namespace {
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
           type == MessageType::PAIR_REJECT;
}
}  // namespace

V5ProvisioningRuntime::V5ProvisioningRuntime(
    Communication& communicationValue,
    LampRegistry& lampsValue,
    MessageTracker& trackerValue
)
    : communication(communicationValue),
      lamps(lampsValue),
      tracker(trackerValue),
      role(DeviceRole::LAMP),
      lampStore(nullptr),
      reservationStore(nullptr),
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
      pairingOperationStartedAt(0) {
    hardwareId[0] = '\0';
    activePairingHardwareId[0] = '\0';
}

bool V5ProvisioningRuntime::begin(
    DeviceRole roleValue,
    ProvisioningStore* lampStoreValue,
    ProvisioningReservationStore* reservationStoreValue
) {
    role = roleValue;
    lampStore = lampStoreValue;
    reservationStore = reservationStoreValue;
    if (!readDeviceHardwareId(hardwareId, sizeof(hardwareId))) {
        Serial.println("[IDENTITY] failed to read IEEE 802.15.4 hardware ID");
        return false;
    }

    mainLogicalId = stableLogicalId(hardwareId);
    if (role == DeviceRole::MAIN) {
        mainPairing = new (std::nothrow)
            MainProvisioning(mainLogicalId, lamps, reservationStore);
        if (mainPairing == nullptr) {
            return false;
        }
        Serial.print("[IDENTITY] MAIN hardwareId=");
        Serial.print(hardwareId);
        Serial.print(" logicalId=");
        Serial.println(mainLogicalId);
    } else if (role == DeviceRole::LAMP && lampStore != nullptr) {
        lampPairing = new (std::nothrow)
            ProvisioningLamp(hardwareId, "5.1.0", 0, *lampStore);
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
    if (activePairingOperationId != 0 ||
        !mainPairing->hasUnpairedCandidate(message.hardwareId)) {
        sendPairingOperationResult(operationSessionId, operationId,
            activePairingOperationId != 0 ? CorePairingResultCode::BUSY
                                          : CorePairingResultCode::NOT_FOUND,
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
            }
            return true;
        }
        if (packet.type == ProvisioningPacketType::PAIR_CONFIRM) {
            if (mainPairing->confirmPairing(packet)) {
                Serial.print("[PAIRING] PAIRED deviceId=");
                Serial.println(packet.deviceId);
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
    local.state = {false, 0, false};
    copyText(local.identity.hardwareId, sizeof(local.identity.hardwareId),
             identity.hardwareId);
    copyText(local.identity.name, sizeof(local.identity.name), identity.name);
    local.identity.parentMainId = identity.parentMainId;
    local.identity.pairingState = identity.pairingState;
    (void)addLamp(lamps, local);
}
