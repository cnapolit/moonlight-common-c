// Parsing of the Vibepollo controller haptics PCM message (0x5601).
#include "ControllerHaptics.h"
#include "test.h"

#include <string.h>

static int calls;
static uint16_t lastController, lastFrames;
static uint32_t lastSequence;
static const uint8_t* lastPcm;

static void onHaptics(uint16_t controllerNumber, uint32_t sequence, const uint8_t* pcm, uint16_t frames) {
    calls++;
    lastController = controllerNumber;
    lastSequence = sequence;
    lastPcm = pcm;
    lastFrames = frames;
}

static size_t build(uint8_t* p, uint16_t controller, uint32_t sequence, uint16_t frames) {
    size_t i, size = ML_HAPTICS_HEADER_SIZE + (size_t)frames * ML_HAPTICS_FRAME_BYTES;
    memset(p, 0, size);
    p[0] = 1;
    p[1] = 1;
    p[2] = (uint8_t)controller; p[3] = (uint8_t)(controller >> 8);
    p[4] = (uint8_t)sequence; p[5] = (uint8_t)(sequence >> 8); p[6] = (uint8_t)(sequence >> 16); p[7] = (uint8_t)(sequence >> 24);
    p[8] = (uint8_t)frames; p[9] = (uint8_t)(frames >> 8);
    for (i = ML_HAPTICS_HEADER_SIZE; i < size; i++) {
        p[i] = (uint8_t)i;
    }
    return size;
}

static int dispatches(const uint8_t* p, size_t size) {
    int before = calls;
    int ret = MlHapticsDispatch(p, size, onHaptics);
    CHECK(ret == (calls - before));
    return ret;
}

int main(void) {
    static uint8_t buf[ML_HAPTICS_HEADER_SIZE + (ML_HAPTICS_MAX_FRAMES + 1) * ML_HAPTICS_FRAME_BYTES + 16];
    size_t size, i;

    // The host's message: 240 frames
    size = build(buf, 3, 0x12345678, ML_HAPTICS_MAX_FRAMES);
    CHECK(dispatches(buf, size));
    CHECK(lastController == 3);
    CHECK(lastSequence == 0x12345678);
    CHECK(lastFrames == ML_HAPTICS_MAX_FRAMES);
    CHECK(lastPcm == buf + ML_HAPTICS_HEADER_SIZE);

    // Without a callback nothing is delivered
    CHECK(MlHapticsDispatch(buf, size, NULL) == 0);
    CHECK(MlHapticsDispatch(NULL, size, onHaptics) == 0);

    // Every truncation, and trailing bytes, are dropped
    for (i = 0; i < size; i++) {
        CHECK(!dispatches(buf, i));
    }
    CHECK(!dispatches(buf, size + 1));

    // A shorter message is fine when its size matches its frame count
    size = build(buf, 15, 1, 1);
    CHECK(dispatches(buf, size));
    CHECK(lastController == 15 && lastFrames == 1);

    // Header fields outside the contract
    size = build(buf, 0, 1, 10); buf[0] = 2; CHECK(!dispatches(buf, size));
    size = build(buf, 0, 1, 10); buf[1] = 0; CHECK(!dispatches(buf, size));
    size = build(buf, 16, 1, 10); CHECK(!dispatches(buf, size));
    size = build(buf, 0, 1, 10); buf[10] = 1; CHECK(!dispatches(buf, size));
    size = build(buf, 0, 1, 0); CHECK(!dispatches(buf, size));
    size = build(buf, 0, 1, ML_HAPTICS_MAX_FRAMES + 1); CHECK(!dispatches(buf, size));

    // A frame count claiming more data than the message holds
    size = build(buf, 0, 1, 10); buf[8] = 11; CHECK(!dispatches(buf, size));

    return TEST_RESULT();
}
