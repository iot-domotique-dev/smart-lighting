#pragma once

#include <Arduino.h>

#include "message.h"

// A CORE ID and a V5 pairing ID may have the same numeric value.
// This scope selects a physical route; it is not part of the wire payload.
enum class CommunicationRouteScope : uint8_t {
    LOCAL_DEVICE,
    CORE
};


/*
 * ============================================================
 * COMMUNICATION TRANSPORT
 * ============================================================
 *
 * Cette interface représente le moyen physique/logique
 * utilisé pour transporter les messages.
 *
 * L'application ne doit jamais dépendre directement de :
 *
 * - ESP-NOW
 * - Zigbee
 * - Wi-Fi
 * - UART
 *
 * Elle utilise uniquement cette interface.
 */

class CommunicationTransportInterface {

public:

    virtual ~CommunicationTransportInterface() = default;


    /*
     * Initialisation du transport.
     */
    virtual bool begin() = 0;


    /*
     * Envoi.
     */
    virtual bool send(
        const Message& message
    ) = 0;

    /* Select a physical next hop without changing the logical message IDs. */
    virtual bool sendVia(
        const Message& message,
        uint32_t nextHopLogicalId,
        CommunicationRouteScope scope = CommunicationRouteScope::LOCAL_DEVICE
    ) {
        (void)nextHopLogicalId;
        (void)scope;
        return send(message);
    }


    /*
     * Réception.
     */
    virtual bool receive(
        Message& message
    ) = 0;


    /*
     * Transport disponible ?
     */
    virtual bool isReady() const = 0;


    /*
     * Nom du transport.
     */
    virtual const char* name() const = 0;
};
