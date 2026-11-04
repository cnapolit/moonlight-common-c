// PyroWave frame assembly: the RTP video queue and the real depacketizer, fed the packets a
// Sunshine host would send (as VideoReceiveThreadProc hands them over after its BE conversions).
//
// The library keeps its stream state in statics (control stream frame counters, depacketizer
// frame numbers), so every case runs in its own process: `test_pyrowave_frames` with no argument
// runs all of them by invoking itself once per case, `test_pyrowave_frames <case>` runs one.
#include "Limelight-internal.h"
#include "RtpVideoQueue.h"
#include "test.h"

#include <stdarg.h>
#include <string.h>

#define PACKET_SIZE   1024                       // NV_VIDEO_PACKET + payload, as negotiated
#define PAYLOAD_SIZE  (PACKET_SIZE - (int)sizeof(NV_VIDEO_PACKET))
#define HEADER_SIZE   8                          // short frame header, 0x01 ...
#define DATA_PACKETS  10
#define CRITICAL      3
#define LAST_PAYLOAD  400                        // payload bytes of the final packet
#define MAX_FRAMES    8
#define MAX_BUFFERS   DATA_PACKETS

// ---- what the decoder saw ----------------------------------------------------------------

typedef struct {
    unsigned int frameNumber;
    int frameType;
    int bufferCount;
    int bufferType[MAX_BUFFERS];
    int bufferLength[MAX_BUFFERS];
    bool allZero[MAX_BUFFERS];
    unsigned char firstByte[MAX_BUFFERS];
    unsigned int critical;
} DELIVERED_FRAME;

static DELIVERED_FRAME delivered[MAX_FRAMES];
static int deliveredCount;

static int onSubmitDecodeUnit(PDECODE_UNIT du) {
    if (deliveredCount < MAX_FRAMES) {
        DELIVERED_FRAME* f = &delivered[deliveredCount];
        memset(f, 0, sizeof(*f));
        f->frameNumber = du->frameNumber;
        f->frameType = du->frameType;
        f->critical = du->pyrowaveCriticalPackets;
        for (PLENTRY e = du->bufferList; e != NULL; e = e->next) {
            if (f->bufferCount < MAX_BUFFERS) {
                int i = f->bufferCount;
                f->bufferType[i] = e->bufferType;
                f->bufferLength[i] = e->length;
                f->firstByte[i] = (unsigned char)e->data[0];
                f->allZero[i] = true;
                for (int b = 0; b < e->length; b++) {
                    if (e->data[b] != 0) {
                        f->allZero[i] = false;
                        break;
                    }
                }
            }
            f->bufferCount++;
        }
    }
    deliveredCount++;
    return DR_OK;
}

static void onLogMessage(const char* format, ...) {
    if (getenv("PYROWAVE_TEST_LOG") != NULL) {
        va_list ap;
        va_start(ap, format);
        vfprintf(stderr, format, ap);
        va_end(ap);
    }
}

// ---- host-side packet construction -------------------------------------------------------

static RTP_VIDEO_QUEUE queue;
static uint16_t nextSeq = 100;            // RTP sequence number of the next frame's first packet
static uint32_t nextStreamPacketIndex = 7;

typedef struct {
    unsigned int frame;
    uint16_t seqBase;
    uint32_t spiBase;
} HOST_FRAME;

static HOST_FRAME beginFrame(unsigned int frame) {
    HOST_FRAME hf = { frame, nextSeq, nextStreamPacketIndex };
    nextSeq = (uint16_t)(nextSeq + DATA_PACKETS);
    nextStreamPacketIndex += DATA_PACKETS;
    return hf;
}

// Byte every payload byte of packet `idx` of `frame` carries (the frame header excepted)
static unsigned char patternByte(unsigned int frame, int idx) {
    return (unsigned char)(0x40 + frame * 16 + idx);
}

static bool isRecordStart(int idx) {
    return idx == 0 || idx == 5;
}

