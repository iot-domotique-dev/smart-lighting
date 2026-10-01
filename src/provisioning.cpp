#include <string.h>

#include "provisioning.h"

static void copyText(char* destination, size_t capacity, const char* source) {
    if (capacity == 0) {
        return;
    }
    if (source == nullptr) {
        destination[0] = '\0';
        return;
    }
    size_t i = 0;
    while (i + 1 < capacity && source[i] != '\0') {
        destination[i] = source[i];
        ++i;
    }
    destination[i] = '\0';
}

static bool sameText(const char* left, const char* right) {
    return left != nullptr && right != nullptr && strcmp(left, right) == 0;
}

static bool packetTypeToMessageType(
    ProvisioningPacketType packetType,
    MessageType& messageType
) {
    switch (packetType) {
        case ProvisioningPacketType::DEVICE_ANNOUNCE:
            messageType = MessageType::DEVICE_ANNOUNCE;
            return true;
        case ProvisioningPacketType::PAIR_REQUEST:
            messageType = MessageType::PAIR_REQUEST;
            return true;
        case ProvisioningPacketType::PAIR_ACCEPT:
            messageType = MessageType::PAIR_ACCEPT;
            return true;
        case ProvisioningPacketType::PAIR_CONFIRM:
            messageType = MessageType::PAIR_CONFIRM;
            return true;
        case ProvisioningPacketType::PAIR_REJECT:
            messageType = MessageType::PAIR_REJECT;
            return true;
    }
    return false;
}

static bool messageTypeToPacketType(
    MessageType messageType,
    ProvisioningPacketType& packetType
) {
    switch (messageType) {
        case MessageType::DEVICE_ANNOUNCE:
            packetType = ProvisioningPacketType::DEVICE_ANNOUNCE;
            return true;
        case MessageType::PAIR_REQUEST:
            packetType = ProvisioningPacketType::PAIR_REQUEST;
            return true;
        case MessageType::PAIR_ACCEPT:
            packetType = ProvisioningPacketType::PAIR_ACCEPT;
            return true;
        case MessageType::PAIR_CONFIRM:
            packetType = ProvisioningPacketType::PAIR_CONFIRM;
            return true;
        case MessageType::PAIR_REJECT:
            packetType = ProvisioningPacketType::PAIR_REJECT;
            return true;
        default:
            return false;
    }
}

ProvisioningLamp::ProvisioningLamp(
    const char* hardwareIdValue,
    const char* firmwareVersionValue,
    uint32_t capabilitiesValue,
    ProvisioningStore& storeValue
)
    : capabilities(capabilitiesValue),
      store(storeValue),
      pairingState(PairingState::UNPAIRED),
      pendingMainId(0),
      hasPendingRequest(false) {
    memset(&configuration, 0, sizeof(configuration));
    copyText(hardwareId, sizeof(hardwareId), hardwareIdValue);
    copyText(firmwareVersion, sizeof(firmwareVersion), firmwareVersionValue);

    ProvisioningRecord saved = {};
    if (store.load(saved) && sameText(saved.hardwareId, hardwareId) &&
        saved.pairingState == PairingState::PAIRED && saved.deviceId != 0 &&
        saved.parentMainId != 0) {
        configuration = saved;
        configuration.hardwareId[sizeof(configuration.hardwareId) - 1] = '\0';
        configuration.name[sizeof(configuration.name) - 1] = '\0';
        pairingState = PairingState::PAIRED;
    }
}

ProvisioningPacket ProvisioningLamp::announce() const {
    ProvisioningPacket packet = {};
    packet.type = ProvisioningPacketType::DEVICE_ANNOUNCE;
    packet.role = DeviceRole::LAMP;
    packet.pairingState = pairingState;
    packet.capabilities = capabilities;
    copyText(packet.hardwareId, sizeof(packet.hardwareId), hardwareId);
    copyText(packet.firmwareVersion, sizeof(packet.firmwareVersion), firmwareVersion);
    if (pairingState == PairingState::PAIRED) {
        packet.deviceId = configuration.deviceId;
        packet.parentMainId = configuration.parentMainId;
        copyText(packet.name, sizeof(packet.name), configuration.name);
    }
    return packet;
}

