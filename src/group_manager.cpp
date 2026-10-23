#include <Arduino.h>
#include <string.h>

#include "group_manager.h"

namespace {
constexpr uint32_t INITIAL_GROUP_REVISION = 1;

bool copyName(char* destination, const char* name) {
    if (destination == nullptr || name == nullptr || name[0] == '\0') {
        return false;
    }
    size_t length = 0;
    while (length < MAX_GROUP_NAME_LENGTH && name[length] != '\0') {
        const unsigned char value = static_cast<unsigned char>(name[length]);
        if (value < 0x20 || value == 0x7f) {
            return false;
        }
        ++length;
    }
    if (length == 0 || length >= MAX_GROUP_NAME_LENGTH) {
        return false;
    }
    memcpy(destination, name, length);
    destination[length] = '\0';
    return true;
}

uint32_t nextRevision(uint32_t revision) {
    ++revision;
    return revision == 0 ? 1 : revision;
}

bool allMembersBelongToMain(
    const LampGroup& group,
    const LampRegistry& lamps,
    uint32_t mainId
) {
    for (uint8_t index = 0; index < group.lampCount; ++index) {
        Lamp* lamp = findLamp(const_cast<LampRegistry&>(lamps), group.lampIds[index]);
        if (lamp == nullptr ||
            lamp->identity.pairingState != PairingState::PAIRED ||
            lamp->identity.parentMainId != mainId) {
            return false;
        }
    }
    return true;
}

bool saveCandidate(
    GroupRegistry& current,
    GroupRegistry& candidate,
    GroupStore& store
) {
    if (!current.persistenceReady || current.mainId == 0 ||
        !validateGroupRegistry(candidate, current.mainId)) {
        return false;
    }
    candidate.mainId = current.mainId;
    candidate.persistenceReady = true;
    if (!store.save(candidate)) {
        return false;
    }
    current = candidate;
    return true;
}
}  // namespace


MemoryGroupStore::MemoryGroupStore()
    : hasStored(false),
      failWrites(false),
      failCommit(false),
      forcedLoadResult(GroupStoreLoadResult::FOUND),
      writeAttempts(0) {
    initGroupRegistry(stored);
}

GroupStoreLoadResult MemoryGroupStore::load(
    uint32_t expectedMainId,
    GroupRegistry& registry
) const {
    if (forcedLoadResult != GroupStoreLoadResult::FOUND) {
        return forcedLoadResult;
    }
    if (!hasStored) {
        return GroupStoreLoadResult::NOT_FOUND;
    }
    if (stored.mainId != expectedMainId ||
        !validateGroupRegistry(stored, expectedMainId)) {
        return GroupStoreLoadResult::CORRUPT;
    }
    registry = stored;
    registry.persistenceReady = true;
    return GroupStoreLoadResult::FOUND;
}

bool MemoryGroupStore::save(const GroupRegistry& registry) {
    ++writeAttempts;
    if (failWrites || failCommit || registry.mainId == 0 ||
        !validateGroupRegistry(registry, registry.mainId)) {
        return false;
    }
    stored = registry;
    stored.persistenceReady = true;
    hasStored = true;
    forcedLoadResult = GroupStoreLoadResult::FOUND;
    return true;
}

void MemoryGroupStore::setFailWrites(bool fail) { failWrites = fail; }
void MemoryGroupStore::setFailCommit(bool fail) { failCommit = fail; }
void MemoryGroupStore::setLoadResultForTest(GroupStoreLoadResult result) {
    forcedLoadResult = result;
}
uint32_t MemoryGroupStore::getWriteAttemptCount() const { return writeAttempts; }


void initGroupRegistry(GroupRegistry& registry) {
    memset(&registry, 0, sizeof(registry));
    registry.persistenceReady = true;
}

GroupStoreLoadResult restoreGroupRegistry(
    GroupRegistry& registry,
    uint32_t mainId,
    GroupStore& store
) {
    initGroupRegistry(registry);
    registry.mainId = mainId;
    if (mainId == 0) {
        registry.persistenceReady = false;
        return GroupStoreLoadResult::ERROR;
    }

    GroupRegistry restored = {};
    const GroupStoreLoadResult result = store.load(mainId, restored);
    if (result == GroupStoreLoadResult::FOUND &&
        validateGroupRegistry(restored, mainId)) {
        restored.mainId = mainId;
        restored.persistenceReady = true;
        registry = restored;
        return result;
    }
    if (result == GroupStoreLoadResult::NOT_FOUND) {
        registry.persistenceReady = true;
        return result;
    }

    // Keep the RAM view empty and block writes so corrupt/unknown data is never
    // silently replaced by a default configuration.
    registry.persistenceReady = false;
    return result == GroupStoreLoadResult::FOUND
        ? GroupStoreLoadResult::CORRUPT : result;
}

