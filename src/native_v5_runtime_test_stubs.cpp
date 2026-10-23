/* Native-only fakes. This file is included by the native PlatformIO source
 * filter and never by an ESP-IDF firmware profile. */
#include <string.h>

#include <Arduino.h>

#include "communication.h"
#include "device_hardware_id.h"
#include "zigbee_transport.h"

bool communicationReady(const Communication& communication) {
    return communication.state == CommunicationState::READY &&
        communication.transport != nullptr && communication.transport->isReady();
}

bool readDeviceHardwareId(char* output, size_t capacity) {
    constexpr char TEST_HARDWARE_ID[] = "C6-TEST-MAIN";
    if (output == nullptr || capacity < sizeof(TEST_HARDWARE_ID)) return false;
    memcpy(output, TEST_HARDWARE_ID, sizeof(TEST_HARDWARE_ID));
    return true;
}

bool openZigbeeJoinWindow(uint8_t) { return true; }
bool closeZigbeeJoinWindow() { return true; }
bool hasZigbeeRouteForHardwareId(const char*) { return true; }
bool forgetZigbeeRoute(uint32_t, const char*, CommunicationRouteScope) { return true; }
