#include <Arduino.h>

#include "event_processor.h"

#include "event.h"

void processEvents(
    EventBus& bus,
    AutomationRegistry& automations,
    SceneRegistry& scenes,
    GroupRegistry& groups,
    LampRegistry& lamps,
    const AutomationContext& context
) {
    Event event;

    while (
        consumeEvent(
            bus,
            event
        )
    ) {

        Serial.println();
        Serial.println(
            ">>> TRAITEMENT EVENEMENT"
        );

        printEvent(event);

        switch (event.type) {

            case EventType::LIGHT_LEVEL_CHANGED:

            case EventType::PRESENCE_CHANGED:

            case EventType::TIME_CHANGED:

            case EventType::DEVICE_STATE_CHANGED:

                processAutomations(
                    automations,
                    scenes,
                    groups,
                    lamps,
                    context
                );

                break;

            case EventType::LAMP_ONLINE:

            case EventType::LAMP_OFFLINE:

                Serial.println(
                    "Evenement etat lampe traite"
                );

                break;

            case EventType::DEVICE_ONLINE:

            case EventType::DEVICE_OFFLINE:

                Serial.println(
                    "Evenement etat device traite"
                );

                break;

            case EventType::COMMAND_RECEIVED:

                Serial.println(
                    "Evenement commande recu"
                );

                break;
        }
    }
}
