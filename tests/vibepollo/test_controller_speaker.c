// Parsing of the Vibepollo controller speaker message (0x5602) and the binding input packet.
#include "ControllerSpeakerPcm.h"
#include "test.h"

#include <string.h>

static int calls;
static struct {
    uint16_t controller, flags, frames;
    uint64_t owner, position;
    uint32_t generation, sequence;
    const uint8_t* pcm;
} last;

static void onSpeaker(uint16_t controllerNumber, uint64_t speakerOwner, uint32_t generation, uint32_t sequence,
                      uint64_t samplePosition, uint16_t flags, const uint8_t* pcm, uint16_t frames) {
    calls++;
    last.controller = controllerNumber;
    last.owner = speakerOwner;
    last.generation = generation;
    last.sequence = sequence;
    last.position = samplePosition;
    last.flags = flags;
    last.pcm = pcm;
    last.frames = frames;
}

static size_t build(uint8_t* p, uint16_t controller, uint32_t generation, uint32_t sequence, uint64_t position,
                    uint16_t frames, uint16_t flags, uint64_t owner) {
    size_t i, size = ML_SPEAKER_PCM_HEADER_SIZE + (size_t)frames * ML_SPEAKER_PCM_FRAME_BYTES;
    memset(p, 0, size);
    p[0] = 2;
    p[1] = 1;
    MlSpeakerWrite16(p + 2, controller);
    MlSpeakerWrite32(p + 4, generation);
    MlSpeakerWrite32(p + 8, sequence);
    MlSpeakerWrite64(p + 12, position);
    MlSpeakerWrite16(p + 20, frames);
    MlSpeakerWrite16(p + 22, flags);
    MlSpeakerWrite64(p + 24, owner);
    for (i = ML_SPEAKER_PCM_HEADER_SIZE; i < size; i++) {
        p[i] = (uint8_t)i;
    }
    return size;
}

static int dispatches(const uint8_t* p, size_t size) {
    int before = calls;
    int ret = MlSpeakerPcmDispatch(p, size, onSpeaker);
    CHECK(ret == (calls - before));
    return ret;
}

int main(void) {
    static uint8_t buf[ML_SPEAKER_PCM_HEADER_SIZE + (ML_SPEAKER_PCM_MAX_FRAMES + 1) * ML_SPEAKER_PCM_FRAME_BYTES + 16];
    uint8_t binding[ML_SPEAKER_BIND_SIZE + 4];
    size_t size, i;

    // The host's message: 240 frames, every field distinct
    size = build(buf, 7, 0x01020304, 0x0A0B0C0D, 0x1122334455667788ull, ML_SPEAKER_PCM_MAX_FRAMES,
                 ML_SPEAKER_PCM_FLAG_DISCONTINUITY, 0x8877665544332211ull);
    CHECK(dispatches(buf, size));
    CHECK(last.controller == 7);
    CHECK(last.generation == 0x01020304);
    CHECK(last.sequence == 0x0A0B0C0D);
    CHECK(last.position == 0x1122334455667788ull);
    CHECK(last.flags == ML_SPEAKER_PCM_FLAG_DISCONTINUITY);
    CHECK(last.owner == 0x8877665544332211ull);
    CHECK(last.frames == ML_SPEAKER_PCM_MAX_FRAMES);
    CHECK(last.pcm == buf + ML_SPEAKER_PCM_HEADER_SIZE);

    CHECK(MlSpeakerPcmDispatch(buf, size, NULL) == 0);
    CHECK(MlSpeakerPcmDispatch(NULL, size, onSpeaker) == 0);

    // Every truncation, and trailing bytes, are dropped
    for (i = 0; i < size; i++) {
        CHECK(!dispatches(buf, i));
    }
    CHECK(!dispatches(buf, size + 1));

    // Fields outside the contract
#define BAD(...) do { size = build(buf, __VA_ARGS__); CHECK(!dispatches(buf, size)); } while (0)
    BAD(16, 1, 1, 0, 10, 0, 1);                     // controller
    BAD(0, 0, 1, 0, 10, 0, 1);                      // zero generation
    BAD(0, 1, 1, 0, 10, 0, 0);                      // zero owner
    BAD(0, 1, 1, 0, 10, 2, 1);                      // unknown flag
    BAD(0, 1, 1, 0, 0, 0, 1);                       // no frames
    BAD(0, 1, 1, 0, ML_SPEAKER_PCM_MAX_FRAMES + 1, 0, 1);
    BAD(0, 1, 1, UINT64_MAX - 5, 10, 0, 1);         // position overflows
    size = build(buf, 0, 1, 1, 0, 10, 0, 1); buf[0] = 1; CHECK(!dispatches(buf, size));
    size = build(buf, 0, 1, 1, 0, 10, 0, 1); buf[1] = 2; CHECK(!dispatches(buf, size));
    size = build(buf, 0, 1, 1, 0, 10, 0, 1); buf[20] = 11; CHECK(!dispatches(buf, size));
    size = build(buf, 0, 1, 1, UINT64_MAX - 10, 10, 0, 1); CHECK(dispatches(buf, size));

    // Binding: NV_INPUT_HEADER (BE size 16, LE magic), controller, version 1, owner
    {
        static const uint8_t expected[ML_SPEAKER_BIND_SIZE] = {
            0, 0, 0, 16, 0x09, 0x00, 0x00, 0x55, 5, 0, 1, 0,
            0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88,
        };
        memset(binding, 0xEE, sizeof(binding));
        CHECK(MlSpeakerPcmBuildBinding(binding, sizeof(binding), 5, 0x8877665544332211ull));
        CHECK(memcmp(binding, expected, sizeof(expected)) == 0);
        CHECK(binding[ML_SPEAKER_BIND_SIZE] == 0xEE);

        CHECK(!MlSpeakerPcmBuildBinding(binding, sizeof(binding), 16, 1));
        CHECK(!MlSpeakerPcmBuildBinding(binding, sizeof(binding), 0, 0));
        CHECK(!MlSpeakerPcmBuildBinding(binding, ML_SPEAKER_BIND_SIZE - 1, 0, 1));
        CHECK(!MlSpeakerPcmBuildBinding(NULL, sizeof(binding), 0, 1));
    }

    return TEST_RESULT();
}
