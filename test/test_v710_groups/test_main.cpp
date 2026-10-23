#include <unity.h>
#include <stdio.h>
#include <string.h>

#define SMART_LIGHTING_CORE_WIFI
#include "../../src/core_http_api.cpp"

#include "group_manager.h"
#include "provisioning.h"
#include "core_module_service.h"
#include "core_command_protocol.h"
#include "core_pairing_protocol.h"
#include "v5_provisioning_runtime.h"
#include "message_router.h"
#include "scene_manager.h"
#include "device_hardware_id.h"
#include "zigbee_transport.h"

FakeHardwareSerial Serial;

static uint32_t fakeNow = 1000;
uint32_t millis() { return fakeNow; }

static CoreLinkPacket httpGroupUartPackets[512] = {};
static uint16_t httpGroupUartPacketCount = 0;
static void (*httpGroupDelayHook)() = nullptr;
static CoreHttpApi* httpGroupApiUnderTest = nullptr;

CoreUartTransport::CoreUartTransport(int uartPort)
    : port(uartPort), started(false) {}
bool CoreUartTransport::begin() { started = true; return true; }
bool CoreUartTransport::send(const CoreLinkPacket& packet) {
    if (!started || httpGroupUartPacketCount >= 512) return false;
    httpGroupUartPackets[httpGroupUartPacketCount++] = packet;
    return true;
}
bool CoreUartTransport::receive(CoreLinkPacket&) { return false; }
bool CoreUartTransport::isReady() const { return started; }
const char* CoreUartTransport::name() const { return "CORE_UART_TEST"; }
uint16_t CoreUartTransport::decoderErrorCount() const { return decoder.errors(); }

WiFiTransport::WiFiTransport()
    : started(false), credentialsConfigured(false), reconnectPending(false),
      reconnectAtMs(0), configuredSsid{} {}
bool WiFiTransport::isReady() const { return true; }

void smartLightingTestTaskDelay(TickType_t ticks) {
    fakeNow += ticks;
    if (httpGroupDelayHook != nullptr) {
        void (*hook)() = httpGroupDelayHook;
        httpGroupDelayHook = nullptr;
        hook();
    }
}


class GroupPowerTestTransport : public CommunicationTransportInterface {
public:
    struct SentMessage { Message message; uint32_t nextHop; };
    SentMessage sent[1024] = {};
    uint16_t sentCount = 0;
    Message incoming[32] = {};
    uint8_t incomingCount = 0;

    bool begin() override { return true; }
    bool send(const Message& message) override {
        return sendVia(message, 0, CommunicationRouteScope::LOCAL_DEVICE);
    }
    bool sendVia(const Message& message, uint32_t nextHop,
                 CommunicationRouteScope = CommunicationRouteScope::LOCAL_DEVICE) override {
        if (sentCount >= 1024) return false;
        sent[sentCount++] = {message, nextHop};
        return true;
    }
    bool receive(Message& message) override {
        if (incomingCount == 0) return false;
        message = incoming[0];
        for (uint8_t index = 0; index + 1 < incomingCount; ++index)
            incoming[index] = incoming[index + 1];
        --incomingCount;
        return true;
    }
    bool isReady() const override { return true; }
    const char* name() const override { return "test"; }
    void enqueue(const Message& message) {
        if (incomingCount < 32) incoming[incomingCount++] = message;
    }
    void clearSent() { sentCount = 0; }
};

static GroupPowerTestTransport groupPowerTransport;
static Communication groupPowerCommunication = {
    CommunicationTransportType::ZIGBEE, CommunicationState::READY, 0,
    &groupPowerTransport
};
static LampRegistry groupPowerLamps = {};
static GroupRegistry groupPowerGroups = {};
static MessageTracker groupPowerTracker = {};
static MessageDeduplicator groupPowerDeduplicator = {};
static SceneRegistry groupPowerScenes = {};
static MemoryGroupStore groupPowerStore;
static V5ProvisioningRuntime groupPowerRuntime(groupPowerCommunication,
    groupPowerLamps, groupPowerTracker, groupPowerDeduplicator);

bool sendMessage(Communication&, Message message) {
    return groupPowerTransport.send(message);
}
bool receiveMessage(Communication&, Message& message) {
    return groupPowerTransport.receive(message);
}

extern "C" void setUp(void) {
    fakeNow = 1000;
    httpGroupUartPacketCount = 0;
    httpGroupDelayHook = nullptr;
    httpGroupApiUnderTest = nullptr;
    smart_freertos_test::resetSemaphores();
    smart_http_test::resetServer();
}
extern "C" void tearDown(void) {}

namespace {
constexpr uint32_t TEST_MAIN_ID = 0x12345678;
constexpr uint32_t TEST_LAMP_ID = 0x80000011;
constexpr uint32_t TEST_GROUP_ID = 901;
constexpr uint32_t TEST_GROUP_SINGLE_ID = 902;
constexpr uint32_t TEST_GROUP_EMPTY_ID = 903;

Lamp pairedLamp(uint32_t id, uint32_t parentMainId);

uint32_t testMainLogicalId() {
    uint32_t hash = 2166136261UL;
    const char* hardwareId = "C6-TEST-MAIN";
    for (const unsigned char* cursor =
             reinterpret_cast<const unsigned char*>(hardwareId);
         *cursor != 0; ++cursor) {
        hash ^= *cursor;
        hash *= 16777619UL;
    }
    return hash == 0 ? 1 : hash;
}

void pumpGroupPowerMessage(const Message& message) {
    groupPowerTransport.enqueue(message);
    processMessages(groupPowerCommunication, groupPowerTracker,
        groupPowerDeduplicator, groupPowerLamps, groupPowerGroups,
        groupPowerScenes, false, nullptr);
}

Message groupPowerRequest(uint32_t operationId, uint32_t groupId, bool on) {
    Message message = {};
    message.id = operationId;
    message.sourceId = CORE_LOGICAL_ID;
    message.destinationId = testMainLogicalId();
    message.type = MessageType::GROUP_CONTROL;
    message.parentMainId = makeCoreModuleId(CORE_LOGICAL_ID, testMainLogicalId());
    message.group.version = GROUP_POWER_PROTOCOL_VERSION;
    message.group.action = GroupProtocolAction::SET_POWER;
    message.group.operationId = operationId;
    message.group.mainLocalId = testMainLogicalId();
    message.group.mainCoreId = message.parentMainId;
    message.group.groupId = groupId;
    message.group.power = on ? 1 : 0;
    return message;
}

Message groupPowerAck(const Message& child, ExecutionStatus status =
    ExecutionStatus::EXECUTED) {
    Message ack = {};
    ack.id = child.id + 100000;
    ack.sourceId = status == ExecutionStatus::EXECUTED
        ? child.destinationId : makeCoreModuleId(CORE_LOGICAL_ID, testMainLogicalId());
    ack.destinationId = CORE_LOGICAL_ID;
    ack.type = MessageType::ACK;
    ack.commandType = child.commandType;
    ack.value = status == ExecutionStatus::EXECUTED
        ? static_cast<int32_t>(ExecutionStatus::EXECUTED) : 0;
    ack.value2 = static_cast<int32_t>(child.id);
    ack.executionStatus = status;
    ack.parentMainId = child.parentMainId;
    ack.provisioningDeviceId = child.provisioningDeviceId;
    return ack;
}

uint16_t countGroupPowerChildren(uint16_t fromIndex, uint32_t localId = 0) {
    uint16_t count = 0;
    for (uint16_t index = fromIndex; index < groupPowerTransport.sentCount; ++index) {
        const Message& message = groupPowerTransport.sent[index].message;
        if (message.type == MessageType::COMMAND &&
            message.timestamp != 0 &&
            message.parentMainId == makeCoreModuleId(CORE_LOGICAL_ID,
                                                      testMainLogicalId()) &&
            (localId == 0 || message.provisioningDeviceId == localId)) ++count;
    }
    return count;
}

Message findGroupPowerChild(uint16_t fromIndex, uint32_t localId,
                            uint8_t occurrence = 0) {
    for (uint16_t index = fromIndex; index < groupPowerTransport.sentCount; ++index) {
        const Message& message = groupPowerTransport.sent[index].message;
        if (message.type == MessageType::COMMAND &&
            message.timestamp != 0 &&
            message.parentMainId == makeCoreModuleId(CORE_LOGICAL_ID,
                                                      testMainLogicalId()) &&
            message.provisioningDeviceId == localId) {
            if (occurrence == 0) return message;
            --occurrence;
        }
    }
    return {};
}

Message lastRoutedCommand(uint16_t fromIndex, uint32_t localId) {
    for (uint16_t index = groupPowerTransport.sentCount; index > fromIndex; --index) {
        const Message& message = groupPowerTransport.sent[index - 1].message;
        if (message.type == MessageType::COMMAND &&
            message.provisioningDeviceId == localId) return message;
    }
    return {};
}

uint16_t countCommandId(uint16_t fromIndex, uint32_t commandId) {
    uint16_t count = 0;
    for (uint16_t index = fromIndex; index < groupPowerTransport.sentCount; ++index) {
        const Message& message = groupPowerTransport.sent[index].message;
        if (message.type == MessageType::COMMAND && message.id == commandId) ++count;
    }
    return count;
}

GroupProtocolResult lastGroupPowerResult(uint32_t operationId,
                                          GroupProtocolAction action) {
    for (uint16_t index = groupPowerTransport.sentCount; index > 0; --index) {
        const Message& message = groupPowerTransport.sent[index - 1].message;
        if (message.type == MessageType::GROUP_CONTROL &&
            message.group.operationId == operationId &&
            message.group.action == action) return message.group.result;
    }
    return GroupProtocolResult::NONE;
}

GroupProtocolResult lastGroupPowerMemberResult(uint32_t operationId,
                                               uint32_t localId) {
    for (uint16_t index = groupPowerTransport.sentCount; index > 0; --index) {
        const Message& message = groupPowerTransport.sent[index - 1].message;
        if (message.type == MessageType::GROUP_CONTROL &&
            message.group.operationId == operationId &&
            message.group.action == GroupProtocolAction::MEMBER_RESULT &&
            message.group.memberLocalId == localId) return message.group.result;
    }
    return GroupProtocolResult::NONE;
}

bool initializeGroupPowerRuntime() {
    const uint32_t mainId = testMainLogicalId();
    if (restoreGroupRegistry(groupPowerGroups, mainId, groupPowerStore) !=
            GroupStoreLoadResult::NOT_FOUND ||
        !createGroupPersisted(groupPowerGroups, groupPowerStore,
                              TEST_GROUP_ID, "Two lamps") ||
        !createGroupPersisted(groupPowerGroups, groupPowerStore,
                              TEST_GROUP_SINGLE_ID, "One lamp") ||
        !createGroupPersisted(groupPowerGroups, groupPowerStore,
                              TEST_GROUP_EMPTY_ID, "Empty")) return false;
    initLampRegistry(groupPowerLamps);
    for (uint32_t localId = 11; localId <= 12; ++localId) {
        Lamp lamp = pairedLamp(localId, mainId);
        lamp.device.capabilities = DEVICE_CAP_POWER;
        if (!addLamp(groupPowerLamps, lamp)) return false;
    }
    if (!addLampToGroupPersisted(groupPowerGroups, groupPowerLamps,
            groupPowerStore, TEST_GROUP_ID, 11) ||
        !addLampToGroupPersisted(groupPowerGroups, groupPowerLamps,
            groupPowerStore, TEST_GROUP_ID, 12) ||
        !addLampToGroupPersisted(groupPowerGroups, groupPowerLamps,
            groupPowerStore, TEST_GROUP_SINGLE_ID, 11)) return false;
    initMessageTracker(groupPowerTracker);
    initMessageDeduplicator(groupPowerDeduplicator);
    initSceneRegistry(groupPowerScenes);
    groupPowerCommunication.localDeviceId = mainId;
    return groupPowerRuntime.begin(DeviceRole::MAIN, nullptr, nullptr,
        &groupPowerStore, &groupPowerGroups);
}

Lamp pairedLamp(uint32_t id, uint32_t parentMainId) {
    Lamp lamp = {};
    lamp.device.id = id;
    lamp.device.role = DeviceRole::LAMP;
    lamp.device.status = DeviceStatus::ONLINE;
    lamp.identity.parentMainId = parentMainId;
    lamp.identity.pairingState = PairingState::PAIRED;
    snprintf(lamp.identity.hardwareId, sizeof(lamp.identity.hardwareId),
             "C6-TEST-%08lx", static_cast<unsigned long>(id));
    return lamp;
}

void initMainGroups(GroupRegistry& groups, GroupStore& store) {
    TEST_ASSERT_EQUAL_INT(static_cast<int>(GroupStoreLoadResult::NOT_FOUND),
        static_cast<int>(restoreGroupRegistry(groups, TEST_MAIN_ID, store)));
}

void prepareRemovalState(MemoryProvisioningReservationStore& reservations,
                         LampRegistry& lamps,
                         GroupRegistry& groups,
                         MemoryGroupStore& groupStore) {
    MainProvisioningReservationState state = {};
    initializeMainProvisioningReservationState(TEST_MAIN_ID, state);
    state.highWaterDeviceId = 11;
    state.count = 1;
    state.identities[0].deviceId = 11;
    strncpy(state.identities[0].hardwareId, "C6-REMOVE-TEST",
            sizeof(state.identities[0].hardwareId) - 1);
    TEST_ASSERT_TRUE(reservations.save(state));

    initLampRegistry(lamps);
    Lamp lamp = pairedLamp(11, TEST_MAIN_ID);
    strncpy(lamp.identity.hardwareId, "C6-REMOVE-TEST",
            sizeof(lamp.identity.hardwareId) - 1);
    TEST_ASSERT_TRUE(addLamp(lamps, lamp));

    initMainGroups(groups, groupStore);
    TEST_ASSERT_TRUE(createGroupPersisted(groups, groupStore,
                                          TEST_GROUP_ID, "Salon"));
    TEST_ASSERT_TRUE(addLampToGroupPersisted(groups, lamps, groupStore,
                                             TEST_GROUP_ID, 11));
}

ProvisioningPacket removalConfirmation(uint32_t operationId) {
    ProvisioningPacket packet = {};
    packet.type = ProvisioningPacketType::UNPAIR_CONFIRM;
    packet.role = DeviceRole::LAMP;
    packet.pairingState = PairingState::UNPAIRING;
    packet.transactionId = operationId;
    packet.deviceId = 11;
    packet.parentMainId = TEST_MAIN_ID;
    packet.sourceId = 11;
    packet.destinationId = TEST_MAIN_ID;
    strncpy(packet.hardwareId, "C6-REMOVE-TEST", sizeof(packet.hardwareId) - 1);
    return packet;
}
}  // namespace