// Builds data packet `idx` and feeds it to the queue like the receive thread does. Returns the
// queue's verdict; a rejected buffer is freed here, a queued one belongs to the queue.
static int sendPacket(HOST_FRAME hf, int idx) {
    const int decryptedSize = PACKET_SIZE + MAX_RTP_HEADER_SIZE;
    const bool last = idx == DATA_PACKETS - 1;
    char* buffer = calloc(1, decryptedSize + sizeof(RTPV_QUEUE_ENTRY));
    CHECK(buffer != NULL);

    PRTP_PACKET rtp = (PRTP_PACKET)buffer;
    rtp->header = FLAG_EXTENSION;
    rtp->packetType = 0;
    rtp->sequenceNumber = (uint16_t)(hf.seqBase + idx);
    rtp->timestamp = hf.frame * 1500;
    rtp->ssrc = 0;

    int dataOffset = sizeof(RTP_PACKET) + 4;
    PNV_VIDEO_PACKET nv = (PNV_VIDEO_PACKET)(buffer + dataOffset);
    nv->streamPacketIndex = LE32((hf.spiBase + idx) << 8);
    nv->frameIndex = LE32(hf.frame);
    nv->flags = FLAG_CONTAINS_PIC_DATA | (idx == 0 ? FLAG_SOF : 0) | (last ? FLAG_EOF : 0);
    nv->extraFlags = isRecordStart(idx) ? NV_VIDEO_PACKET_EXTRA_FLAG_PYROWAVE_RECORD_START : 0;
    nv->multiFecFlags = 0x10;
    nv->multiFecBlocks = 0; // block 0 of 0..0
    nv->fecInfo = LE32(((uint32_t)DATA_PACKETS << 22) | ((uint32_t)idx << 12) | (0u << 4));

    unsigned char* payload = (unsigned char*)(nv + 1);
    int payloadLength = last ? LAST_PAYLOAD : PAYLOAD_SIZE;
    memset(payload, patternByte(hf.frame, idx), payloadLength);
    if (idx == 0) {
        // 8 byte frame header: 0x01, host latency (2), frame type (IDR: intra-only codec),
        // last packet payload length (2), leading critical packets (2), both little-endian
        payload[0] = 0x01;
        payload[1] = 0;
        payload[2] = 0;
        payload[3] = 2;
        payload[4] = LAST_PAYLOAD & 0xFF;
        payload[5] = LAST_PAYLOAD >> 8;
        payload[6] = CRITICAL & 0xFF;
        payload[7] = CRITICAL >> 8;
    }

    int length = dataOffset + (int)sizeof(*nv) + payloadLength;
    int status = RtpvAddPacket(&queue, rtp, length, (PRTPV_QUEUE_ENTRY)&buffer[decryptedSize]);
    if (status != RTPF_RET_QUEUED) {
        free(buffer);
    }
    return status;
}

static void sendAllExcept(HOST_FRAME hf, const int* missing, int missingCount) {
    for (int idx = 0; idx < DATA_PACKETS; idx++) {
        bool skip = false;
        for (int m = 0; m < missingCount; m++) {
            skip |= missing[m] == idx;
        }
        if (!skip) {
            CHECK(sendPacket(hf, idx) == RTPF_RET_QUEUED);
        }
    }
}

static void sendFrame(HOST_FRAME hf) {
    sendAllExcept(hf, NULL, 0);
}

// ---- stream setup, as the connection stages leave it --------------------------------------

static void setupStream(int format) {
    memset(&StreamConfig, 0, sizeof(StreamConfig));
    StreamConfig.packetSize = PACKET_SIZE;
    // Sunshine 7.1.431+: multi-FEC capable, encrypted control stream, short 8 byte frame header
    AppVersionQuad[0] = 7;
    AppVersionQuad[1] = 1;
    AppVersionQuad[2] = 431;
    AppVersionQuad[3] = -1;
    NegotiatedVideoFormat = format;
    ReferenceFrameInvalidationSupported = false;

    LiInitializeConnectionCallbacks(&ListenerCallbacks);
    ListenerCallbacks.logMessage = onLogMessage;
    LiInitializeVideoCallbacks(&VideoCallbacks);
    VideoCallbacks.capabilities = CAPABILITY_DIRECT_SUBMIT;
    VideoCallbacks.submitDecodeUnit = onSubmitDecodeUnit;

    CHECK(initializePlatform() == 0);
    CHECK(initializeControlStream() == 0);
    initializeVideoStream();
    // initializeVideoStream() initialises the module's own queue; the test drives its own
    RtpvInitializeQueue(&queue);
    deliveredCount = 0;
}

