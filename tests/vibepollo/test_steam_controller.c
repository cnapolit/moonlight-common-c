// Parsing of the Vibepollo Steam Controller (2026) messages (0x5504, 0x5505) and the
// attributes input packet.
#include "SteamController.h"
#include "test.h"

#include <string.h>

static int hapticCalls, settingsCalls;
static uint16_t lastController;
static uint8_t lastLength, lastCommand, lastCount;
static const uint8_t* lastData;

static void onHaptic(uint16_t controllerNumber, uint8_t length, const uint8_t* report) {
    hapticCalls++;
    lastController = controllerNumber;
    lastLength = length;
    lastData = report;
}

static void onSettings(uint16_t controllerNumber, uint8_t command, uint8_t count, const uint8_t* triples) {
    settingsCalls++;
    lastController = controllerNumber;
    lastCommand = command;
    lastCount = count;
    lastData = triples;
}

static int haptic(const uint8_t* p, size_t size) {
    int before = hapticCalls;
    int ret = MlSteamHapticDispatch(p, size, onHaptic);
    CHECK(ret == hapticCalls - before);
    return ret;
}

static int settings(const uint8_t* p, size_t size) {
    int before = settingsCalls;
    int ret = MlSteamSettingsDispatch(p, size, onSettings);
    CHECK(ret == settingsCalls - before);
    return ret;
}

static void buildHaptic(uint8_t* p, uint16_t controller, uint8_t length) {
    int i;
    p[0] = (uint8_t)controller; p[1] = (uint8_t)(controller >> 8);
    p[2] = length;
    for (i = 0; i < STEAM_HAPTIC_REPORT_MAX; i++) p[3 + i] = (uint8_t)(0x80 + i);
}

static void buildSettings(uint8_t* p, uint16_t controller, uint8_t command, uint8_t count) {
    int i;
    p[0] = (uint8_t)controller; p[1] = (uint8_t)(controller >> 8);
    p[2] = command;
    p[3] = count;
    for (i = 0; i < STEAM_SETTINGS_PAYLOAD_SIZE; i++) p[4 + i] = (uint8_t)i;
}

