#include <unity.h>

#include <stdio.h>
#include <string.h>

#include "core_http_auth.h"
#include "core_pairing_protocol.h"
#include "core_pairing_state.h"
#include "communication.h"

FakeHardwareSerial Serial;
static uint32_t fakeMillis = 1000;

uint32_t millis() { return fakeMillis; }
bool receiveMessage(Communication&, Message&) { return false; }
bool sendMessage(Communication&, Message) { return true; }

void setUp() { fakeMillis = 1000; }
void tearDown() {}

void test_bearer_and_pairing_input_validation() {
    const char token[] = "0123456789abcdef0123456789abcdef";
    TEST_ASSERT_TRUE(coreHttpTokenConfigured(token));
    TEST_ASSERT_FALSE(coreHttpTokenConfigured(nullptr));
    TEST_ASSERT_FALSE(coreHttpTokenConfigured("short"));
    const char valid[] = "Bearer 0123456789abcdef0123456789abcdef";
    const char wrong[] = "Bearer 0123456789abcdef0123456789abcde0";
    TEST_ASSERT_TRUE(coreHttpHasValidBearer(valid, sizeof(valid) - 1, token));
    TEST_ASSERT_FALSE(coreHttpHasValidBearer(nullptr, 0, token));
    TEST_ASSERT_FALSE(coreHttpHasValidBearer("", 0, token));
    TEST_ASSERT_FALSE(coreHttpHasValidBearer(wrong, sizeof(wrong) - 1, token));
    TEST_ASSERT_FALSE(coreHttpHasValidBearer(
        "bearer 0123456789abcdef0123456789abcdef", 39, token));
    TEST_ASSERT_FALSE(coreHttpHasValidBearer("Bearer short", 12, "short"));
    TEST_ASSERT_FALSE(isValidCorePairingHardwareId("C6-0123456789ABCDE"));
    TEST_ASSERT_TRUE(isValidCorePairingHardwareId("C6-0123456789ABCDEF"));
    TEST_ASSERT_FALSE(isValidCorePairingHardwareId("C6-0123456789ABCDE\""));
    TEST_ASSERT_TRUE(isValidCorePairingName("LAMP 2"));
    TEST_ASSERT_FALSE(isValidCorePairingName("Lamp\"2"));
    TEST_ASSERT_FALSE(isValidCorePairingName("lamp/#"));
}

void test_session_candidate_and_operation_correlation() {
    CorePairingState pairing;
    TEST_ASSERT_EQUAL_INT(static_cast<int>(CorePairingSessionState::NONE),
                          static_cast<int>(pairing.getSessionState()));
    TEST_ASSERT_FALSE(pairing.addCandidate(10, 20, "C6-0123456789ABCDE", 1, 1));
    TEST_ASSERT_TRUE(pairing.beginSession(10, 20, 60, 1000));
    TEST_ASSERT_FALSE(pairing.beginSession(11, 20, 60, 1001));
    TEST_ASSERT_TRUE(pairing.applySessionResult(10, CorePairingResultCode::ACTIVE, 1100));
    TEST_ASSERT_TRUE(pairing.addCandidate(10, 20, "C6-0123456789ABCDEF", 3, 1200));
    TEST_ASSERT_TRUE(pairing.addCandidate(10, 20, "C6-0123456789ABCDEF", 7, 1300));
    TEST_ASSERT_EQUAL_UINT8(1, pairing.getCandidateCount());
    TEST_ASSERT_EQUAL_UINT32(1300, pairing.getCandidate(0)->lastSeen);
    TEST_ASSERT_EQUAL_UINT32(7, pairing.getCandidate(0)->capabilities);

    TEST_ASSERT_TRUE(pairing.beginOperation(10, 100, "C6-0123456789ABCDEF",
                                             "desk lamp", 1400));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(CorePairingOperationState::PENDING),
                          static_cast<int>(pairing.getOperationState()));
    TEST_ASSERT_FALSE(pairing.applyOperationResult(10, 101,
        CorePairingResultCode::SUCCEEDED, 11, "C6-0123456789ABCDEF", 1500));
    TEST_ASSERT_FALSE(pairing.applyOperationResult(10, 100,
        CorePairingResultCode::SUCCEEDED, 11, "C6-0123456789ABCDE0", 1500));
    TEST_ASSERT_TRUE(pairing.applyOperationResult(10, 100,
        CorePairingResultCode::PROCESSING, 0, "C6-0123456789ABCDEF", 1600));
    TEST_ASSERT_TRUE(pairing.applyOperationResult(10, 100,
        CorePairingResultCode::SUCCEEDED, 11, "C6-0123456789ABCDEF", 1700));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(CorePairingOperationState::SUCCEEDED),
                          static_cast<int>(pairing.getOperationState()));
    TEST_ASSERT_EQUAL_UINT32(11, pairing.getOperationDeviceId());
    TEST_ASSERT_EQUAL_UINT8(0, pairing.getCandidateCount());
}

