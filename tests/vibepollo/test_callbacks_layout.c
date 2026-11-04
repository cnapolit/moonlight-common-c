// CONNECTION_LISTENER_CALLBACKS only ever grows at the end: every member upstream defines
// keeps its offset, and LiInitializeConnectionCallbacks() zeroes the appended members.
#include "Limelight.h"
#include "test.h"

#include <stddef.h>
#include <string.h>

// Upstream moonlight-common-c's layout, which the extensions must not disturb.
typedef struct {
    ConnListenerStageStarting stageStarting;
    ConnListenerStageComplete stageComplete;
    ConnListenerStageFailed stageFailed;
    ConnListenerConnectionStarted connectionStarted;
    ConnListenerConnectionTerminated connectionTerminated;
    ConnListenerLogMessage logMessage;
    ConnListenerRumble rumble;
    ConnListenerConnectionStatusUpdate connectionStatusUpdate;
    ConnListenerSetHdrMode setHdrMode;
    ConnListenerRumbleTriggers rumbleTriggers;
    ConnListenerSetMotionEventState setMotionEventState;
    ConnListenerSetControllerLED setControllerLED;
    ConnListenerSetAdaptiveTriggers setAdaptiveTriggers;
} UPSTREAM_CALLBACKS;

#define SAME_OFFSET(m) _Static_assert(offsetof(CONNECTION_LISTENER_CALLBACKS, m) == offsetof(UPSTREAM_CALLBACKS, m), #m " moved")
SAME_OFFSET(stageStarting);
SAME_OFFSET(stageComplete);
SAME_OFFSET(stageFailed);
SAME_OFFSET(connectionStarted);
SAME_OFFSET(connectionTerminated);
SAME_OFFSET(logMessage);
SAME_OFFSET(rumble);
SAME_OFFSET(connectionStatusUpdate);
SAME_OFFSET(setHdrMode);
SAME_OFFSET(rumbleTriggers);
SAME_OFFSET(setMotionEventState);
SAME_OFFSET(setControllerLED);
SAME_OFFSET(setAdaptiveTriggers);
_Static_assert(offsetof(CONNECTION_LISTENER_CALLBACKS, controllerHaptics) == sizeof(UPSTREAM_CALLBACKS),
               "extensions are appended after the upstream members");

static void dummyHaptics(uint16_t c, uint32_t s, const uint8_t* p, uint16_t f) { (void)c; (void)s; (void)p; (void)f; }

int main(void) {
    CONNECTION_LISTENER_CALLBACKS cb;

    memset(&cb, 0xA5, sizeof(cb));
    cb.controllerHaptics = dummyHaptics;
    LiInitializeConnectionCallbacks(&cb);
    CHECK(cb.controllerHaptics == NULL);
    CHECK(cb.setAdaptiveTriggers == NULL);

    return TEST_RESULT();
}
