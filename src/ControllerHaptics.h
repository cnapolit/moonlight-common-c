#pragma once

// Vibepollo protocol extension: controller haptic waveforms (control message 0x5601).
//
// A Vibepollo host sends the PCM it would play on a controller's haptic actuators
// (DualSense voice-coil haptics) to a client that advertised ML_FF_HAPTICS_PCM in
// x-ml-general.featureFlags and LI_CCAP_HAPTICS_PCM on that controller's arrival.
// Stock hosts never send it, and this client never advertises it unless
// CONNECTION_LISTENER_CALLBACKS.controllerHaptics is set.
//
// Payload (after the 2-byte control header), all little endian:
//   u8  version = 1
//   u8  format = 1 (48 kHz, S16LE, stereo: left/right actuator)
//   u16 controllerNumber (< 16)
//   u32 sequence
//   u16 frames (1..ML_HAPTICS_MAX_FRAMES)
//   u16 reserved = 0
//   frames * 4 bytes of interleaved samples

#include "Limelight.h"

#include <stddef.h>
#include <stdint.h>

#define ML_HAPTICS_PACKET_TYPE 0x5601
#define ML_HAPTICS_HEADER_SIZE 12
#define ML_HAPTICS_MAX_FRAMES 240
#define ML_HAPTICS_FRAME_BYTES 4

static inline uint16_t MlHapticsRead16(const uint8_t* p) {
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

static inline uint32_t MlHapticsRead32(const uint8_t* p) {
    return (uint32_t)MlHapticsRead16(p) | ((uint32_t)MlHapticsRead16(p + 2) << 16);
}

// Returns 1 if the payload is a complete, well-formed haptics message. The size must
// match the frame count exactly; anything else is dropped rather than partially played.
static inline int MlHapticsValidate(const uint8_t* p, size_t size) {
    uint16_t frames;

    if (p == NULL || size < ML_HAPTICS_HEADER_SIZE || p[0] != 1 || p[1] != 1 ||
            MlHapticsRead16(p + 2) >= 16 || MlHapticsRead16(p + 10) != 0) {
        return 0;
    }

    frames = MlHapticsRead16(p + 8);
    return frames > 0 && frames <= ML_HAPTICS_MAX_FRAMES &&
           size == ML_HAPTICS_HEADER_SIZE + (size_t)frames * ML_HAPTICS_FRAME_BYTES;
}

// Validates the payload and, if it is well formed, invokes the callback with the samples
// borrowed from the payload. Returns 1 if the callback was invoked.
static inline int MlHapticsDispatch(const uint8_t* p, size_t size, ConnListenerControllerHaptics callback) {
    if (callback == NULL || !MlHapticsValidate(p, size)) {
        return 0;
    }

    callback(MlHapticsRead16(p + 2), MlHapticsRead32(p + 4), p + ML_HAPTICS_HEADER_SIZE, MlHapticsRead16(p + 8));
    return 1;
}
