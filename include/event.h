#pragma once

#include <Arduino.h>

enum class EventType {
    LIGHT_LEVEL_CHANGED,
    PRESENCE_CHANGED,
    TIME_CHANGED,

    LAMP_ONLINE,
    LAMP_OFFLINE,

    COMMAND_RECEIVED,
    DEVICE_STATE_CHANGED,

    DEVICE_ONLINE,
    DEVICE_OFFLINE
};

struct Event {
    uint32_t id;
    EventType type;

    uint32_t sourceId;

    int32_t value;
    int32_t value2;

    uint32_t timestamp;
};
