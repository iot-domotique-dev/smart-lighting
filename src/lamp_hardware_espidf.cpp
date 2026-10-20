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
#if defined(SMART_LIGHTING_LAMP_PWM_ENABLED)
bool pwmOutputReady = false;
bool fadeOutputReady = false;
bool fadeServiceInstalled = false;
portMUX_TYPE fadeCallbackMux = portMUX_INITIALIZER_UNLOCKED;
uint32_t fadeCallbackSequence = 0;
uint32_t fadeCallbackDuty = 0;
uint32_t fadeActiveGeneration = 0;
uint32_t fadeCallbackGeneration = 0;

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
    const esp_err_t result = ledc_set_duty_and_update(
        PWM_MODE, PWM_CHANNEL, brightnessDuty(percent), 0);
    if (result != ESP_OK) {
        Serial.print("[LAMP] LEDC duty update failed: ");
        Serial.println(static_cast<int32_t>(result));
        return false;
    }
    return true;
}

bool onPwmFadeComplete(const ledc_cb_param_t* param, void*) {
    if (param == nullptr || param->event != LEDC_FADE_END_EVT) return false;
    portENTER_CRITICAL_ISR(&fadeCallbackMux);
    fadeCallbackDuty = param->duty;
    fadeCallbackGeneration = fadeActiveGeneration;
    ++fadeCallbackSequence;
    portEXIT_CRITICAL_ISR(&fadeCallbackMux);
    return false;
}

uint32_t readFadeCallbackSequence() {
    portENTER_CRITICAL(&fadeCallbackMux);
    const uint32_t sequence = fadeCallbackSequence;
    portEXIT_CRITICAL(&fadeCallbackMux);
    return sequence;
}

uint32_t readFadeCallbackDuty() {
    portENTER_CRITICAL(&fadeCallbackMux);
    const uint32_t duty = fadeCallbackDuty;
    portEXIT_CRITICAL(&fadeCallbackMux);
    return duty;
}

uint32_t readFadeCallbackGeneration() {
    portENTER_CRITICAL(&fadeCallbackMux);
    const uint32_t generation = fadeCallbackGeneration;
    portEXIT_CRITICAL(&fadeCallbackMux);
    return generation;
}
#endif
}  // namespace

