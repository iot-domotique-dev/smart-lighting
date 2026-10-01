#include <new>
#include <string.h>

#include <Arduino.h>

#include "action.h"
#include "device_hardware_id.h"
#include "message_id_generator.h"
#include "message_router.h"
#include "v5_provisioning_runtime.h"

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
      lampPairing(nullptr),
      mainPairing(nullptr),
      mainLogicalId(0),
      nextAnnounceAt(0) {
    hardwareId[0] = '\0';
}

bool V5ProvisioningRuntime::begin(
    DeviceRole roleValue,
    ProvisioningStore* lampStoreValue
) {
    role = roleValue;
    lampStore = lampStoreValue;
    if (!readDeviceHardwareId(hardwareId, sizeof(hardwareId))) {
        Serial.println("[IDENTITY] failed to read IEEE 802.15.4 hardware ID");
        return false;
    }

    mainLogicalId = stableLogicalId(hardwareId);
    if (role == DeviceRole::MAIN) {
        mainPairing = new (std::nothrow) MainProvisioning(mainLogicalId, lamps);
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
    if (role != DeviceRole::LAMP || lampPairing == nullptr ||
        !communicationReady(communication)) {
        return;
    }

    const uint32_t now = millis();
    if (static_cast<int32_t>(now - nextAnnounceAt) < 0) {
        return;
    }
    const ProvisioningPacket announcement = lampPairing->announce();
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

    ProvisioningPacket request = {};
    mainPairing->startCommissioning();
    const bool requestCreated =
        mainPairing->createPairRequest(hardwareIdValue, request);
    if (!requestCreated) {
        mainPairing->stopCommissioning();
        return false;
    }
    const bool requestSent = sendPacket(request, mainLogicalId, 0);

    ProvisioningPacket acceptance = {};
    const bool acceptanceCreated = mainPairing->createPairAcceptance(
        hardwareIdValue, requestedName, acceptance);
    const bool acceptanceSent = acceptanceCreated &&
        sendPacket(acceptance, mainLogicalId, 0);
    mainPairing->stopCommissioning();
    return requestSent && acceptanceSent;
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
            } else {
                Serial.println("[PAIRING] invalid PAIR_CONFIRM rejected");
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
