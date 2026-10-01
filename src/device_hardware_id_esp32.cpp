#include <stdio.h>

#include "device_hardware_id.h"

#if defined(SMART_LIGHTING_ZIGBEE)
#include "esp_err.h"
#include "esp_mac.h"

bool readDeviceHardwareId(char* output, size_t outputCapacity) {
    if (output == nullptr || outputCapacity < 20) {
        return false;
    }

    uint8_t ieeeAddress[8] = {};
    if (esp_read_mac(ieeeAddress, ESP_MAC_IEEE802154) != ESP_OK) {
        output[0] = '\0';
        return false;
    }

    const int written = snprintf(
        output, outputCapacity,
        "C6-%02X%02X%02X%02X%02X%02X%02X%02X",
        ieeeAddress[0], ieeeAddress[1], ieeeAddress[2], ieeeAddress[3],
        ieeeAddress[4], ieeeAddress[5], ieeeAddress[6], ieeeAddress[7]);
    return written > 0 && static_cast<size_t>(written) < outputCapacity;
}
#else

bool readDeviceHardwareId(char* output, size_t outputCapacity) {
    if (output != nullptr && outputCapacity > 0) {
        output[0] = '\0';
    }
    return false;
}

#endif
