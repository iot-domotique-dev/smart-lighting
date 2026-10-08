#pragma once

#include <Arduino.h>

#include "communication_transport.h"


enum class CommunicationTransportType {

    SIMULATION,

    ZIGBEE,

    CORE_UART
};


enum class CommunicationState {

    INITIALIZING,

    READY,

    ERROR
};


struct Communication {

    CommunicationTransportType type;

    CommunicationState state;

    uint32_t localDeviceId;

    CommunicationTransportInterface*
        transport;
};


/*
 * Initialisation.
 */
bool initCommunication(
    Communication& communication,
    CommunicationTransportType type,
    uint32_t localDeviceId
);


/*
 * Envoi.
 */
bool sendMessage(
    Communication& communication,
    Message message
);

inline bool sendMessageVia(
    Communication& communication,
    Message message,
    uint32_t nextHopLogicalId,
    CommunicationRouteScope scope = CommunicationRouteScope::LOCAL_DEVICE
) {
    if (nextHopLogicalId == 0 || communication.transport == nullptr ||
        communication.state != CommunicationState::READY ||
        !communication.transport->isReady()) {
        return false;
    }
    if (message.timestamp == 0) message.timestamp = millis();
    message.status = MessageStatus::SENT;
    return communication.transport->sendVia(message, nextHopLogicalId, scope);
}


/*
 * Réception.
 */
bool receiveMessage(
    Communication& communication,
    Message& message
);


/*
 * Etat.
 */
bool communicationReady(
    const Communication& communication
);


/*
 * Debug.
 */
void printCommunicationStatus(
    const Communication& communication
);
