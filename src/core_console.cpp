#include "core_console.h"

#if defined(SMART_LIGHTING_CORE_WIFI)
#include <Arduino.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

namespace {
QueueHandle_t requests = nullptr;

bool parseId(const char* text, uint32_t& id) {
    if (text == nullptr || text[0] < '0' || text[0] > '9') return false;
    char* end = nullptr;
    errno = 0;
    const unsigned long value = strtoul(text, &end, 10);
    if (errno != 0 || *end != '\0' || value == 0 || value > UINT32_MAX) return false;
    id = static_cast<uint32_t>(value);
    return true;
}

void executeLine(char* line) {
    char* saved = nullptr;
    const char* command = strtok_r(line, " \t", &saved);
    if (command == nullptr) return;
    if (strcmp(command, "help") == 0) {
        Serial.println("[CORE-CONSOLE] power <V7 lamp id> on|off");
        Serial.println("[CORE-CONSOLE] command <message id>");
        return;
    }
    CoreConsoleRequest request = {};
    const char* idText = strtok_r(nullptr, " \t", &saved);
    const char* state = strtok_r(nullptr, " \t", &saved);
    if (strcmp(command, "power") == 0 && parseId(idText, request.id) &&
        state != nullptr && (strcmp(state, "on") == 0 || strcmp(state, "off") == 0) &&
        strtok_r(nullptr, " \t", &saved) == nullptr) {
        request.power = strcmp(state, "on") == 0;
    } else if (strcmp(command, "command") == 0 &&
               parseId(idText, request.id) && state == nullptr) {
        request.query = true;
    } else {
        Serial.println("[CORE-CONSOLE] invalid command; type help");
        return;
    }
    if (xQueueSend(requests, &request, 0) != pdTRUE)
        Serial.println("[CORE-CONSOLE] queue full; request refused");
}

void consoleTask(void*) {
    Serial.println("[CORE-CONSOLE] type help; commands use global V7 lamp ids");
    char line[64] = {};
    size_t length = 0;
    bool overflow = false;
    for (;;) {
        const int input = getchar();
        if (input == EOF) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        if (input == '\r') continue;
        if (input == '\n') {
            line[length] = '\0';
            if (overflow) Serial.println("[CORE-CONSOLE] line too long; request refused");
            else executeLine(line);
            length = 0;
            overflow = false;
        } else if (length + 1 < sizeof(line)) {
            line[length++] = static_cast<char>(input);
        } else {
            overflow = true;
        }
    }
}
}  // namespace

bool startCoreConsole() {
    if (requests != nullptr) return true;
    requests = xQueueCreate(8, sizeof(CoreConsoleRequest));
    if (requests == nullptr) return false;
    if (xTaskCreate(consoleTask, "core_console", 4096, nullptr, 3, nullptr) != pdPASS) {
        vQueueDelete(requests);
        requests = nullptr;
        return false;
    }
    return true;
}

bool receiveCoreConsoleRequest(CoreConsoleRequest& request) {
    return requests != nullptr && xQueueReceive(requests, &request, 0) == pdTRUE;
}
#else
bool startCoreConsole() { return false; }
bool receiveCoreConsoleRequest(CoreConsoleRequest&) { return false; }
#endif
