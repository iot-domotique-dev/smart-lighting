#pragma once

#include <stdint.h>

struct CoreConsoleRequest {
    bool query;
    uint32_t id;
    bool power;
};

bool startCoreConsole();
bool receiveCoreConsoleRequest(CoreConsoleRequest& request);