void test_group_store_starts_empty_and_restores_committed_snapshot() {
    MemoryGroupStore store;
    GroupRegistry groups;
    initMainGroups(groups, store);
    TEST_ASSERT_EQUAL_UINT32(TEST_MAIN_ID, groups.mainId);
    TEST_ASSERT_EQUAL_UINT8(0, groups.count);
    TEST_ASSERT_EQUAL_UINT32(0, store.getWriteAttemptCount());

    TEST_ASSERT_TRUE(createGroupPersisted(groups, store, TEST_GROUP_ID, "Cuisine"));
    GroupRegistry restarted;
    TEST_ASSERT_EQUAL_INT(static_cast<int>(GroupStoreLoadResult::FOUND),
        static_cast<int>(restoreGroupRegistry(restarted, TEST_MAIN_ID, store)));
    TEST_ASSERT_EQUAL_UINT8(1, restarted.count);
    TEST_ASSERT_EQUAL_STRING("Cuisine", restarted.groups[0].name);
    TEST_ASSERT_EQUAL_UINT32(1, restarted.groups[0].revision);
}

void test_group_name_is_owned_and_bounded() {
    GroupRegistry groups;
    MemoryGroupStore store;
    initMainGroups(groups, store);
    char temporary[] = "Bureau";
    LampGroup input = {};
    input.id = 44;
    input.revision = 1;
    TEST_ASSERT_TRUE(setGroupName(input, temporary));
    temporary[0] = 'x';
    TEST_ASSERT_TRUE(addGroup(groups, input));
    TEST_ASSERT_EQUAL_STRING("Bureau", groups.groups[0].name);

    char tooLong[MAX_GROUP_NAME_LENGTH + 1];
    memset(tooLong, 'x', sizeof(tooLong));
    tooLong[sizeof(tooLong) - 1] = '\0';
    TEST_ASSERT_FALSE(createGroupPersisted(groups, store, 45, tooLong));
    TEST_ASSERT_FALSE(createGroupPersisted(groups, store, 45, ""));
    TEST_ASSERT_FALSE(createGroupPersisted(groups, store, 45, "bad\nname"));
    TEST_ASSERT_EQUAL_UINT8(1, groups.count);
}

void test_group_duplicate_id_and_duplicate_members_are_rejected() {
    GroupRegistry groups;
    MemoryGroupStore store;
    initMainGroups(groups, store);
    TEST_ASSERT_TRUE(createGroupPersisted(groups, store, 17, "A"));
    const uint32_t writesBeforeDuplicate = store.getWriteAttemptCount();
    TEST_ASSERT_FALSE(createGroupPersisted(groups, store, 17, "B"));
    TEST_ASSERT_EQUAL_UINT32(writesBeforeDuplicate, store.getWriteAttemptCount());

    LampGroup invalid = {};
    invalid.id = 18;
    invalid.revision = 1;
    invalid.lampCount = 2;
    invalid.lampIds[0] = 11;
    invalid.lampIds[1] = 11;
    TEST_ASSERT_TRUE(setGroupName(invalid, "Invalid"));
    TEST_ASSERT_FALSE(addGroup(groups, invalid));
}

void test_group_members_require_registered_lamp_on_same_main_and_keep_uint32_id() {
    GroupRegistry groups;
    MemoryGroupStore store;
    LampRegistry lamps;
    initMainGroups(groups, store);
    initLampRegistry(lamps);
    TEST_ASSERT_TRUE(createGroupPersisted(groups, store, TEST_GROUP_ID, "Large ID"));
    TEST_ASSERT_FALSE(addLampToGroupPersisted(groups, lamps, store,
                                               TEST_GROUP_ID, TEST_LAMP_ID));

    TEST_ASSERT_TRUE(addLamp(lamps, pairedLamp(TEST_LAMP_ID, TEST_MAIN_ID)));
    TEST_ASSERT_TRUE(addLampToGroupPersisted(groups, lamps, store,
                                              TEST_GROUP_ID, TEST_LAMP_ID));
    TEST_ASSERT_TRUE(lampBelongsToGroup(groups.groups[0], TEST_LAMP_ID));
    TEST_ASSERT_FALSE(addLampToGroupPersisted(groups, lamps, store,
                                               TEST_GROUP_ID, TEST_LAMP_ID));

    TEST_ASSERT_TRUE(addLamp(lamps, pairedLamp(12, TEST_MAIN_ID + 1)));
    TEST_ASSERT_FALSE(addLampToGroupPersisted(groups, lamps, store,
                                               TEST_GROUP_ID, 12));
    TEST_ASSERT_EQUAL_UINT8(1, groups.groups[0].lampCount);
    TEST_ASSERT_EQUAL_UINT32(TEST_LAMP_ID, groups.groups[0].lampIds[0]);
}

