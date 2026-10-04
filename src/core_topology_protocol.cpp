#include <string.h>

#include "core_topology_protocol.h"

#include "core_module_service.h"
#include "device.h"
#include "lamp.h"
#include "roles.h"

namespace {
bool copyBoundedName(char* destination, size_t capacity, const char* source) {
    if (destination == nullptr || capacity == 0 || source == nullptr ||
        source[0] == '\0') {
        return false;
    }
    size_t length = 0;
    while (length < capacity && source[length] != '\0') {
        ++length;
    }
    if (length >= capacity) {
        return false;
    }
    memcpy(destination, source, length + 1);
    return true;
}
}  // namespace

bool makeCoreMainAnnouncement(
    uint32_t localId,
    const char* name,
    uint32_t capabilities,
    Message& message
) {
    if (localId == 0 || name == nullptr || name[0] == '\0') {
        return false;
    }
    message = {};
    message.sourceId = localId;
    message.destinationId = 0;
    message.type = MessageType::STATE;
    message.commandType = CORE_TOPOLOGY_MAIN_ANNOUNCEMENT;
    message.value = static_cast<int32_t>(CORE_LOGICAL_ID);
    message.capabilities = capabilities;
    message.parentMainId = CORE_LOGICAL_ID;
    message.deviceRole = static_cast<uint8_t>(DeviceRole::MAIN);
    return copyBoundedName(message.name, sizeof(message.name), name);
}

bool coreAnnouncementFromZigbeeMessage(
    const Message& message,
    uint32_t mainCoreId,
    uint32_t mainLocalId,
    CoreLinkPacket& packet
) {
    packet = {};
    packet.sequence = static_cast<uint16_t>(message.id);

    if (message.type == MessageType::STATE &&
        message.commandType == CORE_TOPOLOGY_MAIN_ANNOUNCEMENT &&
        message.destinationId == 0 && message.sourceId != 0 &&
        message.parentMainId == CORE_LOGICAL_ID &&
        message.deviceRole == static_cast<uint8_t>(DeviceRole::MAIN)) {
        packet.type = CoreLinkMessageType::MODULE_ANNOUNCEMENT;
        packet.localId = message.sourceId;
        packet.parentId = CORE_LOGICAL_ID;
        packet.capabilities = message.capabilities;
        packet.role = message.deviceRole;
        return copyBoundedName(packet.name, sizeof(packet.name), message.name);
    }

    if (message.type == MessageType::DEVICE_ANNOUNCE && mainCoreId != 0 &&
        mainLocalId != 0 &&
        message.deviceRole == static_cast<uint8_t>(DeviceRole::LAMP) &&
        message.pairingState == static_cast<uint8_t>(PairingState::PAIRED) &&
        message.provisioningDeviceId != 0 &&
        message.parentMainId == mainLocalId) {
        packet.type = CoreLinkMessageType::MODULE_ANNOUNCEMENT;
        packet.localId = message.provisioningDeviceId;
        packet.parentId = mainCoreId;
        packet.capabilities = message.capabilities;
        packet.role = message.deviceRole;
        return copyBoundedName(packet.name, sizeof(packet.name), message.name);
    }
    return false;
}

bool makeCoreIdAssignedMessage(
    uint32_t localId,
    uint32_t coreId,
    uint8_t resultCode,
    Message& message
) {
    if (localId == 0) return false;
    message = {};
    message.sourceId = CORE_LOGICAL_ID;
    message.destinationId = localId;
    message.type = MessageType::STATE;
    message.commandType = CORE_TOPOLOGY_ID_ASSIGNED;
    message.value = static_cast<int32_t>(coreId);
    message.value2 = resultCode;
    message.deviceRole = static_cast<uint8_t>(DeviceRole::CORE);
    return true;
}

bool readCoreIdAssignedMessage(
    const Message& message,
    uint32_t expectedLocalId,
    uint32_t& coreId,
    uint8_t& resultCode
) {
    if (message.type != MessageType::STATE ||
        message.commandType != CORE_TOPOLOGY_ID_ASSIGNED ||
        message.sourceId != CORE_LOGICAL_ID ||
        message.destinationId != expectedLocalId || expectedLocalId == 0 ||
        message.deviceRole != static_cast<uint8_t>(DeviceRole::CORE)) {
        return false;
    }
    coreId = static_cast<uint32_t>(message.value);
    resultCode = static_cast<uint8_t>(message.value2);
    return true;
}

bool makeCoreIdAckMessage(
    uint32_t localId,
    uint32_t coreId,
    bool accepted,
    Message& message
) {
    if (localId == 0 || coreId == 0) return false;
    message = {};
    message.sourceId = localId;
    message.destinationId = CORE_LOGICAL_ID;
    message.type = MessageType::STATE;
    message.commandType = CORE_TOPOLOGY_ID_ACK;
    message.value = static_cast<int32_t>(coreId);
    message.value2 = accepted ? 1 : 0;
    message.deviceRole = static_cast<uint8_t>(DeviceRole::MAIN);
    return true;
}

bool readCoreIdAckMessage(
    const Message& message,
    uint32_t expectedLocalId,
    uint32_t expectedCoreId,
    bool& accepted
) {
    if (message.type != MessageType::STATE ||
        message.commandType != CORE_TOPOLOGY_ID_ACK ||
        message.sourceId != expectedLocalId || expectedLocalId == 0 ||
        message.destinationId != CORE_LOGICAL_ID ||
        static_cast<uint32_t>(message.value) != expectedCoreId ||
        expectedCoreId == 0 ||
        message.deviceRole != static_cast<uint8_t>(DeviceRole::MAIN)) {
        return false;
    }
    accepted = message.value2 == 1;
    return true;
}