void initializeLampHardware() {
    outputInitialized = false;
#if defined(SMART_LIGHTING_LAMP_PWM_ENABLED)
    if (fadeServiceInstalled) {
        ledc_fade_func_uninstall();
        fadeServiceInstalled = false;
    }
    pwmOutputReady = false;
    fadeOutputReady = false;
    portENTER_CRITICAL(&fadeCallbackMux);
    fadeCallbackSequence = 0;
    fadeCallbackDuty = 0;
    fadeActiveGeneration = 0;
    fadeCallbackGeneration = 0;
    portEXIT_CRITICAL(&fadeCallbackMux);
#endif
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

    const esp_err_t fadeInstallResult = ledc_fade_func_install(0);
    if (fadeInstallResult == ESP_OK) {
        fadeServiceInstalled = true;
        ledc_cbs_t callbacks = {};
        callbacks.fade_cb = onPwmFadeComplete;
        if (ledc_cb_register(PWM_MODE, PWM_CHANNEL, &callbacks, nullptr) == ESP_OK) {
            fadeOutputReady = true;
            Serial.println("[LAMP] LEDC fade callback ready");
        } else {
            ledc_fade_func_uninstall();
            fadeServiceInstalled = false;
            Serial.println("[LAMP] LEDC fade callback registration failed; brightness remains immediate");
        }
    } else {
        Serial.print("[LAMP] LEDC fade service unavailable; brightness remains immediate: ");
        Serial.println(static_cast<int32_t>(fadeInstallResult));
    }
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

bool lampHardwareSupportsFade() {
#if defined(SMART_LIGHTING_LAMP_PWM_ENABLED)
    return outputInitialized && pwmOutputReady && fadeOutputReady;
#else
    return false;
#endif
}

LampHardwareFadeStartResult startLampHardwareFade(uint8_t percent,
                                                  uint16_t durationMs,
                                                  uint32_t generation) {
#if defined(SMART_LIGHTING_LAMP_PWM_ENABLED)
    if (!lampHardwareSupportsBrightness() || percent > 100 || durationMs == 0 ||
        generation == 0)
        return LampHardwareFadeStartResult::FAILED;
    if (!fadeOutputReady) return LampHardwareFadeStartResult::UNSUPPORTED;
    portENTER_CRITICAL(&fadeCallbackMux);
    fadeActiveGeneration = generation;
    portEXIT_CRITICAL(&fadeCallbackMux);
    const esp_err_t result = ledc_set_fade_time_and_start(
        PWM_MODE, PWM_CHANNEL, brightnessDuty(percent), durationMs,
        LEDC_FADE_NO_WAIT);
    if (result != ESP_OK) {
        portENTER_CRITICAL(&fadeCallbackMux);
        if (fadeActiveGeneration == generation) fadeActiveGeneration = 0;
        portEXIT_CRITICAL(&fadeCallbackMux);
        Serial.print("[LAMP] LEDC fade start failed: ");
        Serial.println(static_cast<int32_t>(result));
        return LampHardwareFadeStartResult::FAILED;
    }
    return LampHardwareFadeStartResult::STARTED;
#else
    (void)percent;
    (void)durationMs;
    return LampHardwareFadeStartResult::UNSUPPORTED;
#endif
}

uint32_t lampHardwareFadeCompletionSequence() {
#if defined(SMART_LIGHTING_LAMP_PWM_ENABLED)
    return readFadeCallbackSequence();
#else
    return 0;
#endif
}

bool lampHardwareFadeCompletionMatches(uint32_t& observedSequence,
                                       uint8_t percent,
                                       uint32_t generation) {
#if defined(SMART_LIGHTING_LAMP_PWM_ENABLED)
    if (!lampHardwareSupportsFade() || percent > 100 || generation == 0)
        return false;
    const uint32_t sequence = readFadeCallbackSequence();
    if (sequence == observedSequence) return false;
    observedSequence = sequence;
    const uint32_t expectedDuty = brightnessDuty(percent);
    return readFadeCallbackDuty() == expectedDuty &&
           readFadeCallbackGeneration() == generation &&
           ledc_get_duty(PWM_MODE, PWM_CHANNEL) == expectedDuty;
#else
    (void)observedSequence;
    (void)percent;
    (void)generation;
    return false;
#endif
}

bool lampHardwareFadeAtTarget(uint8_t percent) {
#if defined(SMART_LIGHTING_LAMP_PWM_ENABLED)
    return lampHardwareSupportsBrightness() && percent <= 100 &&
           ledc_get_duty(PWM_MODE, PWM_CHANNEL) == brightnessDuty(percent);
#else
    (void)percent;
    return false;
#endif
}

bool stopLampHardwareFade() {
#if defined(SMART_LIGHTING_LAMP_PWM_ENABLED)
    if (!fadeServiceInstalled) return true;
    const esp_err_t result = ledc_fade_stop(PWM_MODE, PWM_CHANNEL);
    if (result == ESP_OK) {
        portENTER_CRITICAL(&fadeCallbackMux);
        fadeActiveGeneration = 0;
        portEXIT_CRITICAL(&fadeCallbackMux);
        return true;
    }
    Serial.print("[LAMP] LEDC fade stop failed: ");
    Serial.println(static_cast<int32_t>(result));
    return false;
#else
    return true;
#endif
}

bool writeLampHardwarePower(bool enabled, uint8_t lastNonzeroBrightness) {
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
bool simulationFadeSupported = true;
bool simulationFadeActive = false;
bool simulationFailNextFadeStart = false;
bool simulationFailNextFadeStop = false;
uint8_t simulationOutput = 0;
uint8_t simulationAllowedMaximumPercent = 100;
uint8_t simulationFadeTarget = 0;
uint32_t simulationFadeSequence = 0;
uint8_t simulationFadeCallbackPercent = 0;
uint32_t simulationFadeGeneration = 0;
uint32_t simulationFadeCallbackGeneration = 0;
uint16_t simulationFadeDurationMs = 0;

bool consumeSimulationFailure() {
    if (!simulationFailNextWrite) return false;
    simulationFailNextWrite = false;
    return true;
}
}  // namespace

void configureLampHardwareSimulation(bool brightnessSupported,
                                     uint8_t allowedMaximumPercent,
                                     bool fadeSupported) {
    simulationConfigured = true;
    simulationBrightnessSupported = brightnessSupported;
    simulationFadeSupported = fadeSupported;
    simulationFailNextWrite = false;
    simulationOutput = 0;
    simulationFadeActive = false;
    simulationFailNextFadeStart = false;
    simulationFailNextFadeStop = false;
    simulationFadeSequence = 0;
    simulationFadeCallbackPercent = 0;
    simulationFadeGeneration = 0;
    simulationFadeCallbackGeneration = 0;
    simulationFadeDurationMs = 0;
    simulationAllowedMaximumPercent = allowedMaximumPercent >= 1 &&
                                     allowedMaximumPercent <= 100
        ? allowedMaximumPercent : 100;
}

void failNextLampHardwareWrite() {
    simulationFailNextWrite = true;
}

void failNextLampHardwareFadeStart() {
    simulationFailNextFadeStart = true;
}

void failNextLampHardwareFadeStop() {
    simulationFailNextFadeStop = true;
}

void simulateLampHardwareFadeCompletion() {
    if (!simulationFadeActive) return;
    simulationOutput = static_cast<uint8_t>(lampPwmDutyFromPercent(
        100, simulationAllowedMaximumPercent, simulationFadeTarget));
    simulationFadeCallbackPercent = simulationFadeTarget;
    simulationFadeCallbackGeneration = simulationFadeGeneration;
    simulationFadeActive = false;
    simulationFadeGeneration = 0;
    ++simulationFadeSequence;
}

void simulateLampHardwareReachTargetWithoutCallback() {
    if (!simulationFadeActive) return;
    simulationOutput = static_cast<uint8_t>(lampPwmDutyFromPercent(
        100, simulationAllowedMaximumPercent, simulationFadeTarget));
    simulationFadeActive = false;
}

void simulateLampHardwareFadeCallback(uint8_t reportedPercent) {
    simulationFadeCallbackPercent = reportedPercent;
    simulationFadeCallbackGeneration = simulationFadeGeneration;
    ++simulationFadeSequence;
}

void simulateLampHardwareFadeCallbackForGeneration(uint8_t reportedPercent,
                                                   uint32_t generation) {
    simulationFadeCallbackPercent = reportedPercent;
    simulationFadeCallbackGeneration = generation;
    ++simulationFadeSequence;
}

bool simulatedLampHardwareFadeActive() { return simulationFadeActive; }

uint16_t simulatedLampHardwareFadeDuration() {
    return simulationFadeDurationMs;
}

uint8_t simulatedLampHardwareOutput() {
    return simulationOutput;
}
#endif

void initializeLampHardware() {
#if !defined(ESP_PLATFORM)
    if (simulationConfigured) {
        simulationOutput = 0;
        simulationFadeActive = false;
        simulationFadeSequence = 0;
        simulationFadeCallbackPercent = 0;
        simulationFadeGeneration = 0;
        simulationFadeCallbackGeneration = 0;
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

bool lampHardwareSupportsFade() {
#if !defined(ESP_PLATFORM)
    return simulationConfigured && simulationBrightnessSupported &&
           simulationFadeSupported;
#else
    return false;
#endif
}

LampHardwareFadeStartResult startLampHardwareFade(uint8_t percent,
                                                  uint16_t durationMs,
                                                  uint32_t generation) {
#if !defined(ESP_PLATFORM)
    if (!lampHardwareSupportsBrightness() || percent > 100 || durationMs == 0 ||
        generation == 0)
        return LampHardwareFadeStartResult::FAILED;
    if (!simulationFadeSupported) return LampHardwareFadeStartResult::UNSUPPORTED;
    if (simulationFailNextFadeStart) {
        simulationFailNextFadeStart = false;
        return LampHardwareFadeStartResult::FAILED;
    }
    if (simulationFadeActive) return LampHardwareFadeStartResult::FAILED;
    simulationFadeTarget = percent;
    simulationFadeDurationMs = durationMs;
    simulationFadeGeneration = generation;
    simulationFadeActive = true;
    return LampHardwareFadeStartResult::STARTED;
#else
    (void)percent;
    (void)durationMs;
    (void)generation;
    return LampHardwareFadeStartResult::UNSUPPORTED;
#endif
}

uint32_t lampHardwareFadeCompletionSequence() {
#if !defined(ESP_PLATFORM)
    return simulationFadeSequence;
#else
    return 0;
#endif
}

bool lampHardwareFadeCompletionMatches(uint32_t& observedSequence,
                                       uint8_t percent,
                                       uint32_t generation) {
#if !defined(ESP_PLATFORM)
    if (!lampHardwareSupportsFade() || simulationFadeSequence == observedSequence)
        return false;
    observedSequence = simulationFadeSequence;
    const uint8_t expected = static_cast<uint8_t>(lampPwmDutyFromPercent(
        100, simulationAllowedMaximumPercent, percent));
    const uint8_t callback = static_cast<uint8_t>(lampPwmDutyFromPercent(
        100, simulationAllowedMaximumPercent, simulationFadeCallbackPercent));
    return simulationOutput == expected && callback == expected &&
           simulationFadeCallbackGeneration == generation;
#else
    (void)observedSequence;
    (void)percent;
    (void)generation;
    return false;
#endif
}

bool lampHardwareFadeAtTarget(uint8_t percent) {
#if !defined(ESP_PLATFORM)
    if (!lampHardwareSupportsBrightness() || percent > 100) return false;
    return simulationOutput == static_cast<uint8_t>(lampPwmDutyFromPercent(
        100, simulationAllowedMaximumPercent, percent));
#else
    (void)percent;
    return false;
#endif
}

bool stopLampHardwareFade() {
#if !defined(ESP_PLATFORM)
    if (simulationFailNextFadeStop) {
        simulationFailNextFadeStop = false;
        return false;
    }
    simulationFadeActive = false;
    simulationFadeGeneration = 0;
    return true;
#else
    return true;
#endif
}

bool writeLampHardwarePower(bool enabled, uint8_t lastNonzeroBrightness) {
#if !defined(ESP_PLATFORM)
    if (!simulationConfigured) return true;
    if (consumeSimulationFailure()) return false;
    if (simulationBrightnessSupported) {
        simulationOutput = enabled
            ? static_cast<uint8_t>(lampPwmDutyFromPercent(
                  100, simulationAllowedMaximumPercent,
                  lastNonzeroBrightness))
            : 0;
    } else {
        simulationOutput = enabled ? 100 : 0;
    }
    return true;
#else
#if defined(SMART_LIGHTING_ZIGBEE) && defined(DEVICE_ROLE_LAMP)
    (void)enabled;
    (void)lastNonzeroBrightness;
    return false;
#else
    (void)enabled;
    (void)lastNonzeroBrightness;
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
    return true;
#else
    (void)percent;
    return false;
#endif
}

#endif
