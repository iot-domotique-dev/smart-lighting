#include <Arduino.h>

#include "action_executor.h"

#include "scene_executor.h"
#include "group_command_handler.h"
#include "lamp_controller.h"


static ExecutionStatus convertSceneStatus(
    SceneExecutionStatus status
) {

    switch (status) {

        case SceneExecutionStatus::EXECUTED:
            return ExecutionStatus::EXECUTED;

        case SceneExecutionStatus::PARTIAL:
            return ExecutionStatus::PARTIAL;

        case SceneExecutionStatus::FAILED:
            return ExecutionStatus::FAILED;
    }

    return ExecutionStatus::FAILED;
}


ExecutionStatus executeAction(
    const Action& action,
    SceneRegistry& scenes,
    GroupRegistry& groups,
    LampRegistry& lamps
) {

    switch (action.type) {

        // ====================================================
        // SCENE
        // ====================================================

        case ActionType::EXECUTE_SCENE: {

            Serial.print(
                "Action : EXECUTE_SCENE -> "
            );

            Serial.println(
                action.targetId
            );


            SceneExecutionResult result;


            SceneExecutionStatus sceneStatus =
                executeScene(
                    scenes,
                    groups,
                    lamps,
                    action.targetId,
                    result
                );


            return convertSceneStatus(
                sceneStatus
            );
        }


        // ====================================================
        // LAMPE
        // ====================================================

        case ActionType::SET_LAMP_POWER:
        case ActionType::SET_LAMP_BRIGHTNESS:
        case ActionType::SET_LAMP_AUTOMATIC: {

            Lamp* lamp =
                findLamp(
                    lamps,
                    action.targetId
                );


            if (lamp == nullptr) {

                Serial.println(
                    "Lampe introuvable"
                );

                return ExecutionStatus::FAILED;
            }


            if (
                lamp->device.status
                != DeviceStatus::ONLINE
            ) {

                Serial.print(
                    "Lampe OFFLINE - commande ignoree : "
                );

                Serial.println(
                    lamp->device.name
                );

                return ExecutionStatus::FAILED;
            }


            switch (action.type) {

                case ActionType::SET_LAMP_POWER:

                    if (!setLampPower(
                        *lamp,
                        action.value != 0
                    )) {
                        return ExecutionStatus::FAILED;
                    }

                    break;


                case ActionType::SET_LAMP_BRIGHTNESS:

                    setLampBrightness(
                        *lamp,
                        action.value
                    );

                    break;


                case ActionType::SET_LAMP_AUTOMATIC:

                    setLampAutomatic(
                        *lamp,
                        action.value != 0
                    );

                    break;


                default:

                    return ExecutionStatus::FAILED;
            }


            return ExecutionStatus::EXECUTED;
        }


        // ====================================================
        // GROUPE
        // ====================================================

        case ActionType::SET_GROUP_POWER:
        case ActionType::SET_GROUP_BRIGHTNESS: {

            Command command = {

                0,

                action.targetId,

                action.type ==
                    ActionType::SET_GROUP_POWER
                    ? CommandType::SET_GROUP_POWER
                    : CommandType::SET_GROUP_BRIGHTNESS,

                action.value,

                CommandStatus::PENDING
            };


            bool success =
                executeGroupCommand(
                    groups,
                    lamps,
                    command
                );


            if (!success) {

                return ExecutionStatus::FAILED;
            }


            if (
                command.status ==
                CommandStatus::PARTIAL
            ) {

                return ExecutionStatus::PARTIAL;
            }


            if (
                command.status ==
                CommandStatus::EXECUTED
            ) {

                return ExecutionStatus::EXECUTED;
            }


            return ExecutionStatus::FAILED;
        }
    }


    return ExecutionStatus::FAILED;
}
