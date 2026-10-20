#include <Arduino.h>

#include "lamp_controller.h"
#include "action.h"
#include "lamp_hardware.h"
#include "provisioning_store.h"

namespace {
constexpr uint32_t FULL_RANGE_FADE_MS = 1000;
constexpr uint32_t FADE_TIMEOUT_MS = 3500;
constexpr uint32_t MIN_FADE_MS = 100;

void persistLastNonzeroBrightness(Lamp& lamp, uint8_t brightness) {
    if (lamp.settingsStore == nullptr) return;
    uint8_t storedBrightness = 0;
    const LampBrightnessLoadResult loadResult =
        lamp.settingsStore->loadLastNonzeroBrightness(storedBrightness);
    if (loadResult == LampBrightnessLoadResult::ERROR) {
        Serial.println("[LAMP] brightness applied; persistence read failed, not writing blindly");
    } else if (loadResult != LampBrightnessLoadResult::FOUND ||
               storedBrightness != brightness) {
        if (!lamp.settingsStore->saveLastNonzeroBrightness(brightness)) {
            Serial.println("[LAMP] brightness applied; NVS persistence failed");
        }
    }
}

void applyCompletedBrightness(Lamp& lamp, uint8_t brightness) {
    lamp.state.brightness = brightness;
    lamp.state.power = brightness != 0;
    if (brightness != 0) {
        lamp.state.lastNonzeroBrightness = brightness;
        persistLastNonzeroBrightness(lamp, brightness);
    }
}

uint32_t nextOutputGeneration(Lamp& lamp) {
    ++lamp.outputGeneration;
    if (lamp.outputGeneration == 0) ++lamp.outputGeneration;
    return lamp.outputGeneration;
}

void completeOutputTransition(Lamp& lamp) {
    LampOutputTransition& transition = lamp.outputTransition;
    applyCompletedBrightness(lamp, transition.targetBrightness);
    transition.active = false;
    Serial.print("[LAMP] fade completed target=");
    Serial.print(transition.targetBrightness);
    Serial.println("%");
}
}  // namespace


void restoreLampSettings(Lamp& lamp, ProvisioningStore& store) {
    lamp.outputTransition = {};
    lamp.settingsStore = &store;
    lamp.state.lastNonzeroBrightness = 100;

    uint8_t brightness = 0;
    const LampBrightnessLoadResult result =
        store.loadLastNonzeroBrightness(brightness);
    if (result == LampBrightnessLoadResult::FOUND) {
        lamp.state.lastNonzeroBrightness = brightness;
    } else if (result == LampBrightnessLoadResult::INVALID) {
        Serial.println("[LAMP] stored brightness invalid; using default 100%");
    } else if (result == LampBrightnessLoadResult::ERROR) {
        Serial.println("[LAMP] stored brightness could not be read; using default 100%");
    }
}


bool setLampPower(
    Lamp& lamp,
    bool state
) {

    if (!writeLampHardwarePower(state, lamp.state.lastNonzeroBrightness)) {
        Serial.println("[LAMP] GPIO power write failed");
        return false;
    }
    lamp.state.power = state;
    if (lampHardwareSupportsBrightness()) {
        lamp.state.brightness = state
            ? lamp.state.lastNonzeroBrightness : 0;
    }

    Serial.print("Power -> ");
    Serial.println(
        state ? "ON" : "OFF"
    );
    return true;
}


bool setLampBrightness(
    Lamp& lamp,
    int32_t brightness
) {
    if (brightness < 0 || brightness > 100 ||
        !lampHardwareSupportsBrightness()) {
        Serial.println("[LAMP] brightness command rejected: unsupported or out of range");
        return false;
    }
    if (!writeLampHardwareBrightness(static_cast<uint8_t>(brightness))) {
        Serial.println("[LAMP] PWM brightness write failed");
        return false;
    }

    applyCompletedBrightness(lamp, static_cast<uint8_t>(brightness));

    Serial.print("Brightness -> ");
    Serial.print(brightness);
    Serial.println("%");
    return true;
}

