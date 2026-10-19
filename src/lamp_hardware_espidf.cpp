#include <Arduino.h>

#include "lamp_hardware.h"
#include "lamp_pwm_mapping.h"

#if defined(SMART_LIGHTING_ZIGBEE) && defined(DEVICE_ROLE_LAMP) && \
    defined(SMART_LIGHTING_LED_GPIO)
#include "driver/gpio.h"
#if defined(SMART_LIGHTING_LAMP_PWM_ENABLED)
#include "driver/ledc.h"
#if !defined(SMART_LIGHTING_LED_PWM_ACTIVE_LOW)
#error "Set SMART_LIGHTING_LED_PWM_ACTIVE_LOW after checking GPIO18 LED polarity."
#endif
#if (SMART_LIGHTING_LED_PWM_ACTIVE_LOW != 0) && \
    (SMART_LIGHTING_LED_PWM_ACTIVE_LOW != 1)
#error "SMART_LIGHTING_LED_PWM_ACTIVE_LOW must be 0 or 1."
#endif
#if !defined(SMART_LIGHTING_LED_PWM_MAX_DUTY_PERCENT)
#error "Set SMART_LIGHTING_LED_PWM_MAX_DUTY_PERCENT after measuring the resistor and checking LED current."
#endif
#if (SMART_LIGHTING_LED_PWM_MAX_DUTY_PERCENT < 1) || \
    (SMART_LIGHTING_LED_PWM_MAX_DUTY_PERCENT > 100)
#error "SMART_LIGHTING_LED_PWM_MAX_DUTY_PERCENT must be from 1 to 100."
#endif
#endif

namespace {
bool outputInitialized = false;
uint8_t lastNonzeroBrightness = 100;
#if defined(SMART_LIGHTING_LAMP_PWM_ENABLED)
bool pwmOutputReady = false;

constexpr ledc_mode_t PWM_MODE = LEDC_LOW_SPEED_MODE;
constexpr ledc_timer_t PWM_TIMER = LEDC_TIMER_0;
constexpr ledc_channel_t PWM_CHANNEL = LEDC_CHANNEL_0;
constexpr ledc_timer_bit_t PWM_RESOLUTION = LEDC_TIMER_10_BIT;
constexpr uint32_t PWM_MAX_DUTY = (1U << 10) - 1U;
constexpr uint8_t PWM_ALLOWED_MAX_PERCENT =
    SMART_LIGHTING_LED_PWM_MAX_DUTY_PERCENT;

uint32_t brightnessDuty(uint8_t percent) {
    return lampPwmDutyFromPercent(PWM_MAX_DUTY,
                                  PWM_ALLOWED_MAX_PERCENT, percent);
}

bool writePwmDuty(uint8_t percent) {
    const esp_err_t setResult = ledc_set_duty(
        PWM_MODE, PWM_CHANNEL, brightnessDuty(percent));
    if (setResult != ESP_OK) {
        Serial.print("[LAMP] LEDC set duty failed: ");
        Serial.println(static_cast<int32_t>(setResult));
        return false;
    }
    const esp_err_t updateResult = ledc_update_duty(PWM_MODE, PWM_CHANNEL);
    if (updateResult != ESP_OK) {
        Serial.print("[LAMP] LEDC update duty failed: ");
        Serial.println(static_cast<int32_t>(updateResult));
        return false;
    }
    return true;
}
#endif
}  // namespace

void initializeLampHardware() {
    outputInitialized = false;
#if defined(SMART_LIGHTING_LAMP_PWM_ENABLED)
    pwmOutputReady = false;
#endif
    lastNonzeroBrightness = 100;

    gpio_config_t config = {};
    config.pin_bit_mask = 1ULL << SMART_LIGHTING_LED_GPIO;
    config.mode = GPIO_MODE_OUTPUT;
    config.pull_up_en = GPIO_PULLUP_DISABLE;
    config.pull_down_en = GPIO_PULLDOWN_DISABLE;
    config.intr_type = GPIO_INTR_DISABLE;
    const esp_err_t result = gpio_config(&config);
    if (result != ESP_OK) {
        Serial.print("[LAMP] GPIO initialization failed: ");
        Serial.println(static_cast<int32_t>(result));
        return;
    }
    outputInitialized = true;
    int inactiveLevel = 0;
#if defined(SMART_LIGHTING_LAMP_PWM_ENABLED)
    inactiveLevel = lampPwmOffLevel(SMART_LIGHTING_LED_PWM_ACTIVE_LOW != 0);
#endif
    if (gpio_set_level(static_cast<gpio_num_t>(SMART_LIGHTING_LED_GPIO),
                       inactiveLevel) != ESP_OK) {
        outputInitialized = false;
        Serial.println("[LAMP] GPIO could not be placed in the LED-OFF state at startup");
        return;
    }

#if defined(SMART_LIGHTING_LAMP_PWM_ENABLED)
    ledc_timer_config_t timer = {};
    timer.speed_mode = PWM_MODE;
    timer.duty_resolution = PWM_RESOLUTION;
    timer.timer_num = PWM_TIMER;
    timer.freq_hz = 5000;
    timer.clk_cfg = LEDC_AUTO_CLK;
    if (ledc_timer_config(&timer) != ESP_OK) {
        Serial.println("[LAMP] LEDC timer initialization failed; keeping digital power output");
        return;
    }

    ledc_channel_config_t channel = {};
    channel.gpio_num = SMART_LIGHTING_LED_GPIO;
    channel.speed_mode = PWM_MODE;
    channel.channel = PWM_CHANNEL;
    channel.timer_sel = PWM_TIMER;
    channel.duty = 0;
    channel.hpoint = 0;
    channel.flags.output_invert = lampPwmOutputInvert(
        SMART_LIGHTING_LED_PWM_ACTIVE_LOW != 0);
    if (ledc_channel_config(&channel) != ESP_OK) {
        Serial.println("[LAMP] LEDC channel initialization failed; keeping digital power output");
        return;
    }
    pwmOutputReady = true;
    Serial.println("[LAMP] LEDC output ready; brightness 100 maps to configured safe ceiling");
#else
    Serial.println("[LAMP] GPIO output initialized LOW (digital profile; PWM disabled)");
#endif
}