bool ProvisioningLamp::handlePairRequest(
    const ProvisioningPacket& request,
    ProvisioningPacket& rejection
) {
    rejection = {};
    rejection.type = ProvisioningPacketType::PAIR_REJECT;
    rejection.role = DeviceRole::LAMP;
    rejection.pairingState = pairingState;
    copyText(rejection.hardwareId, sizeof(rejection.hardwareId), hardwareId);

    if (request.type != ProvisioningPacketType::PAIR_REQUEST ||
        request.role != DeviceRole::MAIN ||
        !sameText(request.hardwareId, hardwareId) || request.parentMainId == 0) {
        return false;
    }
    if (pairingState == PairingState::PAIRED) {
        return false;
    }
    if (hasPendingRequest && pendingMainId != request.parentMainId) {
        return false;
    }

    pairingState = PairingState::PAIRING;
    hasPendingRequest = true;
    pendingMainId = request.parentMainId;
    return true;
}

bool ProvisioningLamp::handlePairAccept(
    const ProvisioningPacket& acceptance,
    ProvisioningPacket& confirmation
) {
    confirmation = {};
    confirmation.type = ProvisioningPacketType::PAIR_CONFIRM;
    confirmation.role = DeviceRole::LAMP;

    if (acceptance.type != ProvisioningPacketType::PAIR_ACCEPT ||
        acceptance.role != DeviceRole::MAIN ||
        !sameText(acceptance.hardwareId, hardwareId) ||
        acceptance.deviceId == 0 || acceptance.parentMainId == 0) {
        return false;
    }

    if (pairingState == PairingState::PAIRED) {
        if (configuration.deviceId != acceptance.deviceId ||
            configuration.parentMainId != acceptance.parentMainId ||
            !sameText(configuration.name, acceptance.name)) {
            return false;
        }
    } else {
        if (!hasPendingRequest || pendingMainId != acceptance.parentMainId ||
            pairingState != PairingState::PAIRING) {
            return false;
        }

        ProvisioningRecord accepted = {};
        accepted.deviceId = acceptance.deviceId;
        copyText(accepted.hardwareId, sizeof(accepted.hardwareId), hardwareId);
        copyText(accepted.name, sizeof(accepted.name), acceptance.name);
        if (accepted.name[0] == '\0') {
            copyText(accepted.name, sizeof(accepted.name), "LAMP");
        }
        accepted.parentMainId = acceptance.parentMainId;
        accepted.pairingState = PairingState::PAIRED;
        if (!store.save(accepted)) {
            return false;
        }
        configuration = accepted;
        pairingState = PairingState::PAIRED;
        hasPendingRequest = false;
        pendingMainId = 0;
    }

    confirmation.deviceId = configuration.deviceId;
    confirmation.parentMainId = configuration.parentMainId;
    confirmation.pairingState = PairingState::PAIRED;
    copyText(confirmation.hardwareId, sizeof(confirmation.hardwareId), hardwareId);
    copyText(confirmation.name, sizeof(confirmation.name), configuration.name);
    return true;
}

PairingState ProvisioningLamp::state() const {
    return pairingState;
}

MainProvisioning::MainProvisioning(uint32_t mainIdValue, LampRegistry& registryValue)
    : mainId(mainIdValue),
      registry(registryValue),
      discoveredCount(0),
      commissioningEnabled(false),
      hasPendingPairing(false),
      pendingDeviceId(0) {
    memset(discovered, 0, sizeof(discovered));
    pendingHardwareId[0] = '\0';
}

void MainProvisioning::startCommissioning() {
    commissioningEnabled = true;
}

void MainProvisioning::stopCommissioning() {
    commissioningEnabled = false;
}

bool MainProvisioning::commissioningIsEnabled() const {
    return commissioningEnabled;
}

uint8_t MainProvisioning::detectedCount() const {
    return discoveredCount;
}

int8_t MainProvisioning::findDiscovered(const char* hardwareIdValue) const {
    for (uint8_t i = 0; i < discoveredCount; ++i) {
        if (sameText(discovered[i].hardwareId, hardwareIdValue)) {
            return static_cast<int8_t>(i);
        }
    }
    return -1;
}

uint32_t MainProvisioning::findAvailableDeviceId() const {
    for (uint32_t candidate = 1; candidate != 0; ++candidate) {
        if (findLamp(registry, candidate) == nullptr) {
            return candidate;
        }
    }
    return 0;
}

void MainProvisioning::clearPendingPairing() {
    hasPendingPairing = false;
    pendingHardwareId[0] = '\0';
    pendingDeviceId = 0;
}

