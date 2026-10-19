#include <Arduino.h>

#include "group_command_handler.h"
#include "lamp_controller.h"


bool executeGroupCommand(
    GroupRegistry& groups,
    LampRegistry& lamps,
    Command& command
) {

    LampGroup* group =
        findGroup(
            groups,
            command.targetId
        );


    if (group == nullptr) {

        Serial.print(
            "Groupe introuvable : "
        );

        Serial.println(
            command.targetId
        );

        command.status =
            CommandStatus::FAILED;

        return false;
    }


    uint8_t executedCount = 0;
    uint8_t skippedCount = 0;


    for (
        uint8_t i = 0;
        i < group->lampCount;
        i++
    ) {

        Lamp* lamp =
            findLamp(
                lamps,
                group->lampIds[i]
            );


        if (lamp == nullptr) {

            Serial.print(
                "Lampe introuvable : "
            );

            Serial.println(
                group->lampIds[i]
            );

            skippedCount++;

            continue;
        }


        if (
            lamp->device.status ==
            DeviceStatus::OFFLINE
        ) {

            Serial.print(
                "Lampe OFFLINE - commande ignoree : "
            );

            Serial.println(
                lamp->device.name
            );

            skippedCount++;

            continue;
        }


        switch (
            command.type
        ) {

            case CommandType::SET_GROUP_POWER:

                if (!setLampPower(
                    *lamp,
                    command.value != 0
                )) {
                    skippedCount++;
                    break;
                }

                executedCount++;

                break;


            case CommandType::SET_GROUP_BRIGHTNESS:

                if (!setLampBrightness(
                    *lamp,
                    command.value
                )) {
                    skippedCount++;
                    break;
                }

                executedCount++;

                break;


            default:

                Serial.println(
                    "Type de commande groupe invalide"
                );

                command.status =
                    CommandStatus::FAILED;

                return false;
        }
    }


    if (
        executedCount == 0
    ) {

        command.status =
            CommandStatus::FAILED;

        return false;
    }


    if (
        skippedCount > 0
    ) {

        command.status =
            CommandStatus::PARTIAL;

        return true;
    }


    command.status =
        CommandStatus::EXECUTED;

    return true;
}
