// PS3 port: the few GX calls that mark the end of a frame. Graphics are not translated yet (Phase 4);
// these count the frames the game draws (shown in the log's heartbeat) and act as thread-switch points.

#include "sdk_internal.h"

#include "Dolphin/gx.h"

static volatile u32 sFramesDrawn;

extern "C" unsigned int ps3FramesDrawn(void) { return sFramesDrawn; }

void GXCopyDisp(void* dest, GXBool doClear)
{
	(void)dest;
	(void)doClear;
	PS3_ACTIVITY("GXCopyDisp");
	sFramesDrawn++;
	ps3DeliverInterrupts();
}

void GXDrawDone(void)
{
	PS3_ACTIVITY("GXDrawDone");
	ps3DeliverInterrupts();
}

void GXFlush(void) { ps3DeliverInterrupts(); }