void test_candidate_limit_and_expiration_are_bounded() {
    CorePairingState pairing;
    TEST_ASSERT_TRUE(pairing.beginSession(20, 30, 60, 1000));
    TEST_ASSERT_TRUE(pairing.applySessionResult(20, CorePairingResultCode::ACTIVE, 1001));
    char hardwareId[32] = {};
    for (uint8_t index = 0; index < CORE_PAIRING_MAX_CANDIDATES; ++index) {
        snprintf(hardwareId, sizeof(hardwareId), "C6-%016X", index + 1U);
        TEST_ASSERT_TRUE(pairing.addCandidate(20, 30, hardwareId, 0, 1100 + index));
    }
    TEST_ASSERT_EQUAL_UINT8(CORE_PAIRING_MAX_CANDIDATES, pairing.getCandidateCount());
    TEST_ASSERT_FALSE(pairing.addCandidate(20, 30, "C6-FFFFFFFFFFFFFFFF", 0, 2000));
    TEST_ASSERT_TRUE(pairing.hasCandidateLimit());
    pairing.poll(31110);
    TEST_ASSERT_EQUAL_UINT8(0, pairing.getCandidateCount());
    TEST_ASSERT_TRUE(pairing.applySessionResult(20, CorePairingResultCode::CLOSED, 31200));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(CorePairingSessionState::CLOSED),
                          static_cast<int>(pairing.getSessionState()));
}

void test_capacity_failure_and_lost_result_remain_explicit() {
    CorePairingState pairing;
    TEST_ASSERT_TRUE(pairing.beginSession(40, 50, 30, 1000));
    TEST_ASSERT_TRUE(pairing.applySessionResult(40, CorePairingResultCode::ACTIVE, 1001));
    TEST_ASSERT_TRUE(pairing.addCandidate(40, 50, "C6-0123456789ABCDEF", 0, 1100));
    TEST_ASSERT_TRUE(pairing.beginOperation(40, 400, "C6-0123456789ABCDEF", "lamp", 1200));
    TEST_ASSERT_TRUE(pairing.applyOperationResult(40, 400,
        CorePairingResultCode::ROUTE_UNAVAILABLE, 0,
        "C6-0123456789ABCDEF", 1300));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(CorePairingOperationState::FAILED),
                          static_cast<int>(pairing.getOperationState()));
    TEST_ASSERT_EQUAL_STRING("route_unavailable",
        corePairingResultCodeName(pairing.getOperationResult()));
    TEST_ASSERT_EQUAL_UINT8(1, pairing.getCandidateCount());

    TEST_ASSERT_TRUE(pairing.beginOperation(40, 401, "C6-0123456789ABCDEF", "lamp", 1400));
    TEST_ASSERT_TRUE(pairing.applyOperationResult(40, 401,
        CorePairingResultCode::PROCESSING, 0, "C6-0123456789ABCDEF", 1500));
    pairing.markCommunicationLost(1600);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(CorePairingSessionState::UNCERTAIN),
                          static_cast<int>(pairing.getSessionState()));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(CorePairingOperationState::UNCERTAIN),
                          static_cast<int>(pairing.getOperationState()));
    TEST_ASSERT_FALSE(pairing.applyOperationResult(40, 401,
        CorePairingResultCode::SUCCEEDED, 11, "C6-0123456789ABCDEF", 1700));

    CorePairingState afterRestart;
    TEST_ASSERT_EQUAL_INT(static_cast<int>(CorePairingSessionState::NONE),
                          static_cast<int>(afterRestart.getSessionState()));
    TEST_ASSERT_EQUAL_UINT8(0, afterRestart.getCandidateCount());
}

