#pragma once

// Vibepollo protocol extension: controller speaker audio (control message 0x5602).
//
// A Vibepollo host sends the audio a game plays on a PlayStation controller's speaker to a
// client that advertised ML_FF_CONTROLLER_SPEAKER_PCM in x-ml-general.featureFlags and
// announced the controller with LiSendControllerSpeakerArrivalEvent(): a LI_CTYPE_PS arrival
// carrying LI_CCAP_PS_SPEAKER_PCM, preceded on the same reliable channel by a binding input
// packet (magic 0x55000009) that names the client's nonzero owner token for that controller.
// Without the binding the host sends no speaker audio for the controller.
//
// Payload (after the 2-byte control header), all little endian:
//   0  u8  version = 2
//   1  u8  format = 1 (48 kHz, S16LE, stereo)
//   2  u16 controllerNumber (< 16)
//   4  u32 generation (nonzero)
//   8  u32 sequence
//   12 u64 samplePosition (stereo frames within the generation)
//   20 u16 frames (1..ML_SPEAKER_PCM_MAX_FRAMES)
//   22 u16 flags (ML_SPEAKER_PCM_FLAG_*, Limelight.h)
//   24 u64 owner (nonzero, the token from the binding)
//   32 frames * 4 bytes of interleaved samples
//
// Binding input packet (20 bytes): NV_INPUT_HEADER (BE32 size = 16, LE32 magic), u16
// controllerNumber, u16 version = 1, u64 owner.

#include "Limelight.h"

#include <stddef.h>
#include <stdint.h>

#define ML_SPEAKER_PCM_PACKET_TYPE 0x5602
#define ML_SPEAKER_PCM_HEADER_SIZE 32
#define ML_SPEAKER_PCM_MAX_FRAMES 240
#define ML_SPEAKER_PCM_FRAME_BYTES 4

#define ML_SPEAKER_BIND_MAGIC 0x55000009
#define ML_SPEAKER_BIND_SIZE 20

static inline uint16_t MlSpeakerRead16(const uint8_t* p) {
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

static inline uint32_t MlSpeakerRead32(const uint8_t* p) {
    return (uint32_t)MlSpeakerRead16(p) | ((uint32_t)MlSpeakerRead16(p + 2) << 16);
}

static inline uint64_t MlSpeakerRead64(const uint8_t* p) {
    return (uint64_t)MlSpeakerRead32(p) | ((uint64_t)MlSpeakerRead32(p + 4) << 32);
}

static inline void MlSpeakerWrite16(uint8_t* p, uint16_t n) {
    p[0] = (uint8_t)n;
    p[1] = (uint8_t)(n >> 8);
}

static inline void MlSpeakerWrite32(uint8_t* p, uint32_t n) {
    MlSpeakerWrite16(p, (uint16_t)n);
    MlSpeakerWrite16(p + 2, (uint16_t)(n >> 16));
}

static inline void MlSpeakerWrite64(uint8_t* p, uint64_t n) {
    MlSpeakerWrite32(p, (uint32_t)n);
    MlSpeakerWrite32(p + 4, (uint32_t)(n >> 32));
}

// Returns 1 if the payload is a complete, well-formed speaker message. The size must match
// the frame count exactly; anything else is dropped rather than partially played.
static inline int MlSpeakerPcmValidate(const uint8_t* p, size_t size) {
    uint16_t frames;

    if (p == NULL || size < ML_SPEAKER_PCM_HEADER_SIZE || p[0] != 2 || p[1] != 1 ||
            MlSpeakerRead16(p + 2) >= 16 || MlSpeakerRead32(p + 4) == 0 || MlSpeakerRead64(p + 24) == 0 ||
            (MlSpeakerRead16(p + 22) & ~ML_SPEAKER_PCM_FLAG_DISCONTINUITY) != 0) {
        return 0;
    }

    frames = MlSpeakerRead16(p + 20);
    return frames > 0 && frames <= ML_SPEAKER_PCM_MAX_FRAMES &&
           MlSpeakerRead64(p + 12) <= UINT64_MAX - frames &&
           size == ML_SPEAKER_PCM_HEADER_SIZE + (size_t)frames * ML_SPEAKER_PCM_FRAME_BYTES;
}

// Validates the payload and, if it is well formed, invokes the callback with the samples
// borrowed from the payload. Returns 1 if the callback was invoked.
static inline int MlSpeakerPcmDispatch(const uint8_t* p, size_t size, ConnListenerControllerSpeakerPcm callback) {
    if (callback == NULL || !MlSpeakerPcmValidate(p, size)) {
        return 0;
    }

    callback(MlSpeakerRead16(p + 2), MlSpeakerRead64(p + 24), MlSpeakerRead32(p + 4), MlSpeakerRead32(p + 8),
             MlSpeakerRead64(p + 12), MlSpeakerRead16(p + 22), p + ML_SPEAKER_PCM_HEADER_SIZE,
             MlSpeakerRead16(p + 20));
    return 1;
}

// Writes the binding input packet. Returns 0 (and writes nothing) for invalid arguments.
static inline int MlSpeakerPcmBuildBinding(uint8_t* p, size_t size, uint16_t controllerNumber, uint64_t owner) {
    if (p == NULL || size < ML_SPEAKER_BIND_SIZE || controllerNumber >= 16 || owner == 0) {
        return 0;
    }

    // NV_INPUT_HEADER: big endian size of everything after it, then the magic
    p[0] = p[1] = p[2] = 0;
    p[3] = ML_SPEAKER_BIND_SIZE - 4;
    MlSpeakerWrite32(p + 4, ML_SPEAKER_BIND_MAGIC);
    MlSpeakerWrite16(p + 8, controllerNumber);
    MlSpeakerWrite16(p + 10, 1);
    MlSpeakerWrite64(p + 12, owner);
    return 1;
}