bool setGroupName(LampGroup& group, const char* name) {
    char copied[MAX_GROUP_NAME_LENGTH] = {};
    if (!copyName(copied, name)) {
        return false;
    }
    memcpy(group.name, copied, sizeof(group.name));
    return true;
}

bool validateGroup(const LampGroup& group) {
    if (group.id == 0 || group.lampCount > MAX_GROUP_LAMPS ||
        group.revision == 0) {
        return false;
    }
    char checkedName[MAX_GROUP_NAME_LENGTH] = {};
    if (!copyName(checkedName, group.name) ||
        memcmp(checkedName, group.name, sizeof(checkedName)) != 0) {
        return false;
    }
    for (uint8_t index = 0; index < group.lampCount; ++index) {
        if (group.lampIds[index] == 0) {
            return false;
        }
        for (uint8_t prior = 0; prior < index; ++prior) {
            if (group.lampIds[prior] == group.lampIds[index]) {
                return false;
            }
        }
    }
    return true;
}

bool validateGroupRegistry(const GroupRegistry& registry, uint32_t mainId) {
    if (mainId == 0 || registry.count > MAX_GROUPS ||
        (registry.mainId != 0 && registry.mainId != mainId)) {
        return false;
    }
    for (uint8_t index = 0; index < registry.count; ++index) {
        if (!validateGroup(registry.groups[index])) {
            return false;
        }
        for (uint8_t prior = 0; prior < index; ++prior) {
            if (registry.groups[prior].id == registry.groups[index].id) {
                return false;
            }
        }
    }
    return true;
}

LampGroup* findGroup(GroupRegistry& registry, uint32_t groupId) {
    for (uint8_t index = 0; index < registry.count; ++index) {
        if (registry.groups[index].id == groupId) {
            return &registry.groups[index];
        }
    }
    return nullptr;
}

bool lampBelongsToGroup(const LampGroup& group, uint32_t lampId) {
    for (uint8_t index = 0; index < group.lampCount; ++index) {
        if (group.lampIds[index] == lampId) {
            return true;
        }
    }
    return false;
}

bool addGroup(GroupRegistry& registry, const LampGroup& group) {
    if (registry.count >= MAX_GROUPS || group.id == 0 ||
        findGroup(registry, group.id) != nullptr ||
        group.lampCount > MAX_GROUP_LAMPS) {
        return false;
    }
    LampGroup copy = {};
    copy.id = group.id;
    copy.lampCount = group.lampCount;
    copy.revision = group.revision == 0 ? INITIAL_GROUP_REVISION : group.revision;
    if (!setGroupName(copy, group.name)) {
        return false;
    }
    memcpy(copy.lampIds, group.lampIds, sizeof(copy.lampIds));
    if (!validateGroup(copy)) {
        return false;
    }
    registry.groups[registry.count++] = copy;
    return true;
}

bool removeGroup(GroupRegistry& registry, uint32_t groupId) {
    for (uint8_t index = 0; index < registry.count; ++index) {
        if (registry.groups[index].id != groupId) {
            continue;
        }
        for (uint8_t move = index; move + 1 < registry.count; ++move) {
            registry.groups[move] = registry.groups[move + 1];
        }
        --registry.count;
        memset(&registry.groups[registry.count], 0,
               sizeof(registry.groups[registry.count]));
        return true;
    }
    return false;
}

bool createGroupPersisted(
    GroupRegistry& registry,
    GroupStore& store,
    uint32_t groupId,
    const char* name
) {
    if (!registry.persistenceReady || registry.mainId == 0 || groupId == 0 ||
        findGroup(registry, groupId) != nullptr || registry.count >= MAX_GROUPS) {
        return false;
    }
    GroupRegistry candidate = registry;
    LampGroup group = {};
    group.id = groupId;
    group.revision = INITIAL_GROUP_REVISION;
    if (!setGroupName(group, name) || !addGroup(candidate, group)) {
        return false;
    }
    return saveCandidate(registry, candidate, store);
}

bool renameGroupPersisted(
    GroupRegistry& registry,
    GroupStore& store,
    uint32_t groupId,
    const char* name,
    const LampRegistry& lamps
) {
    LampGroup* current = findGroup(registry, groupId);
    if (!registry.persistenceReady || current == nullptr ||
        !allMembersBelongToMain(*current, lamps, registry.mainId)) {
        return false;
    }
    LampGroup renamed = *current;
    if (!setGroupName(renamed, name)) {
        return false;
    }
    if (strcmp(renamed.name, current->name) == 0) {
        return true;
    }
    renamed.revision = nextRevision(renamed.revision);
    GroupRegistry candidate = registry;
    *findGroup(candidate, groupId) = renamed;
    return saveCandidate(registry, candidate, store);
}

bool deleteGroupPersisted(
    GroupRegistry& registry,
    GroupStore& store,
    uint32_t groupId
) {
    if (!registry.persistenceReady || findGroup(registry, groupId) == nullptr) {
        return false;
    }
    GroupRegistry candidate = registry;
    if (!removeGroup(candidate, groupId)) {
        return false;
    }
    return saveCandidate(registry, candidate, store);
}

