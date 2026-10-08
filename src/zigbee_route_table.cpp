#include <string.h>

#include "zigbee_route_table.h"

#include "core_command_protocol.h"
#include "core_module_service.h"
#include "core_topology_protocol.h"

namespace {
bool isCoreSender(const Message& message) {
    return message.sourceId == CORE_LOGICAL_ID &&
        (isCoreCommandMessage(message) || isCoreCommandReply(message) ||
         (message.type == MessageType::STATE &&
          message.commandType == CORE_TOPOLOGY_ID_ASSIGNED &&
          message.deviceRole == static_cast<uint8_t>(DeviceRole::CORE)));
}

bool isCoreDestination(const Message& message) {
    return isCoreCommandReply(message) ||
        (message.type == MessageType::STATE &&
         message.commandType == CORE_TOPOLOGY_ID_ACK &&
         message.deviceRole == static_cast<uint8_t>(DeviceRole::MAIN) &&
         message.destinationId == CORE_LOGICAL_ID);
}
}

void ZigbeeRouteTable::clear() {
    memset(routes, 0, sizeof(routes));
}

void ZigbeeRouteTable::remember(const Message& message, uint16_t physicalSender) {
    const CommunicationRouteScope scope = isCoreSender(message)
        ? CommunicationRouteScope::CORE : CommunicationRouteScope::LOCAL_DEVICE;
    const bool lampExecutionAck = isCoreCommandReply(message) &&
        message.provisioningDeviceId != 0 &&
        message.sourceId != message.parentMainId &&
        message.sourceId != CORE_LOGICAL_ID;
    const uint32_t logicalId = lampExecutionAck
        ? message.provisioningDeviceId : message.sourceId;

    int8_t freeIndex = -1;
    int8_t matchIndex = -1;
    for (uint8_t i = 0; i < ZIGBEE_ROUTE_TABLE_CAPACITY; ++i) {
        if (!routes[i].used && freeIndex < 0) {
            freeIndex = static_cast<int8_t>(i);
        }
        if (!routes[i].used || routes[i].scope != scope) continue;
        if ((logicalId != 0 && routes[i].logicalId == logicalId) ||
            (message.hardwareId[0] != '\0' &&
             strncmp(routes[i].hardwareId, message.hardwareId,
                     sizeof(routes[i].hardwareId)) == 0)) {
            matchIndex = static_cast<int8_t>(i);
            break;
        }
    }

    const int8_t index = matchIndex >= 0 ? matchIndex : freeIndex;
    if (index < 0) return;
    Route& route = routes[index];
    route.logicalId = logicalId;
    route.shortAddress = physicalSender;
    route.scope = scope;
    route.used = true;
    if (message.hardwareId[0] != '\0') {
        strncpy(route.hardwareId, message.hardwareId, sizeof(route.hardwareId) - 1);
        route.hardwareId[sizeof(route.hardwareId) - 1] = '\0';
    }
}

bool ZigbeeRouteTable::findInScope(const Message& message,
    CommunicationRouteScope scope, uint16_t& address) const {
    for (uint8_t i = 0; i < ZIGBEE_ROUTE_TABLE_CAPACITY; ++i) {
        const Route& route = routes[i];
        if (!route.used || route.scope != scope) continue;
        const bool logicalMatch = message.destinationId != 0 &&
                                  route.logicalId == message.destinationId;
        const bool hardwareMatch = message.hardwareId[0] != '\0' &&
            strncmp(route.hardwareId, message.hardwareId,
                    sizeof(route.hardwareId)) == 0;
        if (logicalMatch || hardwareMatch) {
            address = route.shortAddress;
            return true;
        }
    }
    return false;
}

CommunicationRouteScope ZigbeeRouteTable::destinationScope(const Message& message) {
    return isCoreDestination(message)
        ? CommunicationRouteScope::CORE : CommunicationRouteScope::LOCAL_DEVICE;
}

bool ZigbeeRouteTable::find(const Message& message, uint16_t& address) const {
    return findInScope(message, destinationScope(message), address);
}

bool ZigbeeRouteTable::find(uint32_t nextHop, CommunicationRouteScope scope,
                          uint16_t& address) const {
    Message route = {};
    route.destinationId = nextHop;
    return findInScope(route, scope, address);
}