DiscoveryResult MainProvisioning::discover(
    const ProvisioningPacket& announcement
) {
    if (announcement.type != ProvisioningPacketType::DEVICE_ANNOUNCE ||
        announcement.role != DeviceRole::LAMP ||
        announcement.hardwareId[0] == '\0' ||
        (announcement.pairingState != PairingState::UNPAIRED &&
         announcement.pairingState != PairingState::PAIRING &&
         announcement.pairingState != PairingState::PAIRED)) {
        return DiscoveryResult::INVALID_ANNOUNCEMENT;
    }

    const int8_t previousIndex = findDiscovered(announcement.hardwareId);
    const bool duplicate = previousIndex >= 0;
    if (!duplicate) {
        if (discoveredCount >= MAX_DISCOVERED_LAMPS) {
            return DiscoveryResult::IDENTITY_CONFLICT;
        }
        discovered[discoveredCount++] = announcement;
    } else {
        discovered[previousIndex] = announcement;
    }

    if (announcement.pairingState != PairingState::PAIRED) {
        return duplicate
            ? DiscoveryResult::DUPLICATE_ANNOUNCEMENT
            : DiscoveryResult::DISCOVERED_UNPAIRED;
    }

    if (announcement.deviceId == 0 || announcement.parentMainId == 0) {
        return DiscoveryResult::INVALID_ANNOUNCEMENT;
    }
    if (announcement.parentMainId != mainId) {
        return DiscoveryResult::DISCOVERED_OTHER_MAIN;
    }

    Lamp* byHardwareId = findLampByHardwareId(registry, announcement.hardwareId);
    if (byHardwareId != nullptr) {
        if (byHardwareId->device.id == announcement.deviceId &&
            byHardwareId->identity.parentMainId == mainId &&
            byHardwareId->identity.pairingState == PairingState::PAIRED) {
            return DiscoveryResult::RECOGNIZED_PAIRED;
        }
        return DiscoveryResult::IDENTITY_CONFLICT;
    }
    if (findLamp(registry, announcement.deviceId) != nullptr) {
        return DiscoveryResult::IDENTITY_CONFLICT;
    }

    Lamp restored = {};
    restored.device.id = announcement.deviceId;
    restored.device.name = "LAMP";
    restored.device.role = DeviceRole::LAMP;
    restored.device.status = DeviceStatus::ONLINE;
    restored.device.lastSeen = 0;
    restored.state = {false, 0, false};
    copyText(restored.identity.hardwareId, sizeof(restored.identity.hardwareId),
             announcement.hardwareId);
    copyText(restored.identity.name, sizeof(restored.identity.name),
             announcement.name[0] == '\0' ? "LAMP" : announcement.name);
    restored.identity.parentMainId = mainId;
    restored.identity.pairingState = PairingState::PAIRED;
    if (!addLamp(registry, restored)) {
        return DiscoveryResult::IDENTITY_CONFLICT;
    }
    return DiscoveryResult::RESTORED_PAIRED;
}

bool MainProvisioning::createPairRequest(
    const char* hardwareIdValue,
    ProvisioningPacket& request
) {
    if (!commissioningEnabled || hasPendingPairing ||
        hardwareIdValue == nullptr || hardwareIdValue[0] == '\0') {
        return false;
    }
    const int8_t index = findDiscovered(hardwareIdValue);
    if (index < 0 || discovered[index].pairingState != PairingState::UNPAIRED ||
        findLampByHardwareId(registry, hardwareIdValue) != nullptr) {
        return false;
    }

    request = {};
    request.type = ProvisioningPacketType::PAIR_REQUEST;
    request.role = DeviceRole::MAIN;
    request.parentMainId = mainId;
    copyText(request.hardwareId, sizeof(request.hardwareId), hardwareIdValue);
    copyText(pendingHardwareId, sizeof(pendingHardwareId), hardwareIdValue);
    pendingDeviceId = 0;
    hasPendingPairing = true;
    return true;
}