bool lampHardwareSupportsBrightness() {
#if defined(SMART_LIGHTING_LAMP_PWM_ENABLED)
    return outputInitialized && pwmOutputReady;
#else
    return false;
#endif
}

bool writeLampHardwarePower(bool enabled) {
    if (!outputInitialized) return false;
#if defined(SMART_LIGHTING_LAMP_PWM_ENABLED)
    if (pwmOutputReady) {
        return writePwmDuty(enabled ? lastNonzeroBrightness : 0);
    }
#endif
    int level = enabled ? 1 : 0;
#if defined(SMART_LIGHTING_LAMP_PWM_ENABLED)
    level = lampPwmPowerLevel(SMART_LIGHTING_LED_PWM_ACTIVE_LOW != 0,
                              enabled);
#endif
    return gpio_set_level(static_cast<gpio_num_t>(SMART_LIGHTING_LED_GPIO),
                          level) == ESP_OK;
}

bool writeLampHardwareBrightness(uint8_t percent) {
    if (percent > 100 || !lampHardwareSupportsBrightness()) return false;
#if defined(SMART_LIGHTING_LAMP_PWM_ENABLED)
    if (!writePwmDuty(percent)) return false;
    if (percent != 0) lastNonzeroBrightness = percent;
    return true;
#else
    return false;
#endif
}

#else

#if !defined(ESP_PLATFORM)
namespace {
bool simulationConfigured = false;
bool simulationBrightnessSupported = false;
bool simulationFailNextWrite = false;
uint8_t simulationOutput = 0;
uint8_t simulationLastNonzero = 100;
uint8_t simulationAllowedMaximumPercent = 100;

bool consumeSimulationFailure() {
    if (!simulationFailNextWrite) return false;
    simulationFailNextWrite = false;
    return true;
}
}  // namespace

void configureLampHardwareSimulation(bool brightnessSupported,
                                     uint8_t allowedMaximumPercent) {
    simulationConfigured = true;
    simulationBrightnessSupported = brightnessSupported;
    simulationFailNextWrite = false;
    simulationOutput = 0;
    simulationLastNonzero = 100;
    simulationAllowedMaximumPercent = allowedMaximumPercent >= 1 &&
                                     allowedMaximumPercent <= 100
        ? allowedMaximumPercent : 100;
}

void failNextLampHardwareWrite() {
    simulationFailNextWrite = true;
}

uint8_t simulatedLampHardwareOutput() {
    return simulationOutput;
}
#endif

void initializeLampHardware() {
#if !defined(ESP_PLATFORM)
    if (simulationConfigured) {
        simulationOutput = 0;
        simulationLastNonzero = 100;
    }
#endif
#if defined(SMART_LIGHTING_ZIGBEE) && defined(DEVICE_ROLE_LAMP)
    Serial.println(
        "[LAMP] GPIO output not configured; define SMART_LIGHTING_LED_GPIO after board/pin selection"
    );
#endif
}

bool lampHardwareSupportsBrightness() {
#if !defined(ESP_PLATFORM)
    return simulationConfigured && simulationBrightnessSupported;
#else
    return false;
#endif
}

bool writeLampHardwarePower(bool enabled) {
#if !defined(ESP_PLATFORM)
    if (!simulationConfigured) return true;
    if (consumeSimulationFailure()) return false;
    if (simulationBrightnessSupported) {
        simulationOutput = enabled
            ? static_cast<uint8_t>(lampPwmDutyFromPercent(
                  100, simulationAllowedMaximumPercent,
                  simulationLastNonzero))
            : 0;
    } else {
        simulationOutput = enabled ? 100 : 0;
    }
    return true;
#else
#if defined(SMART_LIGHTING_ZIGBEE) && defined(DEVICE_ROLE_LAMP)
    (void)enabled;
    return false;
#else
    (void)enabled;
    return true;
#endif
#endif
}

bool writeLampHardwareBrightness(uint8_t percent) {
#if !defined(ESP_PLATFORM)
    if (percent > 100 || !lampHardwareSupportsBrightness() ||
        consumeSimulationFailure()) {
        return false;
    }
    simulationOutput = static_cast<uint8_t>(lampPwmDutyFromPercent(
        100, simulationAllowedMaximumPercent, percent));
    if (percent != 0) simulationLastNonzero = percent;
    return true;
#else
    (void)percent;
    return false;
#endif
}

#endif
