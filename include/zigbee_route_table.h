#pragma once

#include "communication_transport.h"

constexpr uint8_t ZIGBEE_ROUTE_TABLE_CAPACITY = 10;

/* Physical routes are scoped because CORE 1 and a lamp's local V5 ID 1
 * are different peers. This cache never changes application message IDs. */
class ZigbeeRouteTable {
private:
    struct Route {
        uint32_t logicalId;
        char hardwareId[32];
        uint16_t shortAddress;
        CommunicationRouteScope scope;
        bool used;
    };
    Route routes[ZIGBEE_ROUTE_TABLE_CAPACITY] = {};

    bool findInScope(const Message& message, CommunicationRouteScope scope,
                     uint16_t& address) const;

public:
    static CommunicationRouteScope destinationScope(const Message& message);
    void clear();
    void remember(const Message& message, uint16_t physicalSender);
    bool find(const Message& message, uint16_t& address) const;
    bool find(uint32_t nextHop, CommunicationRouteScope scope,
              uint16_t& address) const;
};
