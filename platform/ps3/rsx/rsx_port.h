// PS3 port: the RSX renderer as the graphics glue (platform/ps3/gpu/gx_fifo.cpp) sees it. Portable
// declarations only (gx_fifo.cpp is built with the game's headers, the renderer with the PS3 SDK's).

#ifndef PS3_RSX_PORT_H
#define PS3_RSX_PORT_H

#include "../gpu/gpu_cp.h"

namespace ps3rsx {

// Takes the display over from the text console and starts drawing the game's graphics. NULL if the RSX
// could not be set up, or PIKMIN/no_rsx.txt exists on the USB drive (the text console stays).
gpu::Listener* start(gpu::CommandProcessor& cp, gpu::Memory& memory);

// At a copy to the display: writes the frame (the copied rectangle of the embedded frame buffer, as the
// RSX drew it) to PIKMIN/<name>.ppm. For comparing with the frame traces' reference renderings.
void captureFrame(const gpu::CopyInfo& info, const char* name);

// One line for the heartbeat.
void status(char* buf, unsigned int size);

// The game changes sections (a new area is loaded: what is at the vertex cache's display list and vertex
// array addresses may be something else now): the cache starts over at the end of the frame. From any
// thread.
void sectionChanged();

} // namespace ps3rsx

#endif
