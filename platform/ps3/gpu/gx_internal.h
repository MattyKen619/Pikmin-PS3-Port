// PS3 port: shared between the GX FIFO glue (gx_fifo.cpp) and the frame trace recorder (gx_trace.cpp).

#ifndef PS3_GX_INTERNAL_H
#define PS3_GX_INTERNAL_H

#include "gpu_cp.h"

namespace ps3gx {

// ---- the command processor and its renderer (gx_fifo.cpp)
gpu::CommandProcessor& commandProcessor();
// Draws and copies are passed on to this listener (the renderer; host tests).
void setRenderer(gpu::Listener* renderer);

// ---- frame traces (gx_trace.cpp)
//
// A trace is one frame of graphics commands: the command processor's register state at the start of
// the frame, then everything it consumed until the frame was copied to the display: FIFO bytes, and
// every block of memory it read (display lists, vertex arrays, textures), in order. Replaying it
// through the same command processor reproduces the frame exactly, off the console.

bool traceRecording();
void traceFifo(const gpu::u8* bytes, gpu::u32 n);
void traceMemory(gpu::u32 address, gpu::u32 size, gpu::MemKind kind, const gpu::u8* data);
// Called at every copy to the display (the end of a frame). Ends recordings, and arms the next one,
// which starts at the next batch of commands (traceAtFlush, before the batch is interpreted).
void traceFrameEnd(gpu::u32 frame, const gpu::CommandProcessor& cp);
void traceAtFlush(const gpu::CommandProcessor& cp);
// Records a frame `delay` frames from now (once per label).
void traceSchedule(const char* label, gpu::u32 delay);
void traceStatus(char* buf, unsigned int size);

} // namespace ps3gx

#endif
