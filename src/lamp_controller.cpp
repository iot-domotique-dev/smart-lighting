#include <Arduino.h>

#include "lamp_controller.h"
#include "lamp_hardware.h"


void setLampPower(
    Lamp& lamp,
    bool state
) {

    lamp.state.power = state;
    writeLampHardwarePower(state);

    Serial.print("Power -> ");
    Serial.println(
        state ? "ON" : "OFF"
    );
}


void setLampBrightness(
    Lamp& lamp,
    int32_t brightness
) {
    const int32_t clampedBrightness =
        brightness < 0
            ? 0
            : (brightness > 100 ? 100 : brightness);

    lamp.state.brightness = static_cast<uint8_t>(clampedBrightness);

    Serial.print("Brightness -> ");
    Serial.print(clampedBrightness);
    Serial.println("%");
}


void setLampAutomatic(
    Lamp& lamp,
    bool enabled
) {

    lamp.state.automatic = enabled;

    Serial.print("Automatic -> ");
    Serial.println(
        enabled ? "ON" : "OFF"
    );
}


void printLampState(
    const Lamp& lamp
) {

    Serial.println();
    Serial.println("===== ETAT LAMPE =====");

    Serial.print("ID          : ");
    Serial.println(lamp.device.id);

    Serial.print("Nom         : ");
    Serial.println(lamp.device.name);

    Serial.print("Status      : ");

    Serial.println(
        lamp.device.status == DeviceStatus::ONLINE
            ? "ONLINE"
            : "OFFLINE"
    );

    Serial.print("Power       : ");
    Serial.println(
        lamp.state.power
            ? "ON"
            : "OFF"
    );

    Serial.print("Brightness  : ");
    Serial.print(lamp.state.brightness);
    Serial.println("%");

    Serial.print("Automatic   : ");

    Serial.println(
        lamp.state.automatic
            ? "OUI"
            : "NON"
    );

    Serial.println("======================");
}