bool addLampToGroup(
    GroupRegistry& groups,
    LampRegistry& lamps,
    uint32_t groupId,
    uint32_t lampId
) {
    LampGroup* group = findGroup(groups, groupId);
    Lamp* lamp = findLamp(lamps, lampId);
    if (group == nullptr || lamp == nullptr ||
        lampBelongsToGroup(*group, lampId) ||
        group->lampCount >= MAX_GROUP_LAMPS) {
        return false;
    }
    group->lampIds[group->lampCount++] = lampId;
    group->revision = nextRevision(group->revision);
    return true;
}

bool addLampToGroupPersisted(
    GroupRegistry& groups,
    const LampRegistry& lamps,
    GroupStore& store,
    uint32_t groupId,
    uint32_t lampId
) {
    LampGroup* current = findGroup(groups, groupId);
    Lamp* lamp = findLamp(const_cast<LampRegistry&>(lamps), lampId);
    if (!groups.persistenceReady || current == nullptr || lamp == nullptr ||
        lamp->identity.pairingState != PairingState::PAIRED ||
        lamp->identity.parentMainId != groups.mainId ||
        !allMembersBelongToMain(*current, lamps, groups.mainId) ||
        lampBelongsToGroup(*current, lampId) ||
        current->lampCount >= MAX_GROUP_LAMPS) {
        return false;
    }
    GroupRegistry candidate = groups;
    LampGroup* changed = findGroup(candidate, groupId);
    changed->lampIds[changed->lampCount++] = lampId;
    changed->revision = nextRevision(changed->revision);
    return saveCandidate(groups, candidate, store);
}

bool removeLampFromGroup(
    GroupRegistry& groups,
    uint32_t groupId,
    uint32_t lampId
) {
    LampGroup* group = findGroup(groups, groupId);
    if (group == nullptr) {
        return false;
    }
    for (uint8_t index = 0; index < group->lampCount; ++index) {
        if (group->lampIds[index] != lampId) {
            continue;
        }
        for (uint8_t move = index; move + 1 < group->lampCount; ++move) {
            group->lampIds[move] = group->lampIds[move + 1];
        }
        --group->lampCount;
        group->lampIds[group->lampCount] = 0;
        group->revision = nextRevision(group->revision);
        return true;
    }
    return false;
}

bool removeLampFromGroupPersisted(
    GroupRegistry& groups,
    const LampRegistry& lamps,
    GroupStore& store,
    uint32_t groupId,
    uint32_t lampId
) {
    LampGroup* current = findGroup(groups, groupId);
    Lamp* lamp = findLamp(const_cast<LampRegistry&>(lamps), lampId);
    if (!groups.persistenceReady || current == nullptr || lamp == nullptr ||
        lamp->identity.parentMainId != groups.mainId ||
        !allMembersBelongToMain(*current, lamps, groups.mainId) ||
        !lampBelongsToGroup(*current, lampId)) {
        return false;
    }
    GroupRegistry candidate = groups;
    if (!removeLampFromGroup(candidate, groupId, lampId)) {
        return false;
    }
    return saveCandidate(groups, candidate, store);
}

bool removeLampFromAllGroupsPersisted(
    GroupRegistry& groups,
    GroupStore& store,
    uint32_t lampId
) {
    if (!groups.persistenceReady || groups.mainId == 0 || lampId == 0) {
        return false;
    }
    GroupRegistry candidate = groups;
    bool changed = false;
    for (uint8_t index = 0; index < candidate.count; ++index) {
        if (lampBelongsToGroup(candidate.groups[index], lampId)) {
            (void)removeLampFromGroup(candidate,
                candidate.groups[index].id, lampId);
            changed = true;
        }
    }
    return !changed || saveCandidate(groups, candidate, store);
}

void printGroup(const LampGroup& group) {
    Serial.println();
    Serial.println("===== GROUPE =====");
    Serial.print("ID          : "); Serial.println(group.id);
    Serial.print("Nom         : "); Serial.println(group.name);
    Serial.print("Révision    : "); Serial.println(group.revision);
    Serial.print("Nombre      : "); Serial.println(group.lampCount);
    Serial.print("Lampes      : ");
    if (group.lampCount == 0) {
        Serial.println("Aucune");
    } else {
        for (uint8_t index = 0; index < group.lampCount; ++index) {
            Serial.print(group.lampIds[index]);
            if (index + 1 < group.lampCount) Serial.print(", ");
        }
        Serial.println();
    }
    Serial.println("=================");
}

void printGroupRegistry(const GroupRegistry& registry) {
    Serial.println();
    Serial.print("===== REGISTRE GROUPES : ");
    Serial.print(registry.count);
    Serial.println(" =====");
    for (uint8_t index = 0; index < registry.count; ++index) {
        printGroup(registry.groups[index]);
    }
    Serial.println("============================");
}