void test_group_capacity_and_revision_updates() {
    GroupRegistry groups;
    MemoryGroupStore store;
    LampRegistry lamps;
    initMainGroups(groups, store);
    initLampRegistry(lamps);
    for (uint32_t index = 0; index < MAX_LAMPS; ++index) {
        TEST_ASSERT_TRUE(addLamp(lamps, pairedLamp(index + 1, TEST_MAIN_ID)));
    }
    for (uint32_t index = 0; index < MAX_GROUPS; ++index) {
        TEST_ASSERT_TRUE(createGroupPersisted(groups, store, index + 1, "Room"));
    }
    TEST_ASSERT_FALSE(createGroupPersisted(groups, store, MAX_GROUPS + 1, "Full"));
    TEST_ASSERT_EQUAL_UINT8(MAX_GROUPS, groups.count);

    for (uint32_t index = 0; index < MAX_GROUP_LAMPS; ++index) {
        TEST_ASSERT_TRUE(addLampToGroupPersisted(groups, lamps, store, 1,
                                                 index + 1));
    }
    TEST_ASSERT_EQUAL_UINT8(MAX_GROUP_LAMPS, groups.groups[0].lampCount);
    const uint32_t revision = groups.groups[0].revision;
    TEST_ASSERT_FALSE(addLampToGroupPersisted(groups, lamps, store, 1, 99));
    TEST_ASSERT_TRUE(renameGroupPersisted(groups, store, 1, "New room", lamps));
    TEST_ASSERT_EQUAL_UINT32(revision + 1, groups.groups[0].revision);
    const uint32_t writesAfterRename = store.getWriteAttemptCount();
    TEST_ASSERT_TRUE(renameGroupPersisted(groups, store, 1, "New room", lamps));
    TEST_ASSERT_EQUAL_UINT32(writesAfterRename, store.getWriteAttemptCount());
}

void test_group_remove_member_and_delete_are_persisted() {
    GroupRegistry groups;
    MemoryGroupStore store;
    LampRegistry lamps;
    initMainGroups(groups, store);
    initLampRegistry(lamps);
    TEST_ASSERT_TRUE(addLamp(lamps, pairedLamp(11, TEST_MAIN_ID)));
    TEST_ASSERT_TRUE(createGroupPersisted(groups, store, 7, "Room"));
    TEST_ASSERT_TRUE(addLampToGroupPersisted(groups, lamps, store, 7, 11));
    TEST_ASSERT_TRUE(removeLampFromGroupPersisted(groups, lamps, store, 7, 11));
    TEST_ASSERT_FALSE(lampBelongsToGroup(groups.groups[0], 11));

    GroupRegistry restarted;
    TEST_ASSERT_EQUAL_INT(static_cast<int>(GroupStoreLoadResult::FOUND),
        static_cast<int>(restoreGroupRegistry(restarted, TEST_MAIN_ID, store)));
    TEST_ASSERT_EQUAL_UINT8(0, restarted.groups[0].lampCount);
    TEST_ASSERT_TRUE(deleteGroupPersisted(restarted, store, 7));
    TEST_ASSERT_EQUAL_UINT8(0, restarted.count);
}

void test_failed_group_write_and_commit_leave_ram_and_saved_snapshot_unchanged() {
    GroupRegistry groups;
    MemoryGroupStore store;
    initMainGroups(groups, store);
    TEST_ASSERT_TRUE(createGroupPersisted(groups, store, 1, "Original"));
    const uint32_t originalRevision = groups.groups[0].revision;
    store.setFailWrites(true);
    TEST_ASSERT_FALSE(createGroupPersisted(groups, store, 2, "Not saved"));
    TEST_ASSERT_FALSE(renameGroupPersisted(groups, store, 1, "Changed", {}));
    TEST_ASSERT_EQUAL_UINT8(1, groups.count);
    TEST_ASSERT_EQUAL_STRING("Original", groups.groups[0].name);
    TEST_ASSERT_EQUAL_UINT32(originalRevision, groups.groups[0].revision);

    store.setFailWrites(false);
    store.setFailCommit(true);
    TEST_ASSERT_FALSE(createGroupPersisted(groups, store, 2, "Commit failed"));
    GroupRegistry restarted;
    TEST_ASSERT_EQUAL_INT(static_cast<int>(GroupStoreLoadResult::FOUND),
        static_cast<int>(restoreGroupRegistry(restarted, TEST_MAIN_ID, store)));
    TEST_ASSERT_EQUAL_UINT8(1, restarted.count);
    TEST_ASSERT_EQUAL_STRING("Original", restarted.groups[0].name);
}

void test_corrupt_and_unknown_store_versions_block_mutations() {
    const GroupStoreLoadResult failures[] = {
        GroupStoreLoadResult::CORRUPT,
        GroupStoreLoadResult::UNSUPPORTED_VERSION,
        GroupStoreLoadResult::ERROR
    };
    for (GroupStoreLoadResult failure : failures) {
        MemoryGroupStore store;
        store.setLoadResultForTest(failure);
        GroupRegistry groups;
        TEST_ASSERT_EQUAL_INT(static_cast<int>(failure),
            static_cast<int>(restoreGroupRegistry(groups, TEST_MAIN_ID, store)));
        TEST_ASSERT_FALSE(groups.persistenceReady);
        TEST_ASSERT_FALSE(createGroupPersisted(groups, store, 1, "Blocked"));
        TEST_ASSERT_EQUAL_UINT32(0, store.getWriteAttemptCount());
    }
}

void test_read_only_listing_does_not_write_nvs_store() {
    MemoryGroupStore store;
    GroupRegistry groups;
    initMainGroups(groups, store);
    TEST_ASSERT_TRUE(createGroupPersisted(groups, store, 1, "Read only"));
    const uint32_t writes = store.getWriteAttemptCount();
    GroupRegistry reloaded;
    TEST_ASSERT_EQUAL_INT(static_cast<int>(GroupStoreLoadResult::FOUND),
        static_cast<int>(restoreGroupRegistry(reloaded, TEST_MAIN_ID, store)));
    TEST_ASSERT_EQUAL_UINT32(writes, store.getWriteAttemptCount());
}

void test_confirmed_removal_cleans_group_before_removing_pairing_and_repair_is_not_auto_added() {
    MemoryProvisioningReservationStore reservations;
    MemoryGroupStore groupStore;
    LampRegistry lamps;
    GroupRegistry groups;
    prepareRemovalState(reservations, lamps, groups, groupStore);
    MainProvisioning main(TEST_MAIN_ID, lamps, &reservations, &groups, &groupStore);
    constexpr uint32_t operationId = 555;
    const uint32_t mainCoreId = makeCoreModuleId(CORE_LOGICAL_ID, TEST_MAIN_ID);
    const uint32_t lampCoreId = makeCoreModuleId(mainCoreId, 11);
    TEST_ASSERT_TRUE(main.beginRemoval(operationId, 11, lampCoreId));

    ProvisioningPacket confirmation = removalConfirmation(operationId);
    ProvisioningPacket commit = {};
    bool completed = false;
    TEST_ASSERT_TRUE(main.reconcileRemovalPacket(confirmation, commit, completed));
    TEST_ASSERT_TRUE(completed);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ProvisioningPacketType::UNPAIR_COMMIT),
                          static_cast<int>(commit.type));
    TEST_ASSERT_NULL(findLamp(lamps, 11));
    TEST_ASSERT_EQUAL_UINT8(0, groups.groups[0].lampCount);

    MainProvisioningReservationState savedReservations = {};
    TEST_ASSERT_TRUE(reservations.load(TEST_MAIN_ID, savedReservations));
    TEST_ASSERT_EQUAL_UINT32(11, savedReservations.highWaterDeviceId);
    TEST_ASSERT_EQUAL_UINT8(0, savedReservations.count);

    Lamp newlyPaired = pairedLamp(12, TEST_MAIN_ID);
    strncpy(newlyPaired.identity.hardwareId, "C6-REMOVE-TEST",
            sizeof(newlyPaired.identity.hardwareId) - 1);
    TEST_ASSERT_TRUE(addLamp(lamps, newlyPaired));
    TEST_ASSERT_FALSE(lampBelongsToGroup(groups.groups[0], 12));
    TEST_ASSERT_FALSE(addLampToGroupPersisted(groups, lamps, groupStore,
                                               TEST_GROUP_ID, 11));
}

void test_group_cleanup_failure_keeps_association_and_membership_intact() {
    MemoryProvisioningReservationStore reservations;
    MemoryGroupStore groupStore;
    LampRegistry lamps;
    GroupRegistry groups;
    prepareRemovalState(reservations, lamps, groups, groupStore);
    MainProvisioning main(TEST_MAIN_ID, lamps, &reservations, &groups, &groupStore);
    constexpr uint32_t operationId = 556;
    const uint32_t mainCoreId = makeCoreModuleId(CORE_LOGICAL_ID, TEST_MAIN_ID);
    const uint32_t lampCoreId = makeCoreModuleId(mainCoreId, 11);
    TEST_ASSERT_TRUE(main.beginRemoval(operationId, 11, lampCoreId));
    groupStore.setFailWrites(true);

    ProvisioningPacket confirmation = removalConfirmation(operationId);
    ProvisioningPacket commit = {};
    bool completed = false;
    TEST_ASSERT_FALSE(main.reconcileRemovalPacket(confirmation, commit, completed));
    TEST_ASSERT_FALSE(completed);
    TEST_ASSERT_NOT_NULL(findLamp(lamps, 11));
    TEST_ASSERT_TRUE(lampBelongsToGroup(groups.groups[0], 11));
    MainProvisioningReservationState savedReservations = {};
    TEST_ASSERT_TRUE(reservations.load(TEST_MAIN_ID, savedReservations));
    TEST_ASSERT_EQUAL_UINT8(MAIN_REMOVAL_REQUESTED, savedReservations.removalState);
    TEST_ASSERT_EQUAL_UINT8(1, savedReservations.count);
}

