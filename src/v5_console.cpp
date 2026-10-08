#include <Arduino.h>

#include "v5_console.h"

#if defined(SMART_LIGHTING_ZIGBEE)

#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "roles.h"
#include "v5_provisioning_runtime.h"
#include "core_command_protocol.h"

namespace {
void executeLine(char* line) {
    char* command = strtok(line, " \t");
    if (command == nullptr) {
        return;
    }

    if (strcmp(command, "help") == 0) {
        Serial.println("[CONSOLE] pair <hardwareId> <name>");
        Serial.println("[CONSOLE] power <deviceId> on|off");
        if (DEVICE_ROLE == DeviceRole::LAMP)
            Serial.println("[CONSOLE] drop_ack (diagnostic: lose next V7.2 execution ACK)");
        return;
    }

    if (strcmp(command, "drop_ack") == 0 && DEVICE_ROLE == DeviceRole::LAMP) {
        if (strtok(nullptr, " \t\r\n") != nullptr) {
            Serial.println("[CONSOLE] usage: drop_ack");
            return;
        }
        requestDropNextCoreExecutionAck();
        Serial.println("[CONSOLE] next V7.2 execution ACK will be intentionally lost");
        return;
    }

    if (strcmp(command, "pair") == 0) {
        char* hardwareId = strtok(nullptr, " \t");
        char* name = strtok(nullptr, "\r\n");
        if (hardwareId == nullptr || name == nullptr ||
            !commissionLampV5(hardwareId, name)) {
            Serial.println("[CONSOLE] pairing request failed; check discovery and role");
        } else {
            Serial.println("[CONSOLE] explicit V4 pairing flow started");
        }
        return;
    }

    if (strcmp(command, "power") == 0) {
        char* deviceIdText = strtok(nullptr, " \t");
        char* state = strtok(nullptr, " \t\r\n");
        if (deviceIdText == nullptr || state == nullptr ||
            (strcmp(state, "on") != 0 && strcmp(state, "off") != 0)) {
            Serial.println("[CONSOLE] usage: power <deviceId> on|off");
            return;
        }
        const uint32_t deviceId = static_cast<uint32_t>(strtoul(deviceIdText, nullptr, 10));
        if (!setLampPowerV5(deviceId, strcmp(state, "on") == 0)) {
            Serial.println("[CONSOLE] SET_POWER could not be sent");
        } else {
            Serial.println("[CONSOLE] SET_POWER sent; waiting for existing ACK/retry path");
        }
        return;
    }

    Serial.println("[CONSOLE] unknown command; type help");
}

void consoleTask(void*) {
    if (DEVICE_ROLE == DeviceRole::MAIN) {
        Serial.println("[CONSOLE] type help");
    }
    char line[96] = {};
    size_t length = 0;
    while (true) {
        const int input = getchar();
        if (input == EOF) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        if (input == '\r') {
            continue;
        }
        if (input == '\n') {
            line[length] = '\0';
            executeLine(line);
            length = 0;
            continue;
        }
        if (length + 1 < sizeof(line)) {
            line[length++] = static_cast<char>(input);
        }
    }
}
}  // namespace

void startV5Console() {
    (void)xTaskCreate(consoleTask, "v5_console", 4096, nullptr, 3, nullptr);
}

#else

void startV5Console() {}

#endif
