#include <Arduino.h>

#include "message_id_generator.h"

namespace { uint32_t nextId = 1; }

void seedMessageIds(uint32_t firstId) { nextId = firstId != 0 ? firstId : 1; }

uint32_t generateMessageId() {
    const uint32_t result = nextId++;
    if (nextId == 0) nextId = 1;
    return result;
}