void test_reservation_write_failure_after_group_cleanup_is_retry_safe() {
    MemoryProvisioningReservationStore reservations;
    MemoryGroupStore groupStore;
    LampRegistry lamps;
    GroupRegistry groups;
    prepareRemovalState(reservations, lamps, groups, groupStore);
    MainProvisioning main(TEST_MAIN_ID, lamps, &reservations, &groups, &groupStore);
    constexpr uint32_t operationId = 557;
    const uint32_t mainCoreId = makeCoreModuleId(CORE_LOGICAL_ID, TEST_MAIN_ID);
    const uint32_t lampCoreId = makeCoreModuleId(mainCoreId, 11);
    TEST_ASSERT_TRUE(main.beginRemoval(operationId, 11, lampCoreId));
    reservations.setFailWrites(true);

    ProvisioningPacket confirmation = removalConfirmation(operationId);
    ProvisioningPacket commit = {};
    bool completed = false;
    TEST_ASSERT_FALSE(main.reconcileRemovalPacket(confirmation, commit, completed));
    TEST_ASSERT_FALSE(completed);
    TEST_ASSERT_NOT_NULL(findLamp(lamps, 11));
    TEST_ASSERT_FALSE(lampBelongsToGroup(groups.groups[0], 11));

    reservations.setFailWrites(false);
    // The existing reservation store deliberately fails closed after a write
    // error. Reconstructing MAIN reloads its still-requested transaction.
    MainProvisioning restartedMain(TEST_MAIN_ID, lamps, &reservations,
                                   &groups, &groupStore);
    TEST_ASSERT_TRUE(restartedMain.reconcileRemovalPacket(
        confirmation, commit, completed));
    TEST_ASSERT_TRUE(completed);
    TEST_ASSERT_NULL(findLamp(lamps, 11));
}

void test_group_storage_does_not_modify_pairing_reservations() {
    MemoryProvisioningReservationStore reservations;
    MainProvisioningReservationState before = {};
    initializeMainProvisioningReservationState(TEST_MAIN_ID, before);
    before.highWaterDeviceId = 11;
    before.count = 1;
    before.identities[0].deviceId = 11;
    strncpy(before.identities[0].hardwareId, "C6-STABLE",
            sizeof(before.identities[0].hardwareId) - 1);
    TEST_ASSERT_TRUE(reservations.save(before));

    MemoryGroupStore groupsStore;
    GroupRegistry groups;
    initMainGroups(groups, groupsStore);
    TEST_ASSERT_TRUE(createGroupPersisted(groups, groupsStore, 1, "Group"));
    MainProvisioningReservationState after = {};
    TEST_ASSERT_TRUE(reservations.load(TEST_MAIN_ID, after));
    TEST_ASSERT_EQUAL_UINT32(before.highWaterDeviceId, after.highWaterDeviceId);
    TEST_ASSERT_EQUAL_UINT8(before.count, after.count);
    TEST_ASSERT_EQUAL_UINT32(before.identities[0].deviceId,
                             after.identities[0].deviceId);
    TEST_ASSERT_EQUAL_STRING(before.identities[0].hardwareId,
                             after.identities[0].hardwareId);
}

void test_rename_write_failure_never_replaces_valid_snapshot() {
    MemoryGroupStore store;
    GroupRegistry groups;
    initMainGroups(groups, store);
    TEST_ASSERT_TRUE(createGroupPersisted(groups, store, 1, "Stable"));
    LampRegistry noLamps;
    initLampRegistry(noLamps);
    store.setFailWrites(true);
    TEST_ASSERT_FALSE(renameGroupPersisted(groups, store, 1, "Failed", noLamps));
    store.setFailWrites(false);
    GroupRegistry restored;
    TEST_ASSERT_EQUAL_INT(static_cast<int>(GroupStoreLoadResult::FOUND),
        static_cast<int>(restoreGroupRegistry(restored, TEST_MAIN_ID, store)));
    TEST_ASSERT_EQUAL_STRING("Stable", restored.groups[0].name);
}