void test_session_close_retries_and_operation_timeout_remain_uncertain() {
    CorePairingState pairing;
    TEST_ASSERT_TRUE(pairing.beginSession(60, 70, 15, 1000));
    TEST_ASSERT_TRUE(pairing.applySessionResult(60, CorePairingResultCode::ACTIVE, 1001));
    pairing.poll(21002);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(CorePairingSessionState::UNCERTAIN),
                          static_cast<int>(pairing.getSessionState()));
    TEST_ASSERT_TRUE(pairing.beginClose(60, 21003));
    TEST_ASSERT_TRUE(pairing.applySessionResult(60, CorePairingResultCode::FAILED, 21004));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(CorePairingSessionState::UNCERTAIN),
                          static_cast<int>(pairing.getSessionState()));
    TEST_ASSERT_TRUE(pairing.beginClose(60, 21005));
    TEST_ASSERT_TRUE(pairing.applySessionResult(60, CorePairingResultCode::CLOSED, 21006));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(CorePairingSessionState::CLOSED),
                          static_cast<int>(pairing.getSessionState()));

    CorePairingState expired;
    TEST_ASSERT_TRUE(expired.beginSession(61, 70, 60, 1000));
    TEST_ASSERT_TRUE(expired.applySessionResult(61, CorePairingResultCode::ACTIVE, 1001));
    TEST_ASSERT_TRUE(expired.applySessionResult(61, CorePairingResultCode::EXPIRED, 2000));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(CorePairingSessionState::EXPIRED),
                          static_cast<int>(expired.getSessionState()));

    CorePairingState operationTimeout;
    TEST_ASSERT_TRUE(operationTimeout.beginSession(62, 70, 60, 1000));
    TEST_ASSERT_TRUE(operationTimeout.applySessionResult(62,
        CorePairingResultCode::ACTIVE, 1001));
    TEST_ASSERT_TRUE(operationTimeout.addCandidate(62, 70,
        "C6-0123456789ABCDEF", 0, 1100));
    TEST_ASSERT_TRUE(operationTimeout.beginOperation(62, 620,
        "C6-0123456789ABCDEF", "lamp", 1200));
    operationTimeout.poll(36201);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(CorePairingOperationState::UNCERTAIN),
                          static_cast<int>(operationTimeout.getOperationState()));
    TEST_ASSERT_FALSE(operationTimeout.applyOperationResult(62, 620,
        CorePairingResultCode::SUCCEEDED, 11,
        "C6-0123456789ABCDEF", 36202));
}

void test_pairing_message_fields_are_correlated_and_validated() {
    Message command = {};
    TEST_ASSERT_TRUE(makeCorePairingSessionCommand(20, 77, true, 60, command));
    CorePairingCommand parsedCommand = CorePairingCommand::CLOSE_SESSION;
    uint32_t sessionId = 0;
    uint16_t duration = 0;
    TEST_ASSERT_TRUE(readCorePairingSessionCommand(command, 20, parsedCommand,
                                                   sessionId, duration));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(CorePairingCommand::OPEN_SESSION),
                          static_cast<int>(parsedCommand));
    TEST_ASSERT_EQUAL_UINT32(77, sessionId);
    TEST_ASSERT_EQUAL_UINT16(60, duration);
    TEST_ASSERT_FALSE(readCorePairingSessionCommand(command, 21, parsedCommand,
                                                    sessionId, duration));

    ProvisioningPacket candidate = {};
    candidate.type = ProvisioningPacketType::DEVICE_ANNOUNCE;
    candidate.role = DeviceRole::LAMP;
    candidate.pairingState = PairingState::UNPAIRED;
    strncpy(candidate.hardwareId, "C6-0123456789ABCDEF", sizeof(candidate.hardwareId) - 1);
    Message announcement = {};
    TEST_ASSERT_TRUE(makeCorePairingCandidateMessage(20, 77, candidate, announcement));
    uint32_t mainId = 0;
    TEST_ASSERT_TRUE(readCorePairingCandidateMessage(announcement, mainId, sessionId));
    TEST_ASSERT_EQUAL_UINT32(20, mainId);
    TEST_ASSERT_EQUAL_UINT32(77, sessionId);

    TEST_ASSERT_TRUE(makeCorePairingOperationCommand(20, 77, 88,
        candidate.hardwareId, "lamp", command));
    uint32_t operationId = 0;
    TEST_ASSERT_TRUE(readCorePairingOperationCommand(command, 20, sessionId,
                                                      operationId));
    TEST_ASSERT_EQUAL_UINT32(77, sessionId);
    TEST_ASSERT_EQUAL_UINT32(88, operationId);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_bearer_and_pairing_input_validation);
    RUN_TEST(test_session_candidate_and_operation_correlation);
    RUN_TEST(test_candidate_limit_and_expiration_are_bounded);
    RUN_TEST(test_capacity_failure_and_lost_result_remain_explicit);
    RUN_TEST(test_session_close_retries_and_operation_timeout_remain_uncertain);
    RUN_TEST(test_pairing_message_fields_are_correlated_and_validated);
    return UNITY_END();
}