static void teardownStream(void) {
    RtpvCleanupQueue(&queue);
    destroyVideoStream();
    // The control stream has no teardown without its threads (and cleanupPlatform() insists
    // on its mutex being gone); the process exits right after the case.
}

// ---- checks -------------------------------------------------------------------------------

// Frame delivered with every packet present (no LOST buffers), PICDATA/RECORD_START as sent
static void checkIntactFrame(const DELIVERED_FRAME* f, unsigned int frame) {
    CHECK(f->frameNumber == frame);
    CHECK(f->frameType == FRAME_TYPE_IDR);
    CHECK(f->critical == CRITICAL);
    CHECK(f->bufferCount == DATA_PACKETS);
    for (int i = 0; i < f->bufferCount && i < MAX_BUFFERS; i++) {
        CHECK(f->bufferType[i] == (isRecordStart(i) ? BUFFER_TYPE_RECORD_START : BUFFER_TYPE_PICDATA));
        int expected = i == DATA_PACKETS - 1 ? LAST_PAYLOAD : (i == 0 ? PAYLOAD_SIZE - HEADER_SIZE : PAYLOAD_SIZE);
        CHECK(f->bufferLength[i] == expected);
        CHECK(f->firstByte[i] == patternByte(frame, i));
    }
}

// Frame delivered with the packets in `lost` replaced by zero-filled BUFFER_TYPE_LOST buffers
static void checkFrameWithLost(const DELIVERED_FRAME* f, unsigned int frame, const int* lost, int lostCount) {
    CHECK(f->frameNumber == frame);
    CHECK(f->critical == CRITICAL);
    CHECK(f->bufferCount == DATA_PACKETS);
    for (int i = 0; i < f->bufferCount && i < MAX_BUFFERS; i++) {
        bool isLost = false;
        for (int l = 0; l < lostCount; l++) {
            isLost |= lost[l] == i;
        }
        int expected = i == DATA_PACKETS - 1 ? LAST_PAYLOAD : PAYLOAD_SIZE;
        if (isLost) {
            CHECK(f->bufferType[i] == BUFFER_TYPE_LOST);
            CHECK(f->bufferLength[i] == expected);
            CHECK(f->allZero[i]);
        }
        else {
            CHECK(f->bufferType[i] == (isRecordStart(i) ? BUFFER_TYPE_RECORD_START : BUFFER_TYPE_PICDATA));
            CHECK(f->bufferLength[i] == (i == 0 ? PAYLOAD_SIZE - HEADER_SIZE : expected));
            CHECK(f->firstByte[i] == patternByte(frame, i));
        }
    }
}

// ---- cases --------------------------------------------------------------------------------

// 1. complete frames are delivered exactly once, on arrival of the final packet
static void caseComplete(void) {
    setupStream(VIDEO_FORMAT_PYROWAVE);

    HOST_FRAME f1 = beginFrame(1);
    for (int idx = 0; idx < DATA_PACKETS - 1; idx++) {
        CHECK(sendPacket(f1, idx) == RTPF_RET_QUEUED);
        CHECK(deliveredCount == 0);
    }
    CHECK(RtpvGetPendingFrameDeadlineUs(&queue) != 0); // armed, but nothing to expire yet
    CHECK(sendPacket(f1, DATA_PACKETS - 1) == RTPF_RET_QUEUED);
    CHECK(deliveredCount == 1);
    CHECK(RtpvGetPendingFrameDeadlineUs(&queue) == 0);
    CHECK(RtpvGetCurrentFrameNumber(&queue) == 2);

    // A duplicate of an already delivered packet is behind the window and rejected
    CHECK(sendPacket(f1, 4) == RTPF_RET_REJECTED);

    HOST_FRAME f2 = beginFrame(2);
    sendFrame(f2);
    CHECK(deliveredCount == 2);

    // Reordered arrival still yields the packets in order
    HOST_FRAME f3 = beginFrame(3);
    static const int order[DATA_PACKETS] = { 0, 2, 1, 3, 4, 6, 5, 9, 7, 8 };
    for (int i = 0; i < DATA_PACKETS; i++) {
        CHECK(sendPacket(f3, order[i]) == RTPF_RET_QUEUED);
    }
    CHECK(deliveredCount == 3);

    CHECK(RtpvExpirePendingFrame(&queue, UINT64_MAX) == false);
    CHECK(deliveredCount == 3);
    if (deliveredCount == 3) {
        checkIntactFrame(&delivered[0], 1);
        checkIntactFrame(&delivered[1], 2);
        checkIntactFrame(&delivered[2], 3);
    }
    teardownStream();
}

