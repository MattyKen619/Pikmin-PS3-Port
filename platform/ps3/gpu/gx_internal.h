// PS3 port: shared between the GX FIFO glue (gx_fifo.cpp) and the frame trace recorder (gx_trace.cpp).

#ifndef PS3_GX_INTERNAL_H
#define PS3_GX_INTERNAL_H

#include "gpu_cp.h"

namespace ps3gx {

// ---- the command processor and its renderer (gx_fifo.cpp)
gpu::CommandProcessor& commandProcessor();
// Draws and copies are passed on to this listener (the renderer; host tests).
void setRenderer(gpu::Listener* renderer);
// Waits until the graphics thread has interpreted everything written so far (host tests).
void finish();
// Interpret commands on the writing thread, as they are flushed (host tests; call before GXInit).
void forceSynchronous();
// While the processor interprets a batch of FIFO bytes (data[0..size), starting at stream offset
// `base`): that batch. False between batches.
bool currentBatch(const gpu::u8** data, gpu::u32* size, gpu::u64* base);

// A lock for data the game thread and the graphics thread share (short critical sections only).
void spinLock(volatile int* lock);
void spinUnlock(volatile int* lock);

// ---- frame traces (gx_trace.cpp)
//
// A trace is one frame of graphics commands: the command processor's register state at the start of
// the frame (just after the previous frame's copy to the display), then everything it consumed until the
// frame was copied to the display: FIFO bytes, and the memory it read (display lists, vertex arrays,
// textures) as 4 KB pages, each recorded before the command that reads it. Replaying it through the same
// command processor reproduces the frame exactly, off the console.

bool traceRecording();
// After the processor interpreted a batch of FIFO bytes (while the batch is still current).
void traceBatchDone();
// A memory read by the processor: `size` bytes at `address` (as the command gives it) are at `data`, in
// a region of game memory [regionLo, regionHi).
void traceMemory(gpu::u32 address, gpu::u32 size, gpu::MemKind kind, const gpu::u8* data, const gpu::u8* regionLo,
                 const gpu::u8* regionHi);
// Called at every copy to the display (the end of a frame), from inside the processor (so
// cp.commandEnd() is just past the copy). Ends a recording, or starts one that is due.
void traceFrameEnd(gpu::u32 frame, const gpu::CommandProcessor& cp);
// Records a frame `delay` frames from now (once per label). Called by the game's thread.
void traceSchedule(const char* label, gpu::u32 delay);
void traceStatus(char* buf, unsigned int size);
// While a frame is recorded: a name for files that go with it ("NN_label", as in gx_trace_NN_label.bin).
bool traceCurrentName(char* buf, unsigned int size);

} // namespace ps3gx

#endif
