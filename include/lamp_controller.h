#pragma once

#include "lamp.h"


bool setLampPower(
    Lamp& lamp,
    bool state
);


bool setLampBrightness(
    Lamp& lamp,
    int32_t brightness
);


void setLampAutomatic(
    Lamp& lamp,
    bool enabled
);


void printLampState(
    const Lamp& lamp
);