// 2. an interior hole is filled when the next frame starts, and that frame still decodes
static void caseInteriorHoleAtBoundary(int hole) {
    setupStream(VIDEO_FORMAT_PYROWAVE);

    HOST_FRAME f1 = beginFrame(1);
    sendFrame(f1);
    CHECK(deliveredCount == 1);

    HOST_FRAME f2 = beginFrame(2);
    sendAllExcept(f2, &hole, 1);
    CHECK(deliveredCount == 1); // held back for reordering

    HOST_FRAME f3 = beginFrame(3);
    CHECK(sendPacket(f3, 0) == RTPF_RET_QUEUED);
    CHECK(deliveredCount == 2); // frame 2 released by the next frame's first packet
    for (int idx = 1; idx < DATA_PACKETS; idx++) {
        CHECK(sendPacket(f3, idx) == RTPF_RET_QUEUED);
    }
    CHECK(deliveredCount == 3);

    if (deliveredCount == 3) {
        checkIntactFrame(&delivered[0], 1);
        checkFrameWithLost(&delivered[1], 2, &hole, 1);
        checkIntactFrame(&delivered[2], 3);
    }
    teardownStream();
}

// 3. a frame without its first packet is dropped; the stream resumes with the next frame
static void caseFirstPacketMissing(void) {
    setupStream(VIDEO_FORMAT_PYROWAVE);

    HOST_FRAME f1 = beginFrame(1);
    sendFrame(f1);
    CHECK(deliveredCount == 1);

    int missing = 0;
    HOST_FRAME f2 = beginFrame(2);
    sendAllExcept(f2, &missing, 1);
    CHECK(deliveredCount == 1);
    // Everything else arrived, including the final packet; expiry must not invent frame 2
    uint64_t deadline = RtpvGetPendingFrameDeadlineUs(&queue);
    CHECK(RtpvExpirePendingFrame(&queue, deadline + 1) == false);
    CHECK(deliveredCount == 1);

    HOST_FRAME f3 = beginFrame(3);
    sendFrame(f3);
    CHECK(deliveredCount == 2);
    if (deliveredCount == 2) {
        checkIntactFrame(&delivered[0], 1);
        checkIntactFrame(&delivered[1], 3);
    }
    teardownStream();
}