LampOutputTransitionResult beginLampOutputTransition(
    Lamp& lamp,
    ActionType commandType,
    int32_t value,
    uint32_t sourceId,
    uint32_t messageId,
    bool coreCommand,
    bool dropAck
) {
    if (lamp.outputTransition.active) return LampOutputTransitionResult::BUSY;
    if (lamp.outputTransition.faulted) return LampOutputTransitionResult::FAILED;
    if (!lampHardwareSupportsBrightness()) return LampOutputTransitionResult::FAILED;

    bool targetPower = lamp.state.power;
    uint8_t targetBrightness = lamp.state.brightness;
    if (commandType == ActionType::SET_LAMP_POWER) {
        targetPower = value != 0;
        targetBrightness = targetPower ? lamp.state.lastNonzeroBrightness : 0;
    } else if (commandType == ActionType::SET_LAMP_BRIGHTNESS) {
        if (value < 0 || value > 100) return LampOutputTransitionResult::FAILED;
        targetBrightness = static_cast<uint8_t>(value);
        targetPower = targetBrightness != 0;
    } else {
        return LampOutputTransitionResult::FAILED;
    }

    // OFF remains a direct, immediate write when the output is idle.
    if (commandType == ActionType::SET_LAMP_POWER && !targetPower) {
        if (!writeLampHardwarePower(false, lamp.state.lastNonzeroBrightness))
            return LampOutputTransitionResult::FAILED;
        lamp.state.power = false;
        lamp.state.brightness = 0;
        Serial.println("[LAMP] power OFF applied immediately");
        return LampOutputTransitionResult::COMPLETED;
    }

    if (lamp.state.power == targetPower &&
        lamp.state.brightness == targetBrightness) {
        return LampOutputTransitionResult::COMPLETED;
    }

    const uint8_t currentBrightness = lamp.state.brightness;
    const uint8_t delta = currentBrightness > targetBrightness
        ? static_cast<uint8_t>(currentBrightness - targetBrightness)
        : static_cast<uint8_t>(targetBrightness - currentBrightness);
    if (delta == 0) {
        applyCompletedBrightness(lamp, targetBrightness);
        return LampOutputTransitionResult::COMPLETED;
    }
    uint32_t durationMs =
        (FULL_RANGE_FADE_MS * delta + 99U) / 100U;
    if (durationMs < MIN_FADE_MS) durationMs = MIN_FADE_MS;
    if (durationMs > FULL_RANGE_FADE_MS) durationMs = FULL_RANGE_FADE_MS;

    const uint32_t completionSequence =
        lampHardwareFadeCompletionSequence();
    const uint32_t generation = nextOutputGeneration(lamp);
    const LampHardwareFadeStartResult fadeResult = startLampHardwareFade(
        targetBrightness, static_cast<uint16_t>(durationMs), generation);
    if (fadeResult == LampHardwareFadeStartResult::UNSUPPORTED) {
        // A PWM build without the optional fade ISR keeps the established
        // brightness behavior and applies the requested target immediately.
        if (!writeLampHardwareBrightness(targetBrightness))
            return LampOutputTransitionResult::FAILED;
        applyCompletedBrightness(lamp, targetBrightness);
        Serial.println("[LAMP] fade unavailable; brightness applied immediately");
        return LampOutputTransitionResult::COMPLETED;
    }
    if (fadeResult == LampHardwareFadeStartResult::FAILED) {
        if (stopLampHardwareFade() &&
            writeLampHardwareBrightness(currentBrightness)) {
            return LampOutputTransitionResult::FAILED;
        }
        lamp.outputTransition.faulted = true;
        return LampOutputTransitionResult::UNCERTAIN;
    }

    LampOutputTransition transition = {};
    transition.active = true;
    transition.coreCommand = coreCommand;
    transition.dropAck = dropAck;
    transition.targetPower = targetPower;
    transition.targetBrightness = targetBrightness;
    transition.previousBrightness = currentBrightness;
    transition.startedAt = millis();
    transition.timeoutMs = FADE_TIMEOUT_MS;
    transition.completionSequence = completionSequence;
    transition.generation = generation;
    transition.sourceId = sourceId;
    transition.messageId = messageId;
    lamp.outputTransition = transition;
    Serial.print("[LAMP] fade started target=");
    Serial.print(targetBrightness);
    Serial.print("% duration_ms=");
    Serial.println(static_cast<uint32_t>(durationMs));
    return LampOutputTransitionResult::STARTED;
}

