#include <Arduino.h>

#include "event_bus.h"

void initEventBus(EventBus& bus) {
    bus.head = 0;
    bus.tail = 0;
    bus.count = 0;
}

bool isEventBusEmpty(
    const EventBus& bus
) {
    return bus.count == 0;
}

bool isEventBusFull(
    const EventBus& bus
) {
    return bus.count >= MAX_EVENTS;
}

bool publishEvent(
    EventBus& bus,
    const Event& event
) {
    if (isEventBusFull(bus)) {
        Serial.println(
            "EVENT BUS PLEIN"
        );

        return false;
    }

    bus.events[bus.tail] = event;

    bus.tail++;

    if (bus.tail >= MAX_EVENTS) {
        bus.tail = 0;
    }

    bus.count++;

    return true;
}

bool consumeEvent(
    EventBus& bus,
    Event& event
) {
    if (isEventBusEmpty(bus)) {
        return false;
    }

    event = bus.events[bus.head];

    bus.head++;

    if (bus.head >= MAX_EVENTS) {
        bus.head = 0;
    }

    bus.count--;

    return true;
}

void printEvent(
    const Event& event
) {
    Serial.println();
    Serial.println(
        "===== EVENT ====="
    );

    Serial.print("ID        : ");
    Serial.println(event.id);

    Serial.print("Type      : ");

    switch (event.type) {

        case EventType::LIGHT_LEVEL_CHANGED:
            Serial.println(
                "LIGHT_LEVEL_CHANGED"
            );
            break;

        case EventType::PRESENCE_CHANGED:
            Serial.println(
                "PRESENCE_CHANGED"
            );
            break;

        case EventType::TIME_CHANGED:
            Serial.println(
                "TIME_CHANGED"
            );
            break;

        case EventType::LAMP_ONLINE:
            Serial.println(
                "LAMP_ONLINE"
            );
            break;

        case EventType::LAMP_OFFLINE:
            Serial.println(
                "LAMP_OFFLINE"
            );
            break;

        case EventType::COMMAND_RECEIVED:
            Serial.println(
                "COMMAND_RECEIVED"
            );
            break;

        case EventType::DEVICE_STATE_CHANGED:
            Serial.println(
                "DEVICE_STATE_CHANGED"
            );
            break;

        case EventType::DEVICE_ONLINE:
            Serial.println(
                "DEVICE_ONLINE"
            );
            break;

        case EventType::DEVICE_OFFLINE:
            Serial.println(
                "DEVICE_OFFLINE"
            );
            break;
    }

    Serial.print("Source ID : ");
    Serial.println(event.sourceId);

    Serial.print("Value     : ");
    Serial.println(event.value);

    Serial.print("Value 2   : ");
    Serial.println(event.value2);

    Serial.print("Timestamp : ");
    Serial.println(event.timestamp);

    Serial.println(
        "================="
    );
}

void printEventBus(
    const EventBus& bus
) {
    Serial.println();
    Serial.println(
        "===== EVENT BUS ====="
    );

    Serial.print("Nombre : ");
    Serial.println(bus.count);

    Serial.print("Head   : ");
    Serial.println(bus.head);

    Serial.print("Tail   : ");
    Serial.println(bus.tail);

    Serial.println(
        "====================="
    );
}
