#pragma once

#include <Arduino.h>

// ============================================================
// CONFIGURATION
// ============================================================

constexpr uint8_t MAX_GROUP_LAMPS = 10;
constexpr uint8_t MAX_GROUP_NAME_LENGTH = 32;  // Includes the terminating NUL.


// ============================================================
// GROUPE DE LAMPES
//
// lampIds are MAIN-local provisioning IDs, not V7 IDs, table indexes,
// Zigbee addresses, or CORE-derived IDs. The name is owned by this value.
// ============================================================

struct LampGroup {
    uint32_t id;
    char name[MAX_GROUP_NAME_LENGTH];
    uint32_t lampIds[MAX_GROUP_LAMPS];
    uint8_t lampCount;
    uint32_t revision;
};