void test_collective_power_fans_out_tracks_each_lamp_and_respects_priority_off() {
    memset(&groupPowerTransport.sent, 0, sizeof(groupPowerTransport.sent));
    groupPowerTransport.sentCount = 0;
    groupPowerTransport.incomingCount = 0;
    TEST_ASSERT_TRUE(initializeGroupPowerRuntime());

    // Two lamp ON: independent child IDs/routes, parent replay does not fan out again.
    uint16_t start = groupPowerTransport.sentCount;
    Message on = groupPowerRequest(1001, TEST_GROUP_ID, true);
    pumpGroupPowerMessage(on);
    TEST_ASSERT_EQUAL_UINT16(2, countGroupPowerChildren(start));
    Message onA = findGroupPowerChild(start, 11);
    Message onB = findGroupPowerChild(start, 12);
    TEST_ASSERT_NOT_EQUAL(0, onA.id);
    TEST_ASSERT_NOT_EQUAL(0, onB.id);
    TEST_ASSERT_NOT_EQUAL(onA.id, onB.id);
    TEST_ASSERT_NOT_EQUAL(on.id, onA.id);
    TEST_ASSERT_EQUAL_UINT32(11, onA.provisioningDeviceId);
    TEST_ASSERT_EQUAL_UINT32(12, onB.provisioningDeviceId);
    TEST_ASSERT_EQUAL_UINT32(1, onA.value);
    TEST_ASSERT_EQUAL_UINT32(1, onB.value);

    uint16_t sentBeforeReplay = groupPowerTransport.sentCount;
    pumpGroupPowerMessage(on);
    TEST_ASSERT_EQUAL_UINT16(2, countGroupPowerChildren(start));
    TEST_ASSERT_TRUE(groupPowerTransport.sentCount > sentBeforeReplay);

    // An ACK from the wrong lamp cannot complete lamp A's child operation.
    Message invalidAck = groupPowerAck(onA);
    invalidAck.sourceId = onB.destinationId;
    pumpGroupPowerMessage(invalidAck);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(GroupProtocolResult::IN_PROGRESS),
        static_cast<int>(lastGroupPowerMemberResult(1001, 11)));

    pumpGroupPowerMessage(groupPowerAck(onA));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(GroupProtocolResult::EXECUTED),
        static_cast<int>(lastGroupPowerMemberResult(1001, 11)));
    fakeNow += 5000;
    groupPowerRuntime.poll();
    TEST_ASSERT_EQUAL_UINT16(1, countGroupPowerChildren(start, 11));
    TEST_ASSERT_EQUAL_UINT16(2, countGroupPowerChildren(start, 12));
    Message onBRetry = findGroupPowerChild(start, 12, 1);
    TEST_ASSERT_EQUAL_UINT32(onB.id, onBRetry.id);
    pumpGroupPowerMessage(groupPowerAck(onBRetry));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(GroupProtocolResult::COMPLETED),
        static_cast<int>(lastGroupPowerResult(1001, GroupProtocolAction::RESULT)));
    sentBeforeReplay = groupPowerTransport.sentCount;
    pumpGroupPowerMessage(on);
    TEST_ASSERT_EQUAL_UINT16(0, countGroupPowerChildren(sentBeforeReplay));
    TEST_ASSERT_TRUE(groupPowerTransport.sentCount > sentBeforeReplay);

    // A status query replays retained results only; it never fans out commands.
    Message statusQuery = on;
    statusQuery.group.action = GroupProtocolAction::QUERY_OPERATION;
    start = groupPowerTransport.sentCount;
    pumpGroupPowerMessage(statusQuery);
    TEST_ASSERT_EQUAL_UINT16(0, countGroupPowerChildren(start));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(GroupProtocolResult::COMPLETED),
        static_cast<int>(lastGroupPowerResult(1001, GroupProtocolAction::RESULT)));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(GroupProtocolResult::EXECUTED),
        static_cast<int>(lastGroupPowerMemberResult(1001, 11)));

    // More than twenty completed operations fit inside the short result window.
    for (uint32_t operationId = 1010; operationId < 1034; ++operationId) {
        start = groupPowerTransport.sentCount;
        Message repeated = groupPowerRequest(operationId, TEST_GROUP_ID,
                                              (operationId & 1U) != 0);
        pumpGroupPowerMessage(repeated);
        Message repeatedA = findGroupPowerChild(start, 11);
        Message repeatedB = findGroupPowerChild(start, 12);
        TEST_ASSERT_NOT_EQUAL(0, repeatedA.id);
        TEST_ASSERT_NOT_EQUAL(0, repeatedB.id);
        pumpGroupPowerMessage(groupPowerAck(repeatedA));
        pumpGroupPowerMessage(groupPowerAck(repeatedB));
        TEST_ASSERT_EQUAL_INT(static_cast<int>(GroupProtocolResult::COMPLETED),
            static_cast<int>(lastGroupPowerResult(operationId,
                GroupProtocolAction::RESULT)));
    }

    // A lost RAM history after reboot is uncertain. Querying it creates a
    // tombstone so a later duplicate SET_POWER cannot execute in this boot.
    Message unknownQuery = groupPowerRequest(1099, TEST_GROUP_ID, true);
    unknownQuery.group.action = GroupProtocolAction::QUERY_OPERATION;
    start = groupPowerTransport.sentCount;
    pumpGroupPowerMessage(unknownQuery);
    TEST_ASSERT_EQUAL_UINT16(0, countGroupPowerChildren(start));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(GroupProtocolResult::UNCERTAIN),
        static_cast<int>(lastGroupPowerResult(1099, GroupProtocolAction::RESULT)));
    pumpGroupPowerMessage(groupPowerRequest(1099, TEST_GROUP_ID, true));
    TEST_ASSERT_EQUAL_UINT16(0, countGroupPowerChildren(start));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(GroupProtocolResult::UNCERTAIN),
        static_cast<int>(lastGroupPowerResult(1099, GroupProtocolAction::RESULT)));

    // A one-lamp group remains an ordinary single-child fan-out.
    start = groupPowerTransport.sentCount;
    Message oneLamp = groupPowerRequest(1002, TEST_GROUP_SINGLE_ID, true);
    pumpGroupPowerMessage(oneLamp);
    TEST_ASSERT_EQUAL_UINT16(1, countGroupPowerChildren(start));
    Message oneChild = findGroupPowerChild(start, 11);
    pumpGroupPowerMessage(groupPowerAck(oneChild));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(GroupProtocolResult::COMPLETED),
        static_cast<int>(lastGroupPowerResult(1002, GroupProtocolAction::RESULT)));

    // Two lamp OFF also uses separate child IDs and reports full execution.
    start = groupPowerTransport.sentCount;
    Message off = groupPowerRequest(1003, TEST_GROUP_ID, false);
    pumpGroupPowerMessage(off);
    Message offA = findGroupPowerChild(start, 11);
    Message offB = findGroupPowerChild(start, 12);
    TEST_ASSERT_EQUAL_UINT16(2, countGroupPowerChildren(start));
    TEST_ASSERT_EQUAL_UINT32(0, offA.value);
    TEST_ASSERT_EQUAL_UINT32(0, offB.value);
    pumpGroupPowerMessage(groupPowerAck(offA));
    pumpGroupPowerMessage(groupPowerAck(offB));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(GroupProtocolResult::COMPLETED),
        static_cast<int>(lastGroupPowerResult(1003, GroupProtocolAction::RESULT)));
    fakeNow += 120001;  // Expire settled results after their query window.

    start = groupPowerTransport.sentCount;
    pumpGroupPowerMessage(groupPowerRequest(1004, TEST_GROUP_EMPTY_ID, true));
    TEST_ASSERT_EQUAL_UINT16(0, countGroupPowerChildren(start));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(GroupProtocolResult::EMPTY_GROUP),
        static_cast<int>(lastGroupPowerResult(1004, GroupProtocolAction::RESULT)));
    pumpGroupPowerMessage(groupPowerRequest(1005, 0xdeadbeef, true));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(GroupProtocolResult::NOT_FOUND),
        static_cast<int>(lastGroupPowerResult(1005, GroupProtocolAction::RESULT)));

    // Offline member is preserved in the aggregate while the online member executes.
    findLamp(groupPowerLamps, 12)->device.status = DeviceStatus::OFFLINE;
    start = groupPowerTransport.sentCount;
    pumpGroupPowerMessage(groupPowerRequest(1006, TEST_GROUP_ID, true));
    Message partialChild = findGroupPowerChild(start, 11);
    TEST_ASSERT_EQUAL_UINT16(1, countGroupPowerChildren(start));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(GroupProtocolResult::OFFLINE),
        static_cast<int>(lastGroupPowerMemberResult(1006, 12)));
    pumpGroupPowerMessage(groupPowerAck(partialChild));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(GroupProtocolResult::PARTIAL),
        static_cast<int>(lastGroupPowerResult(1006, GroupProtocolAction::RESULT)));
    findLamp(groupPowerLamps, 12)->device.status = DeviceStatus::ONLINE;

    // A stale group member whose pairing entry was removed fails independently.
    TEST_ASSERT_TRUE(removeLamp(groupPowerLamps, 12));
    start = groupPowerTransport.sentCount;
    pumpGroupPowerMessage(groupPowerRequest(1007, TEST_GROUP_ID, true));
    Message remainingChild = findGroupPowerChild(start, 11);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(GroupProtocolResult::FAILED),
        static_cast<int>(lastGroupPowerMemberResult(1007, 12)));
    pumpGroupPowerMessage(groupPowerAck(remainingChild));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(GroupProtocolResult::PARTIAL),
        static_cast<int>(lastGroupPowerResult(1007, GroupProtocolAction::RESULT)));
    Lamp restoredLamp = pairedLamp(12, testMainLogicalId());
    restoredLamp.device.capabilities = DEVICE_CAP_POWER;
    TEST_ASSERT_TRUE(addLamp(groupPowerLamps, restoredLamp));

    // Individual output is serialized against a group; individual OFF is the only preemption.
    start = groupPowerTransport.sentCount;
    Message concurrent = groupPowerRequest(1008, TEST_GROUP_ID, true);
    pumpGroupPowerMessage(concurrent);
    Message groupChildA = findGroupPowerChild(start, 11);
    Message groupChildB = findGroupPowerChild(start, 12);
    Message individualOn = {};
    individualOn.id = 8101;
    individualOn.sourceId = CORE_LOGICAL_ID;
    individualOn.destinationId = groupChildA.destinationId;
    individualOn.type = MessageType::COMMAND;
    individualOn.commandType = static_cast<int32_t>(ActionType::SET_LAMP_POWER);
    individualOn.value = 1;
    individualOn.parentMainId = groupChildA.parentMainId;
    individualOn.provisioningDeviceId = 11;
    pumpGroupPowerMessage(individualOn);
    bool sawBusy = false;
    for (uint16_t index = start; index < groupPowerTransport.sentCount; ++index) {
        const Message& message = groupPowerTransport.sent[index].message;
        if (message.type == MessageType::ACK && message.value2 == 8101 &&
            message.executionStatus == ExecutionStatus::FAILED &&
            message.value == static_cast<int32_t>(CoreCommandError::COMMAND_IN_PROGRESS))
            sawBusy = true;
    }
    TEST_ASSERT_TRUE(sawBusy);

    Message priorityOff = individualOn;
    priorityOff.id = 8102;
    priorityOff.value = 0;
    pumpGroupPowerMessage(priorityOff);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(GroupProtocolResult::SUPERSEDED),
        static_cast<int>(lastGroupPowerMemberResult(1008, 11)));
    Message routedPriorityOff = lastRoutedCommand(start, 11);
    TEST_ASSERT_EQUAL_UINT32(8102, routedPriorityOff.id);
    TEST_ASSERT_EQUAL_UINT32(groupChildA.id,
        static_cast<uint32_t>(routedPriorityOff.value2));

    Message removal = {};
    TEST_ASSERT_TRUE(makeCoreRemovalControlMessage(testMainLogicalId(),
        makeCoreModuleId(CORE_LOGICAL_ID, testMainLogicalId()),
        CoreRemovalCommand::REMOVE, 8103, 12,
        makeCoreModuleId(makeCoreModuleId(CORE_LOGICAL_ID,
            testMainLogicalId()), 12), removal));
    pumpGroupPowerMessage(removal);
    bool sawRemovalBusy = false;
    for (uint16_t index = start; index < groupPowerTransport.sentCount; ++index) {
        const Message& message = groupPowerTransport.sent[index].message;
        uint32_t operationId = 0;
        CorePairingResultCode result = CorePairingResultCode::FAILED;
        uint32_t localId = 0;
        char hardwareId[LAMP_HARDWARE_ID_LENGTH] = {};
        if (readCoreRemovalResultMessage(message, testMainLogicalId(),
                makeCoreModuleId(CORE_LOGICAL_ID, testMainLogicalId()),
                operationId, result, localId, hardwareId, sizeof(hardwareId)) &&
            operationId == 8103 && result == CorePairingResultCode::BUSY)
            sawRemovalBusy = true;
    }
    TEST_ASSERT_TRUE(sawRemovalBusy);
    fakeNow += 5000;
    groupPowerRuntime.poll();
    TEST_ASSERT_EQUAL_UINT16(1, countCommandId(start, groupChildA.id));
    TEST_ASSERT_EQUAL_UINT16(2, countCommandId(start, groupChildB.id));
    pumpGroupPowerMessage(groupPowerAck(groupChildB));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(GroupProtocolResult::UNCERTAIN),
        static_cast<int>(lastGroupPowerResult(1008, GroupProtocolAction::RESULT)));
    pumpGroupPowerMessage(groupPowerAck(groupChildA));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(GroupProtocolResult::SUPERSEDED),
        static_cast<int>(lastGroupPowerMemberResult(1008, 11)));
    pumpGroupPowerMessage(groupPowerAck(routedPriorityOff));

    // Lost ACK retries keep the child ID; timeout is uncertain and a late ACK is correlated.
    start = groupPowerTransport.sentCount;
    Message timeoutRequest = groupPowerRequest(1009, TEST_GROUP_SINGLE_ID, true);
    pumpGroupPowerMessage(timeoutRequest);
    Message timedChild = findGroupPowerChild(start, 11);
    fakeNow += 5000;
    groupPowerRuntime.poll();
    TEST_ASSERT_EQUAL_UINT16(2, countGroupPowerChildren(start, 11));
    fakeNow += 5000;
    groupPowerRuntime.poll();
    TEST_ASSERT_EQUAL_UINT16(3, countGroupPowerChildren(start, 11));
    fakeNow += 5000;
    groupPowerRuntime.poll();
    TEST_ASSERT_EQUAL_UINT16(3, countGroupPowerChildren(start, 11));
    TEST_ASSERT_EQUAL_UINT32(timedChild.id,
        findGroupPowerChild(start, 11, 1).id);
    TEST_ASSERT_EQUAL_UINT32(timedChild.id,
        findGroupPowerChild(start, 11, 2).id);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(GroupProtocolResult::UNCERTAIN),
        static_cast<int>(lastGroupPowerResult(1009, GroupProtocolAction::RESULT)));
    pumpGroupPowerMessage(groupPowerAck(timedChild));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(GroupProtocolResult::EXECUTED),
        static_cast<int>(lastGroupPowerMemberResult(1009, 11)));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(GroupProtocolResult::COMPLETED),
        static_cast<int>(lastGroupPowerResult(1009, GroupProtocolAction::RESULT)));
}

namespace {
constexpr uint32_t HTTP_TEST_MAIN_LOCAL_ID = 5;
constexpr uint32_t HTTP_TEST_GROUP_ID = 77;
constexpr const char* HTTP_TEST_TOKEN = "0123456789abcdef0123456789abcdef";

struct HttpGroupTestResponse { uint16_t status; std::string body; };

struct HttpGroupTestFixture {
    DeviceRegistry registry;
    MessageTracker tracker;
    SemaphoreHandle_t registryMutex;
    CoreUartTransport uart;
    WiFiTransport wifi;
    CoreCommandService commands;
    CoreHttpApi api;
    bool ready;

