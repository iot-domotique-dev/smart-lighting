#include <Arduino.h>

#include "lamp_controller.h"
#include "lamp_hardware.h"


bool setLampPower(
    Lamp& lamp,
    bool state
) {

    if (!writeLampHardwarePower(state)) {
        Serial.println("[LAMP] GPIO power write failed");
        return false;
    }
    lamp.state.power = state;
    if (lampHardwareSupportsBrightness()) {
        lamp.state.brightness = state
            ? lamp.state.lastNonzeroBrightness : 0;
    }

    Serial.print("Power -> ");
    Serial.println(
        state ? "ON" : "OFF"
    );
    return true;
}


bool setLampBrightness(
    Lamp& lamp,
    int32_t brightness
) {
    if (brightness < 0 || brightness > 100 ||
        !lampHardwareSupportsBrightness()) {
        Serial.println("[LAMP] brightness command rejected: unsupported or out of range");
        return false;
    }
    if (!writeLampHardwareBrightness(static_cast<uint8_t>(brightness))) {
        Serial.println("[LAMP] PWM brightness write failed");
        return false;
    }

    lamp.state.brightness = static_cast<uint8_t>(brightness);
    lamp.state.power = brightness != 0;
    if (brightness != 0) {
        lamp.state.lastNonzeroBrightness = static_cast<uint8_t>(brightness);
    }

    Serial.print("Brightness -> ");
    Serial.print(brightness);
    Serial.println("%");
    return true;
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