// 4. deadline delivery: holes after the critical prefix are released on expiry, holes in it are not
static void caseExpire(const int* missing, int missingCount, bool expectDelivered, bool boundaryDelivers) {
    setupStream(VIDEO_FORMAT_PYROWAVE);

    HOST_FRAME f1 = beginFrame(1);
    sendFrame(f1);
    CHECK(deliveredCount == 1);

    HOST_FRAME f2 = beginFrame(2);
    sendAllExcept(f2, missing, missingCount);
    CHECK(deliveredCount == 1);

    uint64_t deadline = RtpvGetPendingFrameDeadlineUs(&queue);
    CHECK(deadline != 0);
    CHECK(RtpvExpirePendingFrame(&queue, deadline - 1) == false); // too early
    CHECK(deliveredCount == 1);
    CHECK(RtpvGetPendingFrameDeadlineUs(&queue) == deadline);

    CHECK(RtpvExpirePendingFrame(&queue, deadline) == expectDelivered);
    CHECK(RtpvGetPendingFrameDeadlineUs(&queue) == 0);
    CHECK(deliveredCount == (expectDelivered ? 2 : 1));
    CHECK(RtpvGetCurrentFrameNumber(&queue) == (expectDelivered ? 3u : 2u));
    CHECK(RtpvExpirePendingFrame(&queue, UINT64_MAX) == false); // nothing left to expire

    if (expectDelivered && deliveredCount == 2) {
        checkFrameWithLost(&delivered[1], 2, missing, missingCount);
    }

    HOST_FRAME f3 = beginFrame(3);
    sendFrame(f3);
    int expectedTotal = (expectDelivered || boundaryDelivers) ? 3 : 2;
    CHECK(deliveredCount == expectedTotal);
    if (!expectDelivered && boundaryDelivers && deliveredCount == 3) {
        // Not released by the deadline, but the next frame's arrival still flushes it
        checkFrameWithLost(&delivered[1], 2, missing, missingCount);
    }
    if (deliveredCount > 0 && deliveredCount <= MAX_FRAMES) {
        checkIntactFrame(&delivered[deliveredCount - 1], 3);
    }
    teardownStream();
}

// 5. other codecs never get the PyroWave treatment
static void caseH265Inert(void) {
    setupStream(VIDEO_FORMAT_H265);

    int missing = 5;
    HOST_FRAME f1 = beginFrame(1);
    sendAllExcept(f1, &missing, 1);
    CHECK(RtpvGetPendingFrameDeadlineUs(&queue) == 0); // no silence deadline armed
    CHECK(RtpvPendingFrameDeadlineIsPrecise(&queue) == false);
    CHECK(RtpvExpirePendingFrame(&queue, UINT64_MAX) == false);
    CHECK(deliveredCount == 0);

    HOST_FRAME f2 = beginFrame(2);
    CHECK(sendPacket(f2, 0) == RTPF_RET_QUEUED); // frame boundary: frame 1 is abandoned
    CHECK(deliveredCount == 0);
    CHECK(RtpvGetCurrentFrameNumber(&queue) == 2);
    teardownStream();
}

// Format masks: the PyroWave formats are their own family, with the right depth and chroma
static void caseFormatClassification(void) {
    static const struct { int format; bool tenBit; bool yuv444; } formats[] = {
        { VIDEO_FORMAT_PYROWAVE,           false, false },
        { VIDEO_FORMAT_PYROWAVE_444,       false, true  },
        { VIDEO_FORMAT_PYROWAVE_HDR10,     true,  false },
        { VIDEO_FORMAT_PYROWAVE_HDR10_444, true,  true  },
    };
    static const struct { unsigned int scm; bool tenBit; bool yuv444; } modes[] = {
        { SCM_PYROWAVE,           false, false },
        { SCM_PYROWAVE_444,       false, true  },
        { SCM_PYROWAVE_HDR10,     true,  false },
        { SCM_PYROWAVE_HDR10_444, true,  true  },
    };
    int allFormats = 0;
    unsigned int allModes = 0;

    for (size_t i = 0; i < sizeof(formats) / sizeof(formats[0]); i++) {
        int f = formats[i].format;
        allFormats |= f;
        CHECK((f & VIDEO_FORMAT_MASK_PYROWAVE) == f);
        CHECK((f & (VIDEO_FORMAT_MASK_H264 | VIDEO_FORMAT_MASK_H265 | VIDEO_FORMAT_MASK_AV1)) == 0);
        CHECK(((f & VIDEO_FORMAT_MASK_10BIT) != 0) == formats[i].tenBit);
        CHECK(((f & VIDEO_FORMAT_MASK_YUV444) != 0) == formats[i].yuv444);
        // Single, distinct bits
        CHECK((f & (f - 1)) == 0);
    }
    CHECK(allFormats == VIDEO_FORMAT_MASK_PYROWAVE);

    for (size_t i = 0; i < sizeof(modes) / sizeof(modes[0]); i++) {
        unsigned int m = modes[i].scm;
        allModes |= m;
        CHECK((m & SCM_MASK_PYROWAVE) == m);
        CHECK((m & (SCM_MASK_H264 | SCM_MASK_HEVC | SCM_MASK_AV1)) == 0);
        CHECK(((m & SCM_MASK_10BIT) != 0) == modes[i].tenBit);
        CHECK(((m & SCM_MASK_YUV444) != 0) == modes[i].yuv444);
        CHECK((m & (m - 1)) == 0);
    }
    CHECK(allModes == SCM_MASK_PYROWAVE);

    // The non-PyroWave families must not leak into the PyroWave masks
    CHECK((VIDEO_FORMAT_H264 & VIDEO_FORMAT_MASK_PYROWAVE) == 0);
    CHECK((VIDEO_FORMAT_H265_MAIN10 & VIDEO_FORMAT_MASK_PYROWAVE) == 0);
    CHECK((VIDEO_FORMAT_AV1_HIGH10_444 & VIDEO_FORMAT_MASK_PYROWAVE) == 0);
    CHECK((SCM_HEVC_MAIN10 & SCM_MASK_PYROWAVE) == 0);
    CHECK((SCM_AV1_HIGH10_444 & SCM_MASK_PYROWAVE) == 0);
}

