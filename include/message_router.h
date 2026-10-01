#pragma once

#include "communication.h"
#include "device_registry.h"
#include "group_manager.h"
#include "scene_manager.h"
#include "message_deduplicator.h"
#include "message_tracker.h"

struct EventBus;

using ApplicationMessageHandler = bool (*)(
    Communication& communication,
    const Message& message,
    void* context
);

void registerApplicationMessageHandler(
    ApplicationMessageHandler handler,
    void* context
);


void processMessages(
    Communication& communication,
    MessageTracker& tracker,
    MessageDeduplicator& deduplicator,
    LampRegistry& lamps,
    GroupRegistry& groups,
    SceneRegistry& scenes,
    bool dropNextAck = false,
    EventBus* eventBus = nullptr
);
