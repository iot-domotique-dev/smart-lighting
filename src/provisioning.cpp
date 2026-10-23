#include <string.h>

#include "provisioning.h"
#include "group_manager.h"
#include "core_module_service.h"

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

static bool hasValidRemovalEnvelope(
    ProvisioningPacketType type,
    PairingState state,
    DeviceRole role,
    uint32_t deviceId,
    uint32_t parentMainId,
    uint32_t sourceId,
    uint32_t destinationId
) {
    switch (type) {
        case ProvisioningPacketType::UNPAIR_REQUEST:
            return role == DeviceRole::MAIN && parentMainId != 0 &&
                   state == PairingState::UNPAIRING &&
                   sourceId == parentMainId && destinationId == deviceId;
        case ProvisioningPacketType::UNPAIR_COMMIT:
            return role == DeviceRole::MAIN && parentMainId != 0 &&
                   state == PairingState::UNPAIRED &&
                   sourceId == parentMainId && destinationId == deviceId;
        case ProvisioningPacketType::UNPAIR_CONFIRM:
            return role == DeviceRole::LAMP && deviceId != 0 &&
                   state == PairingState::UNPAIRING &&
                   sourceId == deviceId && destinationId == parentMainId;
        case ProvisioningPacketType::UNPAIR_REJECT:
            return role == DeviceRole::LAMP && deviceId != 0 &&
                   sourceId == deviceId && destinationId == parentMainId;
        case ProvisioningPacketType::DEVICE_ANNOUNCE:
            if (state != PairingState::UNPAIRING) return true;
            return role == DeviceRole::LAMP && deviceId != 0 &&
                   sourceId == deviceId && destinationId == 0;
        case ProvisioningPacketType::PAIR_REQUEST:
        case ProvisioningPacketType::PAIR_ACCEPT:
        case ProvisioningPacketType::PAIR_CONFIRM:
        case ProvisioningPacketType::PAIR_REJECT:
            return true;
    }
    return false;
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
        case ProvisioningPacketType::UNPAIR_REQUEST:
            messageType = MessageType::UNPAIR_REQUEST;
            return true;
        case ProvisioningPacketType::UNPAIR_CONFIRM:
            messageType = MessageType::UNPAIR_CONFIRM;
            return true;
        case ProvisioningPacketType::UNPAIR_COMMIT:
            messageType = MessageType::UNPAIR_COMMIT;
            return true;
        case ProvisioningPacketType::UNPAIR_REJECT:
            messageType = MessageType::UNPAIR_REJECT;
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
        case MessageType::UNPAIR_REQUEST:
            packetType = ProvisioningPacketType::UNPAIR_REQUEST;
            return true;
        case MessageType::UNPAIR_CONFIRM:
            packetType = ProvisioningPacketType::UNPAIR_CONFIRM;
            return true;
        case MessageType::UNPAIR_COMMIT:
            packetType = ProvisioningPacketType::UNPAIR_COMMIT;
            return true;
        case MessageType::UNPAIR_REJECT:
            packetType = ProvisioningPacketType::UNPAIR_REJECT;
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
      pendingStartedAt(0),
      hasPendingRequest(false),
      hasRemovalReceipt(false) {
    memset(&configuration, 0, sizeof(configuration));
    memset(&removalReceipt, 0, sizeof(removalReceipt));
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

    LampRemovalReceipt receipt = {};
    if (store.loadRemovalReceipt(receipt) &&
        receipt.version == LAMP_REMOVAL_RECEIPT_VERSION &&
        receipt.operationId != 0 && receipt.deviceId != 0 &&
        receipt.parentMainId != 0 && sameText(receipt.hardwareId, hardwareId) &&
        (receipt.stage == LampRemovalReceiptStage::PREPARED ||
         receipt.stage == LampRemovalReceiptStage::CLEARED)) {
        removalReceipt = receipt;
        removalReceipt.hardwareId[sizeof(removalReceipt.hardwareId) - 1] = '\0';
        hasRemovalReceipt = true;
        pairingState = PairingState::UNPAIRING;
        (void)finishPreparedRemoval();
    }
}

ProvisioningPacket ProvisioningLamp::announce() const {
    ProvisioningPacket packet = {};
    packet.type = ProvisioningPacketType::DEVICE_ANNOUNCE;
    packet.role = DeviceRole::LAMP;
    packet.pairingState = hasRemovalReceipt
        ? (configuration.deviceId == 0 ? PairingState::UNPAIRING : PairingState::PAIRED)
        : pairingState;
    packet.capabilities = capabilities;
    // Keep periodic broadcast announcements short enough for APS transmission.
    copyText(packet.hardwareId, sizeof(packet.hardwareId), hardwareId);
    if (hasRemovalReceipt) {
        packet.deviceId = removalReceipt.deviceId;
        packet.parentMainId = removalReceipt.parentMainId;
        packet.transactionId = removalReceipt.operationId;
        if (configuration.deviceId != 0) {
            copyText(packet.name, sizeof(packet.name), configuration.name);
        }
    } else if (pairingState == PairingState::PAIRED) {
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
    expirePendingRequest(millis());
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
    if (pairingState == PairingState::PAIRED || hasRemovalReceipt) {
        return false;
    }
    if (hasPendingRequest && pendingMainId != request.parentMainId) {
        return false;
    }

    if (!hasPendingRequest) {
        pendingStartedAt = millis();
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
    expirePendingRequest(millis());
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
    return hasRemovalReceipt ? PairingState::UNPAIRING : pairingState;
}

bool ProvisioningLamp::finishPreparedRemoval() {
    if (!hasRemovalReceipt) return false;
    if (removalReceipt.stage == LampRemovalReceiptStage::CLEARED) {
        if (configuration.deviceId != 0) {
            const bool sameRecord = configuration.deviceId == removalReceipt.deviceId &&
                configuration.parentMainId == removalReceipt.parentMainId &&
                sameText(configuration.hardwareId, removalReceipt.hardwareId);
            if (!sameRecord || !store.clear()) return false;
            memset(&configuration, 0, sizeof(configuration));
        }
        pairingState = PairingState::UNPAIRING;
        return true;
    }
    const bool exactCurrentRecord = configuration.deviceId == removalReceipt.deviceId &&
        configuration.parentMainId == removalReceipt.parentMainId &&
        sameText(configuration.hardwareId, removalReceipt.hardwareId);
    const bool noCurrentRecord = configuration.deviceId == 0;
    if (!exactCurrentRecord && !noCurrentRecord) return false;
    if (exactCurrentRecord && !store.clear()) return false;
    memset(&configuration, 0, sizeof(configuration));
    pairingState = PairingState::UNPAIRING;
    removalReceipt.stage = LampRemovalReceiptStage::CLEARED;
    if (!store.saveRemovalReceipt(removalReceipt)) {
        removalReceipt.stage = LampRemovalReceiptStage::PREPARED;
        return false;
    }
    return true;
}

bool ProvisioningLamp::retryPreparedRemoval(
    ProvisioningPacket& confirmation
) {
    if (!finishPreparedRemoval() ||
        removalReceipt.stage != LampRemovalReceiptStage::CLEARED) return false;
    confirmation = {};
    confirmation.type = ProvisioningPacketType::UNPAIR_CONFIRM;
    confirmation.role = DeviceRole::LAMP;
    confirmation.pairingState = PairingState::UNPAIRING;
    confirmation.deviceId = removalReceipt.deviceId;
    confirmation.parentMainId = removalReceipt.parentMainId;
    confirmation.transactionId = removalReceipt.operationId;
    copyText(confirmation.hardwareId, sizeof(confirmation.hardwareId), hardwareId);
    return true;
}

bool ProvisioningLamp::handleUnpairRequest(
    const ProvisioningPacket& request,
    ProvisioningPacket& confirmation
) {
    confirmation = {};
    if (request.type != ProvisioningPacketType::UNPAIR_REQUEST ||
        request.role != DeviceRole::MAIN || request.transactionId == 0 ||
        request.pairingState != PairingState::UNPAIRING ||
        request.deviceId == 0 || request.parentMainId == 0 ||
        request.sourceId != request.parentMainId ||
        request.destinationId != request.deviceId ||
        !sameText(request.hardwareId, hardwareId)) return false;

    auto reject = [&]() {
        confirmation = {};
        confirmation.type = ProvisioningPacketType::UNPAIR_REJECT;
        confirmation.role = DeviceRole::LAMP;
        confirmation.deviceId = request.deviceId;
        confirmation.parentMainId = request.parentMainId;
        confirmation.transactionId = request.transactionId;
        confirmation.pairingState = pairingState;
        copyText(confirmation.hardwareId, sizeof(confirmation.hardwareId), hardwareId);
    };

    if (hasRemovalReceipt) {
        if (removalReceipt.operationId != request.transactionId ||
            removalReceipt.deviceId != request.deviceId ||
            removalReceipt.parentMainId != request.parentMainId ||
            !sameText(removalReceipt.hardwareId, request.hardwareId)) {
            reject();
            return false;
        }
        return retryPreparedRemoval(confirmation);
    }

    if (pairingState != PairingState::PAIRED ||
        configuration.deviceId != request.deviceId ||
        configuration.parentMainId != request.parentMainId ||
        !sameText(configuration.hardwareId, request.hardwareId)) {
        reject();
        return false;
    }

    LampRemovalReceipt receipt = {};
    receipt.version = LAMP_REMOVAL_RECEIPT_VERSION;
    receipt.stage = LampRemovalReceiptStage::PREPARED;
    receipt.operationId = request.transactionId;
    receipt.deviceId = configuration.deviceId;
    receipt.parentMainId = configuration.parentMainId;
    copyText(receipt.hardwareId, sizeof(receipt.hardwareId), hardwareId);
    if (!store.saveRemovalReceipt(receipt)) {
        reject();
        return false;
    }
    removalReceipt = receipt;
    hasRemovalReceipt = true;
    pairingState = PairingState::UNPAIRING;
    return retryPreparedRemoval(confirmation);
}

bool ProvisioningLamp::handleUnpairCommit(
    const ProvisioningPacket& commit
) {
    if (!hasRemovalReceipt ||
        commit.type != ProvisioningPacketType::UNPAIR_COMMIT ||
        commit.role != DeviceRole::MAIN ||
        commit.pairingState != PairingState::UNPAIRED ||
        commit.sourceId != commit.parentMainId ||
        commit.destinationId != commit.deviceId ||
        commit.transactionId != removalReceipt.operationId ||
        commit.deviceId != removalReceipt.deviceId ||
        commit.parentMainId != removalReceipt.parentMainId ||
        !sameText(commit.hardwareId, removalReceipt.hardwareId) ||
        removalReceipt.stage != LampRemovalReceiptStage::CLEARED ||
        !store.clearRemovalReceipt()) return false;
    memset(&removalReceipt, 0, sizeof(removalReceipt));
    hasRemovalReceipt = false;
    pairingState = PairingState::UNPAIRED;
    return true;
}

void ProvisioningLamp::expirePendingRequest(uint32_t now) {
    if (!hasPendingRequest ||
        static_cast<uint32_t>(now - pendingStartedAt) <
            PROVISIONING_TRANSACTION_TIMEOUT_MS) {
        return;
    }
    hasPendingRequest = false;
    pendingMainId = 0;
    pendingStartedAt = 0;
    pairingState = PairingState::UNPAIRED;
}

MainProvisioning::MainProvisioning(
    uint32_t mainIdValue,
    LampRegistry& registryValue,
    ProvisioningReservationStore* reservationStoreValue,
    GroupRegistry* groupRegistryValue,
    GroupStore* groupStoreValue
)
    : mainId(mainIdValue),
      registry(registryValue),
      reservationStore(reservationStoreValue),
      groupRegistry(groupRegistryValue),
      groupStore(groupStoreValue),
      reservationStoreReady(false),
      discoveredCount(0),
      commissioningEnabled(false),
      hasPendingPairing(false),
      pendingDeviceId(0),
      pendingStartedAt(0) {
    initializeMainProvisioningReservationState(mainId, reservationState);
    if (reservationStore != nullptr &&
        reservationStore->load(mainId, reservationState) &&
        isValidMainProvisioningReservationState(reservationState, mainId)) {
        reservationStoreReady = true;
    }
    memset(discovered, 0, sizeof(discovered));
    memset(discoveredAt, 0, sizeof(discoveredAt));
    pendingHardwareId[0] = '\0';
    pendingName[0] = '\0';
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

bool MainProvisioning::hasUnpairedCandidate(const char* hardwareIdValue) const {
    if (hardwareIdValue == nullptr || hardwareIdValue[0] == '\0') return false;
    const int8_t index = findDiscovered(hardwareIdValue);
    return index >= 0 &&
        discovered[index].pairingState == PairingState::UNPAIRED;
}

bool MainProvisioning::canPairCandidate(const char* hardwareIdValue) const {
    if (!reservationStoreReady || !commissioningEnabled || hasPendingPairing ||
        hasActiveRemoval() ||
        hardwareIdValue == nullptr || hardwareIdValue[0] == '\0' ||
        pairingCapacityReached(hardwareIdValue)) return false;
    const int8_t index = findDiscovered(hardwareIdValue);
    if (index < 0 || discovered[index].pairingState != PairingState::UNPAIRED ||
        findLampByHardwareId(registry, hardwareIdValue) != nullptr ||
        findAvailableDeviceId() == 0) return false;
    return true;
}

bool MainProvisioning::pairingCapacityReached(
    const char* hardwareIdValue
) const {
    if (!reservationStoreReady || hardwareIdValue == nullptr ||
        hardwareIdValue[0] == '\0') return false;
    if (registry.count >= MAX_LAMPS || findAvailableDeviceId() == 0) return true;
    return findReservationByHardwareId(hardwareIdValue) < 0 &&
           reservationState.count >= MAX_LAMPS;
}

void MainProvisioning::expireUnpairedCandidates(uint32_t now) {
    for (uint8_t index = 0; index < discoveredCount;) {
        if (discovered[index].pairingState == PairingState::PAIRED ||
            now - discoveredAt[index] <= PROVISIONING_TRANSACTION_TIMEOUT_MS) {
            ++index;
            continue;
        }
        for (uint8_t move = index; move + 1 < discoveredCount; ++move) {
            discovered[move] = discovered[move + 1];
            discoveredAt[move] = discoveredAt[move + 1];
        }
        --discoveredCount;
        discovered[discoveredCount] = {};
        discoveredAt[discoveredCount] = 0;
    }
}

void MainProvisioning::clearUnpairedCandidates() {
    for (uint8_t index = 0; index < discoveredCount;) {
        if (discovered[index].pairingState == PairingState::PAIRED) {
            ++index;
            continue;
        }
        for (uint8_t move = index; move + 1 < discoveredCount; ++move) {
            discovered[move] = discovered[move + 1];
            discoveredAt[move] = discoveredAt[move + 1];
        }
        --discoveredCount;
        discovered[discoveredCount] = {};
        discoveredAt[discoveredCount] = 0;
    }
}

void MainProvisioning::expirePendingPairing(uint32_t now) {
    if (!hasPendingPairing ||
        static_cast<uint32_t>(now - pendingStartedAt) <
            PROVISIONING_TRANSACTION_TIMEOUT_MS) {
        return;
    }
    clearPendingPairing(true);
}

void MainProvisioning::abortPendingPairing() {
    clearPendingPairing(true);
}

int8_t MainProvisioning::findDiscovered(const char* hardwareIdValue) const {
    for (uint8_t i = 0; i < discoveredCount; ++i) {
        if (sameText(discovered[i].hardwareId, hardwareIdValue)) {
            return static_cast<int8_t>(i);
        }
    }
    return -1;
}

int8_t MainProvisioning::findReservationByHardwareId(
    const char* hardwareIdValue
) const {
    for (uint8_t i = 0; i < reservationState.count; ++i) {
        if (sameText(reservationState.identities[i].hardwareId, hardwareIdValue)) {
            return static_cast<int8_t>(i);
        }
    }
    return -1;
}

int8_t MainProvisioning::findReservationByDeviceId(uint32_t deviceId) const {
    for (uint8_t i = 0; i < reservationState.count; ++i) {
        if (reservationState.identities[i].deviceId == deviceId) {
            return static_cast<int8_t>(i);
        }
    }
    return -1;
}

uint32_t MainProvisioning::findAvailableDeviceId() const {
    if (!reservationStoreReady || reservationState.highWaterDeviceId == UINT32_MAX) {
        return 0;
    }
    for (uint32_t candidate = reservationState.highWaterDeviceId + 1;
         candidate != 0; ++candidate) {
        bool reservedByDiscoveredPairedDevice = false;
        for (uint8_t i = 0; i < discoveredCount; ++i) {
            if (discovered[i].pairingState == PairingState::PAIRED &&
                discovered[i].deviceId == candidate) {
                reservedByDiscoveredPairedDevice = true;
                break;
            }
        }
        if (findLamp(registry, candidate) == nullptr &&
            !reservedByDiscoveredPairedDevice) {
            return candidate;
        }
    }
    return 0;
}

bool MainProvisioning::saveReservationState(
    const MainProvisioningReservationState& state
) {
    if (!reservationStoreReady || reservationStore == nullptr ||
        !isValidMainProvisioningReservationState(state, mainId)) {
        return false;
    }
    if (!reservationStore->save(state)) {
        reservationStoreReady = false;
        return false;
    }
    reservationState = state;
    return true;
}

bool MainProvisioning::recordPairedIdentity(
    const ProvisioningPacket& announcement
) {
    if (!reservationStoreReady) {
        // Preserve the no-store legacy test mode, but fail closed on a bad NVS blob.
        return reservationStore == nullptr && announcement.deviceId <= MAX_LAMPS;
    }

    if (isRemovedIdentity(announcement.deviceId, announcement.hardwareId)) {
        return false;
    }

    const int8_t byHardware =
        findReservationByHardwareId(announcement.hardwareId);
    const int8_t byId = findReservationByDeviceId(announcement.deviceId);
    if (byHardware >= 0) {
        return reservationState.identities[byHardware].deviceId ==
                   announcement.deviceId &&
               (byId < 0 || byId == byHardware);
    }
    if (byId >= 0 || announcement.deviceId > reservationState.highWaterDeviceId) {
        // An ID above the legacy range must have been reserved before PAIR_ACCEPT.
        return false;
    }
    if (announcement.deviceId > MAX_LAMPS ||
        reservationState.count >= MAX_LAMPS) {
        return false;
    }

    MainProvisioningReservationState next = reservationState;
    ProvisioningIdentityReservation& identity = next.identities[next.count++];
    identity.deviceId = announcement.deviceId;
    copyText(identity.hardwareId, sizeof(identity.hardwareId),
             announcement.hardwareId);
    if (!saveReservationState(next)) {
        // Do not block a known legacy lamp, but disable future ID allocations.
        reservationStoreReady = false;
    }
    return true;
}

void MainProvisioning::clearPendingPairing(bool requireFreshAnnouncement) {
    if (requireFreshAnnouncement && pendingHardwareId[0] != '\0') {
        const int8_t index = findDiscovered(pendingHardwareId);
        if (index >= 0 &&
            discovered[index].pairingState == PairingState::UNPAIRED) {
            discovered[index].pairingState = PairingState::PAIRING;
        }
    }
    hasPendingPairing = false;
    pendingHardwareId[0] = '\0';
    pendingName[0] = '\0';
    pendingDeviceId = 0;
    pendingStartedAt = 0;
}

DiscoveryResult MainProvisioning::discover(
    const ProvisioningPacket& announcement
) {
    expireUnpairedCandidates(millis());
    if (announcement.type != ProvisioningPacketType::DEVICE_ANNOUNCE ||
        announcement.role != DeviceRole::LAMP ||
        announcement.hardwareId[0] == '\0' ||
        (announcement.pairingState != PairingState::UNPAIRED &&
         announcement.pairingState != PairingState::PAIRING &&
         announcement.pairingState != PairingState::PAIRED &&
         announcement.pairingState != PairingState::UNPAIRING)) {
        return DiscoveryResult::INVALID_ANNOUNCEMENT;
    }
    if (announcement.pairingState == PairingState::PAIRED &&
        (announcement.deviceId == 0 || announcement.parentMainId == 0)) {
        return DiscoveryResult::INVALID_ANNOUNCEMENT;
    }
    if (announcement.pairingState == PairingState::UNPAIRING &&
        (announcement.deviceId == 0 || announcement.parentMainId != mainId ||
         announcement.transactionId == 0)) {
        return DiscoveryResult::INVALID_ANNOUNCEMENT;
    }
    if (announcement.pairingState == PairingState::UNPAIRING) {
        if (reservationState.removalOperationId == announcement.transactionId &&
            reservationState.removalDeviceId == announcement.deviceId &&
            reservationState.removalParentMainId == announcement.parentMainId &&
            sameText(reservationState.removalHardwareId, announcement.hardwareId) &&
            (reservationState.removalState == MAIN_REMOVAL_REQUESTED ||
             reservationState.removalState == MAIN_REMOVAL_SUCCEEDED)) {
            return reservationState.removalState == MAIN_REMOVAL_REQUESTED
                ? DiscoveryResult::REMOVAL_PENDING
                : DiscoveryResult::REMOVAL_RECONCILED;
        }
        return DiscoveryResult::IDENTITY_CONFLICT;
    }

    const int8_t previousIndex = findDiscovered(announcement.hardwareId);
    const bool duplicate = previousIndex >= 0;
    if (!duplicate) {
        if (discoveredCount >= MAX_DISCOVERED_LAMPS) {
            return DiscoveryResult::CANDIDATE_CAPACITY;
        }
        discovered[discoveredCount++] = announcement;
        discoveredAt[discoveredCount - 1] = millis();
    } else {
        discovered[previousIndex] = announcement;
        discoveredAt[previousIndex] = millis();
    }

    if (announcement.pairingState != PairingState::PAIRED) {
        return duplicate
            ? DiscoveryResult::DUPLICATE_ANNOUNCEMENT
            : DiscoveryResult::DISCOVERED_UNPAIRED;
    }

    if (announcement.parentMainId != mainId) {
        return DiscoveryResult::DISCOVERED_OTHER_MAIN;
    }
    if (reservationState.removalState == MAIN_REMOVAL_REQUESTED &&
        reservationState.removalDeviceId == announcement.deviceId &&
        sameText(reservationState.removalHardwareId, announcement.hardwareId)) {
        Lamp* target = findLamp(registry, announcement.deviceId);
        if (target != nullptr) target->identity.removalPending = true;
        return DiscoveryResult::REMOVAL_PENDING;
    }
    if (!recordPairedIdentity(announcement)) {
        return DiscoveryResult::IDENTITY_CONFLICT;
    }

    Lamp* byHardwareId = findLampByHardwareId(registry, announcement.hardwareId);
    if (byHardwareId != nullptr) {
        if (byHardwareId->device.id == announcement.deviceId &&
            byHardwareId->identity.parentMainId == mainId &&
            byHardwareId->identity.pairingState == PairingState::PAIRED) {
            byHardwareId->device.capabilities = announcement.capabilities;
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
    restored.device.lastSeen = millis();
    restored.device.capabilities = announcement.capabilities;
    restored.state = {false, 0, false};
    copyText(restored.identity.hardwareId, sizeof(restored.identity.hardwareId),
             announcement.hardwareId);
    copyText(restored.identity.name, sizeof(restored.identity.name),
             announcement.name[0] == '\0' ? "LAMP" : announcement.name);
    restored.identity.parentMainId = mainId;
    restored.identity.pairingState = PairingState::PAIRED;
    restored.identity.removalPending = false;
    if (!addLamp(registry, restored)) {
        return DiscoveryResult::IDENTITY_CONFLICT;
    }
    return DiscoveryResult::RESTORED_PAIRED;
}

bool MainProvisioning::createPairRequest(
    const char* hardwareIdValue,
    ProvisioningPacket& request
) {
    expirePendingPairing(millis());
    if (!reservationStoreReady || !commissioningEnabled || hasPendingPairing ||
        hardwareIdValue == nullptr || hardwareIdValue[0] == '\0') {
        return false;
    }
    const int8_t index = findDiscovered(hardwareIdValue);
    if (index < 0 || !canPairCandidate(hardwareIdValue)) return false;

    request = {};
    request.type = ProvisioningPacketType::PAIR_REQUEST;
    request.role = DeviceRole::MAIN;
    request.parentMainId = mainId;
    copyText(request.hardwareId, sizeof(request.hardwareId), hardwareIdValue);
    copyText(pendingHardwareId, sizeof(pendingHardwareId), hardwareIdValue);
    pendingName[0] = '\0';
    pendingDeviceId = 0;
    pendingStartedAt = millis();
    hasPendingPairing = true;
    return true;
}

bool MainProvisioning::createPairAcceptance(
    const char* hardwareIdValue,
    const char* requestedName,
    ProvisioningPacket& acceptance
) {
    expirePendingPairing(millis());
    if (!reservationStoreReady || !commissioningEnabled || !hasPendingPairing ||
        pendingDeviceId != 0 ||
        !sameText(pendingHardwareId, hardwareIdValue)) {
        return false;
    }

    const uint32_t assignedId = findAvailableDeviceId();
    if (assignedId == 0) {
        return false;
    }

    MainProvisioningReservationState next = reservationState;
    const int8_t identityIndex = findReservationByHardwareId(pendingHardwareId);
    ProvisioningIdentityReservation* identity = nullptr;
    if (identityIndex >= 0) {
        identity = &next.identities[identityIndex];
    } else {
        if (next.count >= MAX_LAMPS) {
            clearPendingPairing(true);
            return false;
        }
        identity = &next.identities[next.count++];
        memset(identity, 0, sizeof(*identity));
        copyText(identity->hardwareId, sizeof(identity->hardwareId),
                 pendingHardwareId);
    }
    identity->deviceId = assignedId;
    next.highWaterDeviceId = assignedId;
    if (!saveReservationState(next)) {
        clearPendingPairing(true);
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
    copyText(pendingName, sizeof(pendingName), acceptance.name);
    pendingDeviceId = assignedId;
    return true;
}

bool MainProvisioning::confirmPairing(
    const ProvisioningPacket& confirmation
) {
    expirePendingPairing(millis());
    if (!hasPendingPairing || pendingDeviceId == 0 ||
        confirmation.type != ProvisioningPacketType::PAIR_CONFIRM ||
        confirmation.role != DeviceRole::LAMP ||
        confirmation.pairingState != PairingState::PAIRED ||
        !sameText(confirmation.hardwareId, pendingHardwareId) ||
        !sameText(confirmation.name, pendingName) ||
        confirmation.deviceId != pendingDeviceId ||
        confirmation.parentMainId != mainId) {
        return false;
    }

    Lamp paired = {};
    paired.device.id = confirmation.deviceId;
    paired.device.name = "LAMP";
    paired.device.role = DeviceRole::LAMP;
    paired.device.status = DeviceStatus::ONLINE;
    paired.device.lastSeen = millis();
    const int8_t discoveredIndex = findDiscovered(confirmation.hardwareId);
    if (discoveredIndex >= 0) {
        paired.device.capabilities = discovered[discoveredIndex].capabilities;
    }
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
    if (discoveredIndex >= 0) {
        discovered[discoveredIndex] = confirmation;
        discoveredAt[discoveredIndex] = millis();
    }
    clearPendingPairing(false);
    return true;
}

bool MainProvisioning::beginRemoval(uint32_t operationId, uint32_t deviceId,
                                    uint32_t coreDeviceId) {
    if (!reservationStoreReady || operationId == 0 || deviceId == 0 ||
        coreDeviceId == 0 || hasPendingPairing ||
        reservationState.removalState == MAIN_REMOVAL_REQUESTED) return false;
    Lamp* lamp = findLamp(registry, deviceId);
    const uint32_t mainCoreId = makeCoreModuleId(CORE_LOGICAL_ID, mainId);
    if (lamp == nullptr || lamp->identity.pairingState != PairingState::PAIRED ||
        lamp->identity.removalPending || lamp->identity.parentMainId != mainId ||
        lamp->device.status != DeviceStatus::ONLINE ||
        lamp->identity.hardwareId[0] == '\0' ||
        coreDeviceId != makeCoreModuleId(mainCoreId, deviceId)) return false;
    const int8_t reservationIndex = findReservationByHardwareId(
        lamp->identity.hardwareId);
    if (reservationIndex < 0 ||
        reservationState.identities[reservationIndex].deviceId != deviceId) return false;

    MainProvisioningReservationState next = reservationState;
    next.removalState = MAIN_REMOVAL_REQUESTED;
    next.removalOperationId = operationId;
    next.removalDeviceId = deviceId;
    next.removalParentMainId = mainId;
    next.removalCoreDeviceId = coreDeviceId;
    copyText(next.removalHardwareId, sizeof(next.removalHardwareId),
             lamp->identity.hardwareId);
    if (!saveReservationState(next)) return false;
    lamp->identity.removalPending = true;
    return true;
}

bool MainProvisioning::buildRemovalPacket(ProvisioningPacket& packet) const {
    if (reservationState.removalState != MAIN_REMOVAL_REQUESTED) return false;
    packet = {};
    packet.type = ProvisioningPacketType::UNPAIR_REQUEST;
    packet.role = DeviceRole::MAIN;
    packet.deviceId = reservationState.removalDeviceId;
    packet.parentMainId = reservationState.removalParentMainId;
    packet.transactionId = reservationState.removalOperationId;
    packet.pairingState = PairingState::UNPAIRING;
    copyText(packet.hardwareId, sizeof(packet.hardwareId),
             reservationState.removalHardwareId);
    return true;
}

bool MainProvisioning::reconcileRemovalPacket(
    const ProvisioningPacket& packet,
    ProvisioningPacket& response,
    bool& removalCompleted
) {
    removalCompleted = false;
    response = {};
    const bool matches = reservationState.removalOperationId != 0 &&
        packet.transactionId == reservationState.removalOperationId &&
        packet.deviceId == reservationState.removalDeviceId &&
        packet.parentMainId == reservationState.removalParentMainId &&
        sameText(packet.hardwareId, reservationState.removalHardwareId);
    if (!matches) return false;

    if (packet.role != DeviceRole::LAMP) return false;
    const bool expectedTransport =
        ((packet.type == ProvisioningPacketType::UNPAIR_CONFIRM ||
          packet.type == ProvisioningPacketType::UNPAIR_REJECT) &&
         packet.sourceId == packet.deviceId &&
         packet.destinationId == packet.parentMainId) ||
        (packet.type == ProvisioningPacketType::DEVICE_ANNOUNCE &&
         packet.pairingState == PairingState::UNPAIRING &&
         packet.sourceId == packet.deviceId && packet.destinationId == 0);
    if (!expectedTransport) return false;
    if (packet.type == ProvisioningPacketType::UNPAIR_REJECT) {
        if (reservationState.removalState != MAIN_REMOVAL_REQUESTED) return false;
        MainProvisioningReservationState failed = reservationState;
        failed.removalState = MAIN_REMOVAL_FAILED;
        if (!saveReservationState(failed)) return false;
        Lamp* lamp = findLamp(registry, failed.removalDeviceId);
        if (lamp != nullptr) lamp->identity.removalPending = false;
        return true;
    }

    if (reservationState.removalState == MAIN_REMOVAL_SUCCEEDED) {
        if (packet.type != ProvisioningPacketType::UNPAIR_CONFIRM &&
            !(packet.type == ProvisioningPacketType::DEVICE_ANNOUNCE &&
              packet.pairingState == PairingState::UNPAIRING)) return false;
        if (packet.type == ProvisioningPacketType::UNPAIR_CONFIRM &&
            packet.pairingState != PairingState::UNPAIRING) return false;
        response = packet;
        response.type = ProvisioningPacketType::UNPAIR_COMMIT;
        response.role = DeviceRole::MAIN;
        response.pairingState = PairingState::UNPAIRED;
        removalCompleted = true;
        return true;
    }

    if (reservationState.removalState != MAIN_REMOVAL_REQUESTED ||
        (packet.type != ProvisioningPacketType::UNPAIR_CONFIRM &&
         !(packet.type == ProvisioningPacketType::DEVICE_ANNOUNCE &&
           packet.pairingState == PairingState::UNPAIRING))) return false;
    if (packet.type == ProvisioningPacketType::UNPAIR_CONFIRM &&
        packet.pairingState != PairingState::UNPAIRING) return false;

    MainProvisioningReservationState next = reservationState;
    const int8_t index = findReservationByHardwareId(
        reservationState.removalHardwareId);
    if (index < 0 ||
        reservationState.identities[index].deviceId != reservationState.removalDeviceId) {
        return false;
    }
    if (next.removalDeviceId <= MAX_LAMPS) {
        next.retiredLegacyIds |= static_cast<uint16_t>(
            1U << (next.removalDeviceId - 1U));
    }
    for (uint8_t move = static_cast<uint8_t>(index); move + 1 < next.count; ++move)
        next.identities[move] = next.identities[move + 1];
    --next.count;
    memset(&next.identities[next.count], 0, sizeof(next.identities[next.count]));
    next.removalState = MAIN_REMOVAL_SUCCEEDED;
    // Persist group cleanup before committing the irreversible pairing removal.
    // If group storage fails, leave the pairing reservation and live lamp intact;
    // retrying the same removal evidence is safe and idempotent.
    if (groupRegistry != nullptr && groupStore != nullptr &&
        !removeLampFromAllGroupsPersisted(
            *groupRegistry, *groupStore, reservationState.removalDeviceId)) {
        return false;
    }
    if (!saveReservationState(next)) return false;

    (void)removeLamp(registry, reservationState.removalDeviceId);
    response = packet;
    response.type = ProvisioningPacketType::UNPAIR_COMMIT;
    response.role = DeviceRole::MAIN;
    response.pairingState = PairingState::UNPAIRED;
    removalCompleted = true;
    return true;
}

bool MainProvisioning::getRemovalStatus(
    uint32_t operationId,
    MainProvisioningReservationState& state
) const {
    if (operationId == 0 || operationId != reservationState.removalOperationId ||
        reservationState.removalState == MAIN_REMOVAL_NONE) return false;
    state = reservationState;
    return true;
}

bool MainProvisioning::isRemovalTarget(uint32_t deviceId) const {
    return reservationState.removalState == MAIN_REMOVAL_REQUESTED &&
           reservationState.removalDeviceId == deviceId;
}

bool MainProvisioning::isRemovedIdentity(uint32_t deviceId,
                                         const char* hardwareIdValue) const {
    if (deviceId == 0 || hardwareIdValue == nullptr || hardwareIdValue[0] == '\0')
        return false;
    if (deviceId <= MAX_LAMPS &&
        (reservationState.retiredLegacyIds &
         static_cast<uint16_t>(1U << (deviceId - 1U))) != 0) return true;
    if (deviceId > MAX_LAMPS &&
        deviceId <= reservationState.highWaterDeviceId &&
        findReservationByDeviceId(deviceId) < 0) return true;
    return reservationState.removalState == MAIN_REMOVAL_SUCCEEDED &&
           reservationState.removalDeviceId == deviceId &&
           sameText(reservationState.removalHardwareId, hardwareIdValue);
}

bool MainProvisioning::hasActiveRemoval() const {
    return reservationState.removalState == MAIN_REMOVAL_REQUESTED;
}

uint32_t MainProvisioning::removalOperationId() const {
    return reservationState.removalOperationId;
}

uint32_t MainProvisioning::removalDeviceId() const {
    return reservationState.removalDeviceId;
}

uint8_t MainProvisioning::removalState() const {
    return reservationState.removalState;
}

bool encodeProvisioningPacket(
    const ProvisioningPacket& packet,
    uint32_t messageId,
    uint32_t sourceId,
    uint32_t destinationId,
    Message& message
) {
    MessageType messageType;
    const bool requiresTransaction =
        packet.type == ProvisioningPacketType::UNPAIR_REQUEST ||
        packet.type == ProvisioningPacketType::UNPAIR_CONFIRM ||
        packet.type == ProvisioningPacketType::UNPAIR_COMMIT ||
        packet.type == ProvisioningPacketType::UNPAIR_REJECT ||
        (packet.type == ProvisioningPacketType::DEVICE_ANNOUNCE &&
         packet.pairingState == PairingState::UNPAIRING);
    if (!packetTypeToMessageType(packet.type, messageType) ||
        packet.hardwareId[0] == '\0' ||
        (requiresTransaction && (packet.transactionId == 0 ||
                                 packet.deviceId == 0 ||
                                 packet.parentMainId == 0)) ||
        !hasValidRemovalEnvelope(packet.type, packet.pairingState, packet.role,
            packet.deviceId, packet.parentMainId, sourceId, destinationId)) {
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
    message.value = static_cast<int32_t>(packet.transactionId);
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
        message.pairingState > static_cast<uint8_t>(PairingState::UNPAIRING)) {
        return false;
    }
    packet = {};
    packet.type = packetType;
    packet.role = static_cast<DeviceRole>(message.deviceRole);
    packet.pairingState = static_cast<PairingState>(message.pairingState);
    packet.transactionId = static_cast<uint32_t>(message.value);
    packet.deviceId = message.provisioningDeviceId;
    packet.parentMainId = message.parentMainId;
    packet.sourceId = message.sourceId;
    packet.destinationId = message.destinationId;
    packet.capabilities = message.capabilities;
    copyText(packet.hardwareId, sizeof(packet.hardwareId), message.hardwareId);
    copyText(packet.name, sizeof(packet.name), message.name);
    copyText(packet.firmwareVersion, sizeof(packet.firmwareVersion),
             message.firmwareVersion);
    const bool requiresTransaction =
        packet.type == ProvisioningPacketType::UNPAIR_REQUEST ||
        packet.type == ProvisioningPacketType::UNPAIR_CONFIRM ||
        packet.type == ProvisioningPacketType::UNPAIR_COMMIT ||
        packet.type == ProvisioningPacketType::UNPAIR_REJECT ||
        (packet.type == ProvisioningPacketType::DEVICE_ANNOUNCE &&
         packet.pairingState == PairingState::UNPAIRING);
    if (requiresTransaction && (packet.transactionId == 0 ||
        packet.deviceId == 0 || packet.parentMainId == 0)) return false;
    return hasValidRemovalEnvelope(packet.type, packet.pairingState, packet.role,
        packet.deviceId, packet.parentMainId, packet.sourceId,
        packet.destinationId);
}