typedef struct {
    const char* name;
    void (*run)(void);
} CASE;

static void caseHoleMiddle(void)     { caseInteriorHoleAtBoundary(5); }
static void caseHoleAfterCrit(void)  { caseInteriorHoleAtBoundary(CRITICAL); }
static void caseTailAtBoundary(void) { caseInteriorHoleAtBoundary(DATA_PACKETS - 1); }

static void caseExpireInterior(void) {
    int missing[] = { 6 };
    caseExpire(missing, 1, true, true);
}
static void caseExpireFirstAfterCritical(void) {
    int missing[] = { CRITICAL };
    caseExpire(missing, 1, true, true);
}
static void caseExpireTailLost(void) {
    // Final data packet absent: the longer tail silence applies, the tail is zero-filled
    int missing[] = { 8, DATA_PACKETS - 1 };
    caseExpire(missing, 2, true, true);
}
static void caseExpireCriticalHole(void) {
    // Not released by the deadline; the next frame's arrival still flushes it with the hole filled
    int missing[] = { 1 };
    caseExpire(missing, 1, false, true);
}
static void caseExpireLastCriticalHole(void) {
    int missing[] = { CRITICAL - 1 };
    caseExpire(missing, 1, false, true);
}

static const CASE cases[] = {
    { "complete", caseComplete },
    { "hole_middle_at_boundary", caseHoleMiddle },
    { "hole_after_critical_at_boundary", caseHoleAfterCrit },
    { "tail_lost_at_boundary", caseTailAtBoundary },
    { "first_packet_missing", caseFirstPacketMissing },
    { "expire_interior_hole", caseExpireInterior },
    { "expire_hole_right_after_critical", caseExpireFirstAfterCritical },
    { "expire_tail_lost", caseExpireTailLost },
    { "expire_critical_hole", caseExpireCriticalHole },
    { "expire_last_critical_hole", caseExpireLastCriticalHole },
    { "h265_inert", caseH265Inert },
    { "format_classification", caseFormatClassification },
};

int main(int argc, char** argv) {
    const size_t count = sizeof(cases) / sizeof(cases[0]);

    if (argc == 2) {
        for (size_t i = 0; i < count; i++) {
            if (strcmp(argv[1], cases[i].name) == 0) {
                cases[i].run();
                if (testFailures != 0) {
                    fprintf(stderr, "case %s failed\n", cases[i].name);
                }
                return TEST_RESULT();
            }
        }
        fprintf(stderr, "unknown case %s\n", argv[1]);
        return 2;
    }

    int failed = 0;
    for (size_t i = 0; i < count; i++) {
        char command[1024];
        snprintf(command, sizeof(command), "\"%s\" %s", argv[0], cases[i].name);
        printf("case %s: ", cases[i].name);
        fflush(stdout);
        if (system(command) != 0) {
            failed++;
        }
    }
    if (failed != 0) {
        fprintf(stderr, "%d cases failed\n", failed);
        return 1;
    }
    printf("OK\n");
    return 0;
}