    HttpGroupTestFixture()
        : registry{}, tracker{}, registryMutex(xSemaphoreCreateMutex()), uart(1), wifi(),
          commands(registry, tracker, groupPowerCommunication),
          api(registry, wifi, uart, registryMutex, commands, tracker, HTTP_TEST_TOKEN),
          ready(false) {
        initDeviceRegistry(registry);
        initMessageTracker(tracker);
        Device main = {};
        main.id = makeCoreModuleId(CORE_LOGICAL_ID, HTTP_TEST_MAIN_LOCAL_ID);
        main.name = "MAIN_LIGHTING";
        main.role = DeviceRole::MAIN;
        main.status = DeviceStatus::ONLINE;
        main.capabilities = DEVICE_CAP_GROUPS;
        main.parentId = CORE_LOGICAL_ID;
        main.localId = HTTP_TEST_MAIN_LOCAL_ID;
        ready = registryMutex != nullptr && registerDevice(registry, main) &&
            uart.begin() && api.begin();
        httpGroupApiUnderTest = &api;
    }

    ~HttpGroupTestFixture() {
        httpGroupApiUnderTest = nullptr;
        httpGroupDelayHook = nullptr;
        smart_http_test::resetServer();
    }

    uint32_t mainId() const {
        return makeCoreModuleId(CORE_LOGICAL_ID, HTTP_TEST_MAIN_LOCAL_ID);
    }
};

HttpGroupTestResponse performHttpRequest(HttpGroupTestFixture& fixture,
        httpd_method_t method, const std::string& uri,
        const std::string& body = std::string()) {
    httpd_req_t request = {};
    request.method = method;
    request.uri = uri.c_str();
    request.content_len = body.size();
    request.body = body.c_str();
    request.authorization = std::string("Bearer ") + HTTP_TEST_TOKEN;
    if (!body.empty()) request.contentType = "application/json";
    const esp_err_t result = smart_http_test::dispatch(request);
    unsigned status = 0;
    (void)sscanf(request.status.c_str(), "%u", &status);
    if (result != ESP_OK) status = 599;
    return {static_cast<uint16_t>(status), request.responseBody};
}

CoreLinkPacket makeHttpGroupPacket(const GroupProtocolPayload& group) {
    CoreLinkPacket packet = {};
    packet.sequence = static_cast<uint16_t>(group.operationId);
    packet.type = CoreLinkMessageType::GROUPS;
    packet.group = group;
    return packet;
}



void sendHttpGroupPacketToCurrentApi(const GroupProtocolPayload& group) {
    if (httpGroupApiUnderTest == nullptr) return;
    CoreLinkPacket packet = makeHttpGroupPacket(group);
    httpGroupApiUnderTest->handleGroupPacket(packet);
}

void replyToHttpEmptyList() {
    if (httpGroupApiUnderTest == nullptr || httpGroupUartPacketCount == 0) return;
    const GroupProtocolPayload request =
        httpGroupUartPackets[httpGroupUartPacketCount - 1].group;
    GroupProtocolPayload begin = request;
    begin.action = GroupProtocolAction::LIST_BEGIN;
    begin.result = GroupProtocolResult::NONE;
    begin.groupId = 0;
    begin.expectedRevision = 0;
    begin.memberLocalId = 0;
    begin.groupCount = 0;
    begin.memberCount = 0;
    begin.memberOffset = 0;
    begin.childCommandId = 0;
    begin.name[0] = '\0';
    sendHttpGroupPacketToCurrentApi(begin);
    GroupProtocolPayload end = begin;
    end.action = GroupProtocolAction::LIST_END;
    sendHttpGroupPacketToCurrentApi(end);
}

void replyToHttpGroupDetail() {
    if (httpGroupApiUnderTest == nullptr || httpGroupUartPacketCount == 0) return;
    const GroupProtocolPayload request =
        httpGroupUartPackets[httpGroupUartPacketCount - 1].group;
    GroupProtocolPayload meta = request;
    meta.action = GroupProtocolAction::GROUP_META;
    meta.result = GroupProtocolResult::NONE;
    meta.expectedRevision = 3;
    meta.memberLocalId = 0;
    meta.groupCount = 0;
    meta.memberCount = 0;
    meta.memberOffset = 0;
    meta.childCommandId = 0;
    strncpy(meta.name, "Test group", sizeof(meta.name) - 1);
    sendHttpGroupPacketToCurrentApi(meta);
    GroupProtocolPayload result = request;
    result.action = GroupProtocolAction::RESULT;
    result.result = GroupProtocolResult::COMPLETED;
    result.expectedRevision = 3;
    result.memberLocalId = 0;
    result.groupCount = 0;
    result.memberCount = 0;
    result.memberOffset = 0;
    result.childCommandId = 0;
    result.name[0] = '\0';
    sendHttpGroupPacketToCurrentApi(result);
}

GroupProtocolPayload makeHttpMutationResult(const GroupProtocolPayload& request) {
    GroupProtocolPayload result = request;
    result.action = GroupProtocolAction::RESULT;
    result.result = GroupProtocolResult::PERSISTED;
    result.expectedRevision = 2;
    result.memberLocalId = 0;
    result.childCommandId = 0;
    result.groupCount = 0;
    result.memberCount = 0;
    result.memberOffset = 0;
    result.power = 0;
    result.name[0] = '\0';
    return result;
}

GroupProtocolPayload makeHttpEmptyPowerResult(const GroupProtocolPayload& request) {
    GroupProtocolPayload result = request;
    result.action = GroupProtocolAction::RESULT;
    result.result = GroupProtocolResult::EMPTY_GROUP;
    result.expectedRevision = 0;
    result.memberLocalId = 0;
    result.childCommandId = 0;
    result.groupCount = 0;
    result.memberCount = 0;
    result.memberOffset = 0;
    result.name[0] = '\0';
    return result;
}

GroupProtocolPayload makeHttpPowerAccepted(const GroupProtocolPayload& request,
                                           uint8_t members) {
    GroupProtocolPayload result = request;
    result.action = GroupProtocolAction::RESULT;
    result.result = GroupProtocolResult::ACCEPTED;
    result.groupCount = members;
    result.memberCount = 0;
    result.memberLocalId = 0;
    result.memberOffset = 0;
    result.childCommandId = 0;
    result.name[0] = '\0';
    return result;
}

GroupProtocolPayload makeHttpPowerMemberResult(
    const GroupProtocolPayload& request, GroupProtocolResult memberResult,
    uint32_t localId, uint32_t childId, uint8_t offset = 0,
    uint8_t count = 1) {
    GroupProtocolPayload result = request;
    result.action = GroupProtocolAction::MEMBER_RESULT;
    result.result = memberResult;
    result.groupCount = count;
    result.memberCount = 0;
    result.memberLocalId = localId;
    result.memberOffset = offset;
    result.childCommandId = childId;
    result.name[0] = '\0';
    return result;
}

std::string httpGroupOperationPath(uint32_t operationId) {
    return std::string("/api/v1/group-operations/") +
        std::to_string(operationId);
}

std::string httpGroupCollectionPath(const HttpGroupTestFixture& fixture) {
    return std::string("/api/v1/modules/") + std::to_string(fixture.mainId()) + "/groups";
}

std::string httpGroupItemPath(const HttpGroupTestFixture& fixture) {
    return httpGroupCollectionPath(fixture) + "/" + std::to_string(HTTP_TEST_GROUP_ID);
}

std::string httpGroupPowerPath(const HttpGroupTestFixture& fixture) {
    return httpGroupItemPath(fixture) + "/commands/power";
}

HttpGroupTestResponse submitHttpGroupCreate(HttpGroupTestFixture& fixture,
        uint32_t operationId, const char* name) {
    const std::string body = std::string("{\"operation_id\":") +
        std::to_string(operationId) + ",\"name\":\"" + name + "\"}";
    return performHttpRequest(fixture, HTTP_POST, httpGroupCollectionPath(fixture), body);
}

HttpGroupTestResponse submitHttpGroupRename(HttpGroupTestFixture& fixture,
        uint32_t operationId) {
    const std::string body = std::string("{\"operation_id\":") +
        std::to_string(operationId) +
        ",\"expected_revision\":1,\"name\":\"Renamed\"}";
    return performHttpRequest(fixture, HTTP_PATCH, httpGroupItemPath(fixture), body);
}

HttpGroupTestResponse submitHttpGroupPower(HttpGroupTestFixture& fixture,
        uint32_t operationId, const char* power) {
    const std::string body = std::string("{\"operation_id\":") +
        std::to_string(operationId) + ",\"power\":\"" + power + "\"}";
    return performHttpRequest(fixture, HTTP_POST, httpGroupPowerPath(fixture), body);
}

void test_http_group_repeated_reads_do_not_consume_command_slots() {
    HttpGroupTestFixture fixture;
    TEST_ASSERT_TRUE(fixture.ready);
    for (uint32_t index = 0; index < 40; ++index) {
        httpGroupDelayHook = replyToHttpEmptyList;
        const HttpGroupTestResponse list = performHttpRequest(
            fixture, HTTP_GET, httpGroupCollectionPath(fixture));
        TEST_ASSERT_EQUAL_UINT16(200, list.status);
        TEST_ASSERT_TRUE(list.body.find("\"count\":0") != std::string::npos);

        httpGroupDelayHook = replyToHttpGroupDetail;
        const HttpGroupTestResponse detail = performHttpRequest(
            fixture, HTTP_GET, httpGroupItemPath(fixture));
        TEST_ASSERT_EQUAL_UINT16(200, detail.status);
        TEST_ASSERT_TRUE(detail.body.find("Test group") != std::string::npos);
    }

    HttpGroupTestResponse power = submitHttpGroupPower(fixture, 30001, "on");
    TEST_ASSERT_EQUAL_UINT16(202, power.status);
    GroupProtocolPayload sent = httpGroupUartPackets[httpGroupUartPacketCount - 1].group;
    uint16_t sendsBeforeRetry = httpGroupUartPacketCount;
    const HttpGroupTestResponse pendingRetry =
        submitHttpGroupPower(fixture, 30001, "on");
    TEST_ASSERT_EQUAL_UINT16(202, pendingRetry.status);
    TEST_ASSERT_EQUAL_UINT16(sendsBeforeRetry, httpGroupUartPacketCount);
    sendHttpGroupPacketToCurrentApi(makeHttpEmptyPowerResult(sent));
    power = submitHttpGroupPower(fixture, 30002, "off");
    TEST_ASSERT_EQUAL_UINT16(202, power.status);
    sent = httpGroupUartPackets[httpGroupUartPacketCount - 1].group;
    sendHttpGroupPacketToCurrentApi(makeHttpEmptyPowerResult(sent));

    for (uint32_t index = 0; index < CORE_GROUP_OPERATION_SLOTS - 2; ++index) {
        const uint32_t operationId = 30100 + index;
        const HttpGroupTestResponse mutation = (index % 2 == 0)
            ? submitHttpGroupRename(fixture, operationId)
            : submitHttpGroupCreate(fixture, operationId, "Distinct group");
        TEST_ASSERT_EQUAL_UINT16(202, mutation.status);
        sent = httpGroupUartPackets[httpGroupUartPacketCount - 1].group;
        sendHttpGroupPacketToCurrentApi(makeHttpMutationResult(sent));
    }
    const uint16_t sendsBeforeFull = httpGroupUartPacketCount;
    const HttpGroupTestResponse full = submitHttpGroupRename(fixture, 30200);
    TEST_ASSERT_EQUAL_UINT16(409, full.status);
    TEST_ASSERT_TRUE(full.body.find("group_operation_history_full") != std::string::npos);
    TEST_ASSERT_EQUAL_UINT16(sendsBeforeFull, httpGroupUartPacketCount);
}

void test_http_group_mutation_retry_is_idempotent_and_history_expires_safely() {
    HttpGroupTestFixture fixture;
    TEST_ASSERT_TRUE(fixture.ready);
    HttpGroupTestResponse first = submitHttpGroupCreate(fixture, 51000, "First group");
    TEST_ASSERT_EQUAL_UINT16(202, first.status);
    GroupProtocolPayload firstRequest = httpGroupUartPackets[httpGroupUartPacketCount - 1].group;
    sendHttpGroupPacketToCurrentApi(makeHttpMutationResult(firstRequest));
    const uint16_t sendsAfterFirst = httpGroupUartPacketCount;
    const HttpGroupTestResponse retry = submitHttpGroupCreate(fixture, 51000, "First group");
    TEST_ASSERT_EQUAL_UINT16(200, retry.status);
    TEST_ASSERT_TRUE(retry.body.find("persisted") != std::string::npos);
    TEST_ASSERT_EQUAL_UINT16(sendsAfterFirst, httpGroupUartPacketCount);

    for (uint32_t index = 1; index < CORE_GROUP_OPERATION_SLOTS; ++index) {
        const uint32_t operationId = 51000 + index;
        const HttpGroupTestResponse mutation = (index % 2 == 0)
            ? submitHttpGroupRename(fixture, operationId)
            : submitHttpGroupCreate(fixture, operationId, "Distinct group");
        TEST_ASSERT_EQUAL_UINT16(202, mutation.status);
        const GroupProtocolPayload sent = httpGroupUartPackets[httpGroupUartPacketCount - 1].group;
        sendHttpGroupPacketToCurrentApi(makeHttpMutationResult(sent));
    }
    const HttpGroupTestResponse full = submitHttpGroupRename(fixture, 51100);
    TEST_ASSERT_EQUAL_UINT16(409, full.status);

    fakeNow += 2U * 60U * 1000U + 1U;
    const HttpGroupTestResponse afterRetention = submitHttpGroupCreate(fixture, 51101, "After retention");
    TEST_ASSERT_EQUAL_UINT16(202, afterRetention.status);
    const GroupProtocolPayload current = httpGroupUartPackets[httpGroupUartPacketCount - 1].group;
    sendHttpGroupPacketToCurrentApi(makeHttpMutationResult(firstRequest));
    const HttpGroupTestResponse currentStatus = performHttpRequest(fixture, HTTP_GET,
        std::string("/api/v1/group-operations/") + std::to_string(current.operationId));
    TEST_ASSERT_EQUAL_UINT16(200, currentStatus.status);
    TEST_ASSERT_TRUE(currentStatus.body.find("\"state\":\"sent\"") != std::string::npos);
}

void test_http_group_terminal_window_and_uncertain_result_are_safe() {
    HttpGroupTestFixture fixture;
    TEST_ASSERT_TRUE(fixture.ready);
    uint32_t firstId = 82000;
    for (uint32_t index = 0; index < CORE_GROUP_OPERATION_SLOTS - 1; ++index) {
        const uint32_t operationId = firstId + index;
        const HttpGroupTestResponse submitted = submitHttpGroupPower(
            fixture, operationId, (index & 1U) == 0 ? "on" : "off");
        TEST_ASSERT_EQUAL_UINT16(202, submitted.status);
        const GroupProtocolPayload sent =
            httpGroupUartPackets[httpGroupUartPacketCount - 1].group;
        sendHttpGroupPacketToCurrentApi(makeHttpEmptyPowerResult(sent));
    }

    const std::string firstStatusPath = httpGroupOperationPath(firstId);
    HttpGroupTestResponse firstStatus = performHttpRequest(
        fixture, HTTP_GET, firstStatusPath);
    TEST_ASSERT_EQUAL_UINT16(200, firstStatus.status);
    TEST_ASSERT_TRUE(firstStatus.body.find("empty_group") != std::string::npos);
    const uint16_t sendsBeforeRetry = httpGroupUartPacketCount;
    const HttpGroupTestResponse retry = submitHttpGroupPower(fixture, firstId, "on");
    TEST_ASSERT_EQUAL_UINT16(200, retry.status);
    TEST_ASSERT_EQUAL_UINT16(sendsBeforeRetry, httpGroupUartPacketCount);
    const HttpGroupTestResponse conflictingId = submitHttpGroupPower(
        fixture, firstId, "off");
    TEST_ASSERT_EQUAL_UINT16(409, conflictingId.status);
    TEST_ASSERT_TRUE(conflictingId.body.find("operation_id_conflict") !=
        std::string::npos);
    TEST_ASSERT_EQUAL_UINT16(sendsBeforeRetry, httpGroupUartPacketCount);

    const uint32_t uncertainId = firstId + CORE_GROUP_OPERATION_SLOTS - 1;
    const HttpGroupTestResponse uncertainSubmitted = submitHttpGroupPower(
        fixture, uncertainId, "on");
    TEST_ASSERT_EQUAL_UINT16(202, uncertainSubmitted.status);
    const GroupProtocolPayload uncertainRequest =
        httpGroupUartPackets[httpGroupUartPacketCount - 1].group;
    sendHttpGroupPacketToCurrentApi(makeHttpPowerAccepted(uncertainRequest, 1));
    sendHttpGroupPacketToCurrentApi(makeHttpPowerMemberResult(
        uncertainRequest, GroupProtocolResult::IN_PROGRESS, 11, 88001));
    fakeNow += GROUP_POWER_OPERATION_TIMEOUT_MS + 1;
    fixture.api.pollGroupOperations(fakeNow);

    HttpGroupTestResponse uncertainStatus = performHttpRequest(
        fixture, HTTP_GET, httpGroupOperationPath(uncertainId));
    TEST_ASSERT_EQUAL_UINT16(200, uncertainStatus.status);
    TEST_ASSERT_TRUE(uncertainStatus.body.find("\"state\":\"uncertain\"") !=
        std::string::npos);
    const uint16_t sendsBeforeUncertainRetry = httpGroupUartPacketCount;
    const HttpGroupTestResponse uncertainRetry = submitHttpGroupPower(
        fixture, uncertainId, "on");
    TEST_ASSERT_EQUAL_UINT16(200, uncertainRetry.status);
    TEST_ASSERT_EQUAL_UINT16(sendsBeforeUncertainRetry, httpGroupUartPacketCount);

    const HttpGroupTestResponse full = submitHttpGroupPower(
        fixture, uncertainId + 1, "off");
    TEST_ASSERT_EQUAL_UINT16(409, full.status);
    fakeNow += GROUP_OPERATION_RESULT_RETENTION_MS + 1;
    const HttpGroupTestResponse afterExpiry = submitHttpGroupPower(
        fixture, uncertainId + 2, "off");
    TEST_ASSERT_EQUAL_UINT16(202, afterExpiry.status);

    uncertainStatus = performHttpRequest(fixture, HTTP_GET,
        httpGroupOperationPath(uncertainId));
    TEST_ASSERT_EQUAL_UINT16(200, uncertainStatus.status);
    TEST_ASSERT_TRUE(uncertainStatus.body.find("\"state\":\"uncertain\"") !=
        std::string::npos);
    GroupProtocolPayload lateExecuted = makeHttpPowerMemberResult(
        uncertainRequest, GroupProtocolResult::EXECUTED, 11, 88001);
    sendHttpGroupPacketToCurrentApi(lateExecuted);
    uncertainStatus = performHttpRequest(fixture, HTTP_GET,
        httpGroupOperationPath(uncertainId));
    TEST_ASSERT_EQUAL_UINT16(200, uncertainStatus.status);
    TEST_ASSERT_TRUE(uncertainStatus.body.find("\"result\":\"completed\"") !=
        std::string::npos);
}

void test_http_group_active_record_is_never_recycled() {
    HttpGroupTestFixture fixture;
    TEST_ASSERT_TRUE(fixture.ready);
    const uint32_t firstId = 84000;
    for (uint32_t index = 0; index < CORE_GROUP_OPERATION_SLOTS - 1; ++index) {
        const HttpGroupTestResponse submitted = submitHttpGroupPower(
            fixture, firstId + index, (index & 1U) == 0 ? "on" : "off");
        TEST_ASSERT_EQUAL_UINT16(202, submitted.status);
        const GroupProtocolPayload sent =
            httpGroupUartPackets[httpGroupUartPacketCount - 1].group;
        sendHttpGroupPacketToCurrentApi(makeHttpEmptyPowerResult(sent));
    }

    const uint32_t activeId = firstId + CORE_GROUP_OPERATION_SLOTS - 1;
    const HttpGroupTestResponse active = submitHttpGroupPower(
        fixture, activeId, "on");
    TEST_ASSERT_EQUAL_UINT16(202, active.status);
    const GroupProtocolPayload activeRequest =
        httpGroupUartPackets[httpGroupUartPacketCount - 1].group;
    const uint16_t sendsWhileActive = httpGroupUartPacketCount;

    const HttpGroupTestResponse activeStatus = performHttpRequest(
        fixture, HTTP_GET, httpGroupOperationPath(activeId));
    TEST_ASSERT_EQUAL_UINT16(200, activeStatus.status);
    TEST_ASSERT_TRUE(activeStatus.body.find("\"state\":\"sent\"") !=
        std::string::npos);
    const HttpGroupTestResponse full = submitHttpGroupPower(
        fixture, activeId + 1, "off");
    TEST_ASSERT_EQUAL_UINT16(409, full.status);
    TEST_ASSERT_EQUAL_UINT16(sendsWhileActive, httpGroupUartPacketCount);
    const HttpGroupTestResponse stillActive = performHttpRequest(
        fixture, HTTP_GET, httpGroupOperationPath(activeId));
    TEST_ASSERT_EQUAL_UINT16(200, stillActive.status);
    TEST_ASSERT_TRUE(stillActive.body.find("\"state\":\"sent\"") !=
        std::string::npos);

    sendHttpGroupPacketToCurrentApi(makeHttpEmptyPowerResult(activeRequest));
    fakeNow += GROUP_OPERATION_RESULT_RETENTION_MS + 1;
    const HttpGroupTestResponse afterTerminalRetention = submitHttpGroupPower(
        fixture, activeId + 2, "off");
    TEST_ASSERT_EQUAL_UINT16(202, afterTerminalRetention.status);
}

void test_http_group_lost_post_response_does_not_resend() {
    HttpGroupTestFixture fixture;
    TEST_ASSERT_TRUE(fixture.ready);
    smart_http_test::server().failNextResponseSend = true;
    const uint16_t sendsBeforePost = httpGroupUartPacketCount;
    const HttpGroupTestResponse lost = submitHttpGroupPower(fixture, 83001, "off");
    TEST_ASSERT_EQUAL_UINT16(599, lost.status);
    TEST_ASSERT_EQUAL_UINT16(sendsBeforePost + 1, httpGroupUartPacketCount);

    const HttpGroupTestResponse retry = submitHttpGroupPower(fixture, 83001, "off");
    TEST_ASSERT_EQUAL_UINT16(202, retry.status);
    TEST_ASSERT_EQUAL_UINT16(sendsBeforePost + 1, httpGroupUartPacketCount);
    const GroupProtocolPayload sent = httpGroupUartPackets[sendsBeforePost].group;
    sendHttpGroupPacketToCurrentApi(makeHttpEmptyPowerResult(sent));
    const HttpGroupTestResponse status = performHttpRequest(
        fixture, HTTP_GET, httpGroupOperationPath(83001));
    TEST_ASSERT_EQUAL_UINT16(200, status.status);
    TEST_ASSERT_TRUE(status.body.find("empty_group") != std::string::npos);
}

void test_http_group_partial_member_results_are_retained() {
    HttpGroupTestFixture fixture;
    TEST_ASSERT_TRUE(fixture.ready);
    const HttpGroupTestResponse submitted = submitHttpGroupPower(
        fixture, 84001, "on");
    TEST_ASSERT_EQUAL_UINT16(202, submitted.status);
    const GroupProtocolPayload sent =
        httpGroupUartPackets[httpGroupUartPacketCount - 1].group;
    sendHttpGroupPacketToCurrentApi(makeHttpPowerAccepted(sent, 2));
    sendHttpGroupPacketToCurrentApi(makeHttpPowerMemberResult(
        sent, GroupProtocolResult::EXECUTED, 11, 88101, 0, 2));
    sendHttpGroupPacketToCurrentApi(makeHttpPowerMemberResult(
        sent, GroupProtocolResult::OFFLINE, 12, 88102, 1, 2));
    const HttpGroupTestResponse status = performHttpRequest(
        fixture, HTTP_GET, httpGroupOperationPath(84001));
    TEST_ASSERT_EQUAL_UINT16(200, status.status);
    TEST_ASSERT_TRUE(status.body.find("\"result\":\"partial\"") !=
        std::string::npos);
    TEST_ASSERT_TRUE(status.body.find("\"lamp_id\":") != std::string::npos);
    TEST_ASSERT_TRUE(status.body.find("\"child_id\":88101") != std::string::npos);
    TEST_ASSERT_TRUE(status.body.find("\"child_id\":88102") != std::string::npos);
}

void test_http_group_reads_are_transient_and_late_read_replies_are_ignored() {
    HttpGroupTestFixture fixture;
    TEST_ASSERT_TRUE(fixture.ready);
    const HttpGroupTestResponse timedOut = performHttpRequest(
        fixture, HTTP_GET, httpGroupCollectionPath(fixture));
    TEST_ASSERT_EQUAL_UINT16(503, timedOut.status);
    TEST_ASSERT_TRUE(timedOut.body.find("group_result_uncertain") != std::string::npos);
    const GroupProtocolPayload expiredRead =
        httpGroupUartPackets[httpGroupUartPacketCount - 1].group;
    GroupProtocolPayload lateBegin = expiredRead;
    lateBegin.action = GroupProtocolAction::LIST_BEGIN;
    lateBegin.result = GroupProtocolResult::NONE;
    lateBegin.groupCount = 0;
    lateBegin.name[0] = '\0';
    sendHttpGroupPacketToCurrentApi(lateBegin);
    GroupProtocolPayload lateEnd = lateBegin;
    lateEnd.action = GroupProtocolAction::LIST_END;
    sendHttpGroupPacketToCurrentApi(lateEnd);

    httpGroupDelayHook = replyToHttpEmptyList;
    const HttpGroupTestResponse recovered = performHttpRequest(
        fixture, HTTP_GET, httpGroupCollectionPath(fixture));
    TEST_ASSERT_EQUAL_UINT16(200, recovered.status);

    HttpGroupTestResponse power = submitHttpGroupPower(fixture, 70001, "on");
    TEST_ASSERT_EQUAL_UINT16(202, power.status);
    const uint16_t sendsDuringPower = httpGroupUartPacketCount;
    httpGroupDelayHook = replyToHttpEmptyList;
    const HttpGroupTestResponse blockedRead = performHttpRequest(
        fixture, HTTP_GET, httpGroupCollectionPath(fixture));
    TEST_ASSERT_EQUAL_UINT16(409, blockedRead.status);
    TEST_ASSERT_EQUAL_UINT16(sendsDuringPower, httpGroupUartPacketCount);
    httpGroupDelayHook = nullptr;

    for (uint32_t index = 0; index < 40; ++index) {
        const HttpGroupTestResponse status = performHttpRequest(fixture, HTTP_GET,
            std::string("/api/v1/group-operations/") + std::to_string(70001));
        TEST_ASSERT_EQUAL_UINT16(200, status.status);
    }
    GroupProtocolPayload sent = httpGroupUartPackets[httpGroupUartPacketCount - 1].group;
    sendHttpGroupPacketToCurrentApi(makeHttpEmptyPowerResult(sent));
    httpGroupDelayHook = replyToHttpEmptyList;
    const HttpGroupTestResponse listAfterPower = performHttpRequest(
        fixture, HTTP_GET, httpGroupCollectionPath(fixture));
    TEST_ASSERT_EQUAL_UINT16(200, listAfterPower.status);
}

}  // namespace

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_group_store_starts_empty_and_restores_committed_snapshot);
    RUN_TEST(test_group_name_is_owned_and_bounded);
    RUN_TEST(test_group_duplicate_id_and_duplicate_members_are_rejected);
    RUN_TEST(test_group_members_require_registered_lamp_on_same_main_and_keep_uint32_id);
    RUN_TEST(test_group_capacity_and_revision_updates);
    RUN_TEST(test_group_remove_member_and_delete_are_persisted);
    RUN_TEST(test_failed_group_write_and_commit_leave_ram_and_saved_snapshot_unchanged);
    RUN_TEST(test_corrupt_and_unknown_store_versions_block_mutations);
    RUN_TEST(test_read_only_listing_does_not_write_nvs_store);
    RUN_TEST(test_confirmed_removal_cleans_group_before_removing_pairing_and_repair_is_not_auto_added);
    RUN_TEST(test_group_cleanup_failure_keeps_association_and_membership_intact);
    RUN_TEST(test_reservation_write_failure_after_group_cleanup_is_retry_safe);
    RUN_TEST(test_group_storage_does_not_modify_pairing_reservations);
    RUN_TEST(test_rename_write_failure_never_replaces_valid_snapshot);
    RUN_TEST(test_collective_power_fans_out_tracks_each_lamp_and_respects_priority_off);
    RUN_TEST(test_http_group_repeated_reads_do_not_consume_command_slots);
    RUN_TEST(test_http_group_mutation_retry_is_idempotent_and_history_expires_safely);
    RUN_TEST(test_http_group_terminal_window_and_uncertain_result_are_safe);
    RUN_TEST(test_http_group_active_record_is_never_recycled);
    RUN_TEST(test_http_group_lost_post_response_does_not_resend);
    RUN_TEST(test_http_group_partial_member_results_are_retained);
    RUN_TEST(test_http_group_reads_are_transient_and_late_read_replies_are_ignored);
    return UNITY_END();
}
