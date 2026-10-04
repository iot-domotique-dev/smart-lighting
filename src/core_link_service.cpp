#include <string.h>

#include "core_link_service.h"

CoreModuleUpdateResult processCoreLinkAnnouncement(
    DeviceRegistry& registry,
    const CoreLinkPacket& announcement,
    uint32_t receivedAt,
    CoreLinkPacket& response
) {
    response = {};
    response.sequence = announcement.sequence;
    response.localId = announcement.localId;
    response.type = CoreLinkMessageType::ERROR;

    if (announcement.type != CoreLinkMessageType::MODULE_ANNOUNCEMENT) {
        response.resultCode = static_cast<uint8_t>(
            CoreModuleUpdateResult::INVALID_ANNOUNCEMENT);
        return CoreModuleUpdateResult::INVALID_ANNOUNCEMENT;
    }

    CoreModuleAnnouncement module = {};
    module.localId = announcement.localId;
    module.parentId = announcement.parentId;
    module.name = announcement.name;
    module.role = static_cast<DeviceRole>(announcement.role);
    module.capabilities = announcement.capabilities;

    const CoreModuleUpdateResult result = ingestCoreModuleAnnouncement(
        registry,
        module,
        receivedAt
    );
    response.resultCode = static_cast<uint8_t>(result);
    if (result == CoreModuleUpdateResult::REGISTERED ||
        result == CoreModuleUpdateResult::UPDATED) {
        response.type = CoreLinkMessageType::ID_ASSIGNMENT;
        response.coreId = makeCoreModuleId(module.parentId, module.localId);
    }
    return result;
}
