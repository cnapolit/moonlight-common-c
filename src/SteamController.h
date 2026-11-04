#pragma once

// Vibepollo protocol extensions for the Steam Controller (2026).
//
// Host to client, on the encrypted control stream (reliable, channel 0), sent only for a
// controller whose arrival carries the matching capability, all little endian, fixed size:
//   0x5504 haptic report:   u16 controllerNumber, u8 length (2..12), u8 report[12]
//   0x5505 settings write:  u16 controllerNumber, u8 command, u8 count, u8 triples[27]
//                           (command 0x87 with count 1..9, or 0x8E with count 0)
// LI_CCAP_STEAM_HAPTIC / LI_CCAP_STEAM_SETTINGS are stripped from arrival events unless the
// matching callback was registered with LiStartConnectionWithSteamController(), so stock and
// Vibepollo hosts alike never send these to a client that did not ask.
//
// Client to host, an input packet on the controller's reliable channel (84 bytes):
//   NV_INPUT_HEADER (BE32 size = 80, LE32 magic 0x55000008), u8 controllerNumber, u8 transport,
//   u8 validMask, u8 reserved, six LE32 values, three 16-byte NUL-padded strings.

#include "Limelight.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define ML_STEAM_HAPTIC_PACKET_TYPE 0x5504
#define ML_STEAM_HAPTIC_PAYLOAD_SIZE (2 + 1 + STEAM_HAPTIC_REPORT_MAX)
#define ML_STEAM_SETTINGS_PACKET_TYPE 0x5505
#define ML_STEAM_SETTINGS_PAYLOAD_SIZE (2 + 1 + 1 + STEAM_SETTINGS_PAYLOAD_SIZE)

#define ML_STEAM_ATTRIBUTES_MAGIC 0x55000008
#define ML_STEAM_ATTRIBUTES_SIZE 84

static inline uint16_t MlSteamRead16(const uint8_t* p) {
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

static inline void MlSteamWrite32(uint8_t* p, uint32_t n) {
    p[0] = (uint8_t)n;
    p[1] = (uint8_t)(n >> 8);
    p[2] = (uint8_t)(n >> 16);
    p[3] = (uint8_t)(n >> 24);
}

// Validates a 0x5504 payload and, if it is well formed, invokes the callback with the report
// borrowed from the payload. Returns 1 if the callback was invoked.
static inline int MlSteamHapticDispatch(const uint8_t* p, size_t size, ConnListenerSteamHaptic callback) {
    if (callback == NULL || p == NULL || size < ML_STEAM_HAPTIC_PAYLOAD_SIZE ||
            MlSteamRead16(p) >= 16 || p[2] < 2 || p[2] > STEAM_HAPTIC_REPORT_MAX) {
        return 0;
    }

    callback(MlSteamRead16(p), p[2], p + 3);
    return 1;
}

// Validates a 0x5505 payload and, if it is well formed, invokes the callback with the triples
// borrowed from the payload. A write the command or the array cannot carry is dropped whole
// rather than applied in part. Returns 1 if the callback was invoked.
static inline int MlSteamSettingsDispatch(const uint8_t* p, size_t size, ConnListenerSteamSettings callback) {
    uint8_t command, count;

    if (callback == NULL || p == NULL || size < ML_STEAM_SETTINGS_PAYLOAD_SIZE || MlSteamRead16(p) >= 16) {
        return 0;
    }

    command = p[2];
    count = p[3];
    if (command == STEAM_SETTINGS_SET_VALUES ? (count == 0 || count > STEAM_SETTINGS_TRIPLES_MAX)
                                             : (command != STEAM_SETTINGS_LOAD_DEFAULTS || count != 0)) {
        return 0;
    }

    callback(MlSteamRead16(p), command, count, p + 4);
    return 1;
}

// Copies at most 15 characters, NUL pads, and replaces anything outside printable ASCII with
// '?': the host logs these and hands them to its virtual controller.
static inline void MlSteamCopyAttributeString(uint8_t* dst, const char* src) {
    size_t i;

    memset(dst, 0, 16);
    for (i = 0; src != NULL && i < 15 && src[i] != '\0'; i++) {
        dst[i] = (src[i] >= 0x20 && src[i] <= 0x7E) ? (uint8_t)src[i] : '?';
    }
}

// Writes the attributes input packet. Returns 0 (and writes nothing) for invalid arguments.
static inline int MlSteamBuildAttributes(uint8_t* p, size_t size, uint8_t controllerNumber,
                                         const LI_STEAM_CONTROLLER_ATTRIBUTES* a) {
    if (p == NULL || size < ML_STEAM_ATTRIBUTES_SIZE || controllerNumber >= 16 || a == NULL) {
        return 0;
    }

    // NV_INPUT_HEADER: big endian size of everything after it, then the magic
    p[0] = p[1] = p[2] = 0;
    p[3] = ML_STEAM_ATTRIBUTES_SIZE - 4;
    MlSteamWrite32(p + 4, ML_STEAM_ATTRIBUTES_MAGIC);
    p[8] = controllerNumber;
    p[9] = a->transport;
    p[10] = a->validMask;
    p[11] = 0;
    MlSteamWrite32(p + 12, a->firmwareBuildTime);
    MlSteamWrite32(p + 16, a->bootloaderBuildTime);
    MlSteamWrite32(p + 20, a->boardRevision);
    MlSteamWrite32(p + 24, a->capabilities);
    MlSteamWrite32(p + 28, a->radioFirmwareBuildTime);
    MlSteamWrite32(p + 32, a->dongleFirmwareBuildTime);
    MlSteamCopyAttributeString(p + 36, a->buildId);
    MlSteamCopyAttributeString(p + 52, a->unitSerial);
    MlSteamCopyAttributeString(p + 68, a->boardSerial);
    return 1;
}
