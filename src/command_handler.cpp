#include <Arduino.h>

#include "command_handler.h"
#include "lamp_controller.h"


bool executeCommand(
    Lamp& lamp,
    Command& command
) {

    Serial.print("Commande #");
    Serial.print(command.id);
    Serial.print(" : ");


    switch (command.type) {

        case CommandType::SET_POWER:

            Serial.println(
                "SET_POWER"
            );

            if (!setLampPower(
                lamp,
                command.value != 0
            )) {
                command.status = CommandStatus::FAILED;
                return false;
            }

            command.status =
                CommandStatus::EXECUTED;

            return true;


        case CommandType::SET_BRIGHTNESS:

            Serial.println(
                "SET_BRIGHTNESS"
            );

            if (!setLampBrightness(
                lamp,
                command.value
            )) {
                command.status = CommandStatus::FAILED;
                return false;
            }

            command.status =
                CommandStatus::EXECUTED;

            return true;


        case CommandType::SET_AUTOMATIC:

            Serial.println(
                "SET_AUTOMATIC"
            );

            setLampAutomatic(
                lamp,
                command.value != 0
            );

            command.status =
                CommandStatus::EXECUTED;

            return true;


        default:

            Serial.println(
                "COMMANDE INCONNUE"
            );

            command.status =
                CommandStatus::FAILED;

            return false;
    }
}