bool MainProvisioning::createPairAcceptance(
    const char* hardwareIdValue,
    const char* requestedName,
    ProvisioningPacket& acceptance
) {
    if (!commissioningEnabled || !hasPendingPairing ||
        !sameText(pendingHardwareId, hardwareIdValue)) {
        return false;
    }

    const uint32_t assignedId = findAvailableDeviceId();
    if (assignedId == 0) {
        return false;
    }
    acceptance = {};
    acceptance.type = ProvisioningPacketType::PAIR_ACCEPT;
    acceptance.role = DeviceRole::MAIN;
    acceptance.deviceId = assignedId;
    acceptance.parentMainId = mainId;
    acceptance.pairingState = PairingState::PAIRED;
    copyText(acceptance.hardwareId, sizeof(acceptance.hardwareId), pendingHardwareId);
    copyText(acceptance.name, sizeof(acceptance.name), requestedName);
    if (acceptance.name[0] == '\0') {
        copyText(acceptance.name, sizeof(acceptance.name), "LAMP");
    }
    pendingDeviceId = assignedId;
    return true;
}

bool MainProvisioning::confirmPairing(
    const ProvisioningPacket& confirmation
) {
    if (!hasPendingPairing || pendingDeviceId == 0 ||
        confirmation.type != ProvisioningPacketType::PAIR_CONFIRM ||
        confirmation.role != DeviceRole::LAMP ||
        confirmation.pairingState != PairingState::PAIRED ||
        !sameText(confirmation.hardwareId, pendingHardwareId) ||
        confirmation.deviceId != pendingDeviceId ||
        confirmation.parentMainId != mainId) {
        return false;
    }

    Lamp paired = {};
    paired.device.id = confirmation.deviceId;
    paired.device.name = "LAMP";
    paired.device.role = DeviceRole::LAMP;
    paired.device.status = DeviceStatus::ONLINE;
    paired.device.lastSeen = 0;
    paired.state = {false, 0, false};
    copyText(paired.identity.hardwareId, sizeof(paired.identity.hardwareId),
             confirmation.hardwareId);
    copyText(paired.identity.name, sizeof(paired.identity.name),
             confirmation.name[0] == '\0' ? "LAMP" : confirmation.name);
    paired.identity.parentMainId = mainId;
    paired.identity.pairingState = PairingState::PAIRED;
    if (!addLamp(registry, paired)) {
        return false;
    }
    clearPendingPairing();
    return true;
}

bool encodeProvisioningPacket(
    const ProvisioningPacket& packet,
    uint32_t messageId,
    uint32_t sourceId,
    uint32_t destinationId,
    Message& message
) {
    MessageType messageType;
    if (!packetTypeToMessageType(packet.type, messageType) ||
        packet.hardwareId[0] == '\0') {
        return false;
    }
    message = {};
    message.id = messageId;
    message.sourceId = sourceId;
    message.destinationId = destinationId;
    message.type = messageType;
    message.status = MessageStatus::PENDING;
    message.executionStatus = ExecutionStatus::NOT_EXECUTED;
    copyText(message.hardwareId, sizeof(message.hardwareId), packet.hardwareId);
    copyText(message.name, sizeof(message.name), packet.name);
    copyText(message.firmwareVersion, sizeof(message.firmwareVersion),
             packet.firmwareVersion);
    message.parentMainId = packet.parentMainId;
    message.capabilities = packet.capabilities;
    message.provisioningDeviceId = packet.deviceId;
    message.deviceRole = static_cast<uint8_t>(packet.role);
    message.pairingState = static_cast<uint8_t>(packet.pairingState);
    return true;
}

bool decodeProvisioningPacket(
    const Message& message,
    ProvisioningPacket& packet
) {
    ProvisioningPacketType packetType;
    if (!messageTypeToPacketType(message.type, packetType) ||
        message.hardwareId[0] == '\0' ||
        message.deviceRole > static_cast<uint8_t>(DeviceRole::RELAY) ||
        message.pairingState > static_cast<uint8_t>(PairingState::PAIRED)) {
        return false;
    }
    packet = {};
    packet.type = packetType;
    packet.role = static_cast<DeviceRole>(message.deviceRole);
    packet.pairingState = static_cast<PairingState>(message.pairingState);
    packet.deviceId = message.provisioningDeviceId;
    packet.parentMainId = message.parentMainId;
    packet.capabilities = message.capabilities;
    copyText(packet.hardwareId, sizeof(packet.hardwareId), message.hardwareId);
    copyText(packet.name, sizeof(packet.name), message.name);
    copyText(packet.firmwareVersion, sizeof(packet.firmwareVersion),
             message.firmwareVersion);
    return true;
}