int main(void) {
    uint8_t buf[64];
    size_t i;
    int length, count;

    // Haptic report: fixed 15-byte payload
    CHECK(ML_STEAM_HAPTIC_PAYLOAD_SIZE == 15);
    buildHaptic(buf, 2, 10);
    CHECK(haptic(buf, ML_STEAM_HAPTIC_PAYLOAD_SIZE));
    CHECK(lastController == 2 && lastLength == 10 && lastData == buf + 3);
    for (i = 0; i < ML_STEAM_HAPTIC_PAYLOAD_SIZE; i++) {
        CHECK(!haptic(buf, i));
    }
    CHECK(MlSteamHapticDispatch(buf, ML_STEAM_HAPTIC_PAYLOAD_SIZE, NULL) == 0);
    CHECK(MlSteamHapticDispatch(NULL, ML_STEAM_HAPTIC_PAYLOAD_SIZE, onHaptic) == 0);
    for (length = 0; length < 256; length++) {
        buildHaptic(buf, 0, (uint8_t)length);
        CHECK(haptic(buf, ML_STEAM_HAPTIC_PAYLOAD_SIZE) == (length >= 2 && length <= STEAM_HAPTIC_REPORT_MAX));
    }
    buildHaptic(buf, 16, 4);
    CHECK(!haptic(buf, ML_STEAM_HAPTIC_PAYLOAD_SIZE));

    // Settings write: fixed 31-byte payload
    CHECK(ML_STEAM_SETTINGS_PAYLOAD_SIZE == 31);
    buildSettings(buf, 1, STEAM_SETTINGS_SET_VALUES, 9);
    CHECK(settings(buf, ML_STEAM_SETTINGS_PAYLOAD_SIZE));
    CHECK(lastController == 1 && lastCommand == STEAM_SETTINGS_SET_VALUES && lastCount == 9 && lastData == buf + 4);
    for (i = 0; i < ML_STEAM_SETTINGS_PAYLOAD_SIZE; i++) {
        CHECK(!settings(buf, i));
    }
    CHECK(MlSteamSettingsDispatch(buf, ML_STEAM_SETTINGS_PAYLOAD_SIZE, NULL) == 0);
    for (count = 0; count < 256; count++) {
        buildSettings(buf, 0, STEAM_SETTINGS_SET_VALUES, (uint8_t)count);
        CHECK(settings(buf, ML_STEAM_SETTINGS_PAYLOAD_SIZE) == (count >= 1 && count <= STEAM_SETTINGS_TRIPLES_MAX));
        buildSettings(buf, 0, STEAM_SETTINGS_LOAD_DEFAULTS, (uint8_t)count);
        CHECK(settings(buf, ML_STEAM_SETTINGS_PAYLOAD_SIZE) == (count == 0));
        buildSettings(buf, 0, 0x88, (uint8_t)count);
        CHECK(!settings(buf, ML_STEAM_SETTINGS_PAYLOAD_SIZE));
    }
    buildSettings(buf, 16, STEAM_SETTINGS_LOAD_DEFAULTS, 0);
    CHECK(!settings(buf, ML_STEAM_SETTINGS_PAYLOAD_SIZE));

    // Attributes input packet
    {
        LI_STEAM_CONTROLLER_ATTRIBUTES a;
        uint8_t pkt[ML_STEAM_ATTRIBUTES_SIZE + 4];

        memset(&a, 0, sizeof(a));
        a.transport = LI_SCATTR_TRANSPORT_BLE;
        a.validMask = 0xFF;
        a.firmwareBuildTime = 0x01020304;
        a.bootloaderBuildTime = 0x05060708;
        a.boardRevision = 9;
        a.capabilities = 0xA0B0C0D0;
        a.radioFirmwareBuildTime = 0x11121314;
        a.dongleFirmwareBuildTime = 0;
        memcpy(a.buildId, "ABCDEF123456", 12);
        memcpy(a.unitSerial, "FXA0123456789XYZ", 16);  // 16 characters and no NUL: cut to 15
        memcpy(a.boardSerial, "MX\x01\x7F", 4);

        memset(pkt, 0xEE, sizeof(pkt));
        CHECK(MlSteamBuildAttributes(pkt, sizeof(pkt), 3, &a));
        CHECK(pkt[0] == 0 && pkt[1] == 0 && pkt[2] == 0 && pkt[3] == 80);
        CHECK(pkt[4] == 0x08 && pkt[5] == 0x00 && pkt[6] == 0x00 && pkt[7] == 0x55);
        CHECK(pkt[8] == 3 && pkt[9] == LI_SCATTR_TRANSPORT_BLE && pkt[10] == 0xFF && pkt[11] == 0);
        CHECK(pkt[12] == 0x04 && pkt[15] == 0x01);
        CHECK(pkt[16] == 0x08 && pkt[20] == 9 && pkt[24] == 0xD0 && pkt[27] == 0xA0 && pkt[28] == 0x14);
        CHECK(pkt[32] == 0 && pkt[35] == 0);
        CHECK(memcmp(pkt + 36, "ABCDEF123456\0\0\0\0", 16) == 0);
        CHECK(memcmp(pkt + 52, "FXA0123456789XY\0", 16) == 0);
        CHECK(memcmp(pkt + 68, "MX??\0\0\0\0\0\0\0\0\0\0\0\0", 16) == 0);
        CHECK(pkt[ML_STEAM_ATTRIBUTES_SIZE] == 0xEE);

        CHECK(!MlSteamBuildAttributes(pkt, ML_STEAM_ATTRIBUTES_SIZE - 1, 0, &a));
        CHECK(!MlSteamBuildAttributes(pkt, sizeof(pkt), 16, &a));
        CHECK(!MlSteamBuildAttributes(pkt, sizeof(pkt), 0, NULL));
    }

    return TEST_RESULT();
}
