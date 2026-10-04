#pragma once

#include "core_link_codec.h"
#include "core_module_service.h"
#include "device_registry.h"

/* Turn a decoded UART announcement into a registry update and reply packet. */
CoreModuleUpdateResult processCoreLinkAnnouncement(
    DeviceRegistry& registry,
    const CoreLinkPacket& announcement,
    uint32_t receivedAt,
    CoreLinkPacket& response
);