LampOutputTransitionPollResult pollLampOutputTransition(
    Lamp& lamp,
    uint32_t now
) {
    LampOutputTransition& transition = lamp.outputTransition;
    if (!transition.active) return LampOutputTransitionPollResult::NONE;

    const auto complete = [&]() {
        completeOutputTransition(lamp);
        return LampOutputTransitionPollResult::EXECUTED;
    };

    if (lampHardwareFadeCompletionMatches(
            transition.completionSequence, transition.targetBrightness,
            transition.generation)) {
        return complete();
    }
    if (static_cast<uint32_t>(now - transition.startedAt) <
        transition.timeoutMs) {
        return LampOutputTransitionPollResult::NONE;
    }
    // A single read at the watchdog boundary covers a delayed/missed callback
    // without polling the PWM duty on every main-loop iteration.
    if (lampHardwareFadeAtTarget(transition.targetBrightness)) {
        return complete();
    }
    if (!stopLampHardwareFade() ||
        !writeLampHardwareBrightness(transition.previousBrightness)) {
        transition.active = false;
        transition.faulted = true;
        Serial.println("[LAMP] fade timeout; output state uncertain, withholding final ACK");
        return LampOutputTransitionPollResult::UNCERTAIN;
    }
    transition.active = false;
    Serial.println("[LAMP] fade timeout; previous output restored");
    return LampOutputTransitionPollResult::FAILED;
}

LampOutputOffResult forceLampOutputOff(
    Lamp& lamp,
    uint32_t now,
    LampOutputInterruption& interruption
) {
    (void)now;
    interruption = {};
    LampOutputTransition& transition = lamp.outputTransition;
    if (transition.active) {
        interruption.hadTransition = true;
        interruption.transition = transition;
        const bool reachedTarget =
            lampHardwareFadeAtTarget(transition.targetBrightness);
        if (!stopLampHardwareFade()) {
            Serial.println("[LAMP] priority OFF could not stop the active fade");
            return LampOutputOffResult::FAILED;
        }

        (void)nextOutputGeneration(lamp);
        transition.active = false;
        if (reachedTarget) {
            interruption.transitionCompleted = true;
            completeOutputTransition(lamp);
        } else {
            interruption.transitionStopped = true;
            Serial.print("[LAMP] fade interrupted by priority OFF, command=");
            Serial.println(interruption.transition.messageId);
        }
    }

    if (!writeLampHardwarePower(false, lamp.state.lastNonzeroBrightness)) {
        lamp.outputTransition.faulted = true;
        Serial.println("[LAMP] priority OFF failed; output state is uncertain");
        return LampOutputOffResult::FAILED;
    }

    lamp.state.power = false;
    if (lampHardwareSupportsBrightness()) lamp.state.brightness = 0;
    lamp.outputTransition.faulted = false;
    return LampOutputOffResult::APPLIED;
}


void setLampAutomatic(
    Lamp& lamp,
    bool enabled
) {

    lamp.state.automatic = enabled;

    Serial.print("Automatic -> ");
    Serial.println(
        enabled ? "ON" : "OFF"
    );
}


void printLampState(
    const Lamp& lamp
) {

    Serial.println();
    Serial.println("===== ETAT LAMPE =====");

    Serial.print("ID          : ");
    Serial.println(lamp.device.id);

    Serial.print("Nom         : ");
    Serial.println(lamp.device.name);

    Serial.print("Status      : ");

    Serial.println(
        lamp.device.status == DeviceStatus::ONLINE
            ? "ONLINE"
            : "OFFLINE"
    );

    Serial.print("Power       : ");
    Serial.println(
        lamp.state.power
            ? "ON"
            : "OFF"
    );

    Serial.print("Brightness  : ");
    Serial.print(lamp.state.brightness);
    Serial.println("%");

    Serial.print("Automatic   : ");

    Serial.println(
        lamp.state.automatic
            ? "OUI"
            : "NON"
    );

    Serial.println("======================");
}
