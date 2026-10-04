#include <Arduino.h>

#include "core_main_runtime.h"

#include "core_module_service.h"
#include "core_topology_protocol.h"
#include "message_id_generator.h"
#include "message_router.h"

namespace {
constexpr uint32_t ANNOUNCEMENT_RETRY_MS = 5000;
constexpr uint32_t ANNOUNCEMENT_BACKOFF_MS = 1000;
constexpr char MAIN_NAME[] = "MAIN_LIGHTING";
}

CoreMainRuntime::CoreMainRuntime(Communication& zigbeeCommunication)
    : zigbee(zigbeeCommunication), localId(0), assignedCoreId(0),
      nextAnnouncementAt(0) {}

bool CoreMainRuntime::begin(uint32_t mainLocalId) {
    if (mainLocalId == 0) return false;
    localId = mainLocalId;
    registerApplicationMessageHandler(handleZigbeeMessage, this);
    return true;
}

void CoreMainRuntime::poll() {
    if (localId == 0 || !communicationReady(zigbee)) return;
    const uint32_t now = millis();
    if (static_cast<int32_t>(now - nextAnnouncementAt) < 0) return;

    Message announcement = {};
    if (!makeCoreMainAnnouncement(
            localId,
            MAIN_NAME,
            DEVICE_CAP_LIGHTING | DEVICE_CAP_GROUPS | DEVICE_CAP_SCENES |
                DEVICE_CAP_AUTOMATION,
            announcement)) {
        nextAnnouncementAt = now + ANNOUNCEMENT_BACKOFF_MS;
        return;
    }
    announcement.id = generateMessageId();
    announcement.timestamp = now;
    if (sendMessage(zigbee, announcement)) {
        nextAnnouncementAt = now + ANNOUNCEMENT_RETRY_MS;
    } else {
        nextAnnouncementAt = now + ANNOUNCEMENT_BACKOFF_MS;
    }
}

bool CoreMainRuntime::handleZigbeeMessage(
    Communication&,
    const Message& message,
    void* context
) {
    CoreMainRuntime* runtime = static_cast<CoreMainRuntime*>(context);
    return runtime != nullptr && runtime->dispatch(message);
}

bool CoreMainRuntime::dispatch(const Message& message) {
    if (message.type != MessageType::STATE ||
        message.commandType != CORE_TOPOLOGY_ID_ASSIGNED) {
        return false;
    }

    uint32_t coreId = 0;
    uint8_t resultCode = 0;
    if (!readCoreIdAssignedMessage(message, localId, coreId, resultCode)) {
        return false;
    }

    const bool accepted = coreId != 0 &&
        (resultCode == static_cast<uint8_t>(CoreModuleUpdateResult::REGISTERED) ||
         resultCode == static_cast<uint8_t>(CoreModuleUpdateResult::UPDATED));
    if (accepted) {
        assignedCoreId = coreId;
        Message acknowledgement = {};
        if (makeCoreIdAckMessage(localId, assignedCoreId, true, acknowledgement)) {
            acknowledgement.id = generateMessageId();
            acknowledgement.timestamp = millis();
            (void)sendMessage(zigbee, acknowledgement);
        }
        Serial.print("[V7] MAIN CORE id=");
        Serial.println(static_cast<unsigned long>(assignedCoreId));
    } else {
        assignedCoreId = 0;
        Serial.print("[V7] CORE rejected MAIN identity, result=");
        Serial.println(static_cast<unsigned>(resultCode));
    }
    return true;
}

uint32_t CoreMainRuntime::coreId() const {
    return assignedCoreId;
}
