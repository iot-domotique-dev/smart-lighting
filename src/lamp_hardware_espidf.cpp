#include <Arduino.h>

#include "lamp_hardware.h"

#if defined(SMART_LIGHTING_ZIGBEE) && defined(DEVICE_ROLE_LAMP) && \
    defined(SMART_LIGHTING_LED_GPIO)
#include "driver/gpio.h"

namespace {
bool outputInitialized = false;
}

void initializeLampHardware() {
    gpio_config_t config = {};
    config.pin_bit_mask = 1ULL << SMART_LIGHTING_LED_GPIO;
    config.mode = GPIO_MODE_OUTPUT;
    config.pull_up_en = GPIO_PULLUP_DISABLE;
    config.pull_down_en = GPIO_PULLDOWN_DISABLE;
    config.intr_type = GPIO_INTR_DISABLE;
    const esp_err_t result = gpio_config(&config);
    if (result == ESP_OK) {
        outputInitialized = true;
        (void)gpio_set_level(static_cast<gpio_num_t>(SMART_LIGHTING_LED_GPIO), 0);
        Serial.println("[LAMP] GPIO output initialized LOW");
    } else {
        Serial.print("[LAMP] GPIO initialization failed: ");
        Serial.println(static_cast<int32_t>(result));
    }
}

void writeLampHardwarePower(bool enabled) {
    if (outputInitialized) {
        (void)gpio_set_level(static_cast<gpio_num_t>(SMART_LIGHTING_LED_GPIO),
                             enabled ? 1 : 0);
    }
}

#else

void initializeLampHardware() {
#if defined(SMART_LIGHTING_ZIGBEE) && defined(DEVICE_ROLE_LAMP)
    Serial.println(
        "[LAMP] GPIO output not configured; define SMART_LIGHTING_LED_GPIO after board/pin selection"
    );
#endif
}

void writeLampHardwarePower(bool) {}

#endif
