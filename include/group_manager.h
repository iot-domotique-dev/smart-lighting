#pragma once

#include <Arduino.h>

#include "group.h"
#include "device_registry.h"

enum class GroupStoreLoadResult : uint8_t {
    FOUND,
    NOT_FOUND,
    CORRUPT,
    UNSUPPORTED_VERSION,
    ERROR
};

class GroupStore {
public:
    virtual ~GroupStore() = default;
    virtual GroupStoreLoadResult load(
        uint32_t expectedMainId,
        struct GroupRegistry& registry
    ) const = 0;
    virtual bool save(const struct GroupRegistry& registry) = 0;
};


// ============================================================
// CONFIGURATION
// ============================================================

constexpr uint8_t MAX_GROUPS = 10;


// ============================================================
// REGISTRE DES GROUPES
// ============================================================

struct GroupRegistry {

    LampGroup groups[MAX_GROUPS];

    uint8_t count;
    uint32_t mainId;
    bool persistenceReady;
};


// Native-test store. A successful save is durable across object reconstruction.
class MemoryGroupStore : public GroupStore {
private:
    GroupRegistry stored;
    bool hasStored;
    bool failWrites;
    bool failCommit;
    GroupStoreLoadResult forcedLoadResult;
    uint32_t writeAttempts;

public:
    MemoryGroupStore();
    GroupStoreLoadResult load(
        uint32_t expectedMainId,
        GroupRegistry& registry
    ) const override;
    bool save(const GroupRegistry& registry) override;
    void setFailWrites(bool fail);
    void setFailCommit(bool fail);
    void setLoadResultForTest(GroupStoreLoadResult result);
    uint32_t getWriteAttemptCount() const;
};


// ============================================================
// INITIALISATION
// ============================================================

void initGroupRegistry(
    GroupRegistry& registry
);

GroupStoreLoadResult restoreGroupRegistry(
    GroupRegistry& registry,
    uint32_t mainId,
    GroupStore& store
);

bool validateGroup(const LampGroup& group);
bool validateGroupRegistry(const GroupRegistry& registry, uint32_t mainId);
bool setGroupName(LampGroup& group, const char* name);


// ============================================================
// GROUPES
// ============================================================

bool addGroup(
    GroupRegistry& registry,
    const LampGroup& group
);

bool createGroupPersisted(
    GroupRegistry& registry,
    GroupStore& store,
    uint32_t groupId,
    const char* name
);

bool renameGroupPersisted(
    GroupRegistry& registry,
    GroupStore& store,
    uint32_t groupId,
    const char* name,
    const LampRegistry& lamps
);

bool deleteGroupPersisted(
    GroupRegistry& registry,
    GroupStore& store,
    uint32_t groupId
);


bool removeGroup(
    GroupRegistry& registry,
    uint32_t groupId
);


LampGroup* findGroup(
    GroupRegistry& registry,
    uint32_t groupId
);


// ============================================================
// GESTION DES LAMPES DANS UN GROUPE
// ============================================================

bool addLampToGroup(
    GroupRegistry& groups,
    LampRegistry& lamps,
    uint32_t groupId,
    uint32_t lampId
);

bool addLampToGroupPersisted(
    GroupRegistry& groups,
    const LampRegistry& lamps,
    GroupStore& store,
    uint32_t groupId,
    uint32_t lampId
);


bool removeLampFromGroup(
    GroupRegistry& groups,
    uint32_t groupId,
    uint32_t lampId
);

bool removeLampFromGroupPersisted(
    GroupRegistry& groups,
    const LampRegistry& lamps,
    GroupStore& store,
    uint32_t groupId,
    uint32_t lampId
);

bool removeLampFromAllGroupsPersisted(
    GroupRegistry& groups,
    GroupStore& store,
    uint32_t lampId
);


bool lampBelongsToGroup(
    const LampGroup& group,
    uint32_t lampId
);


// ============================================================
// AFFICHAGE
// ============================================================

void printGroup(
    const LampGroup& group
);


void printGroupRegistry(
    const GroupRegistry& registry
);
