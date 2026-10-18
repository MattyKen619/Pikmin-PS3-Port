// PS3 port: measures what some basic operations cost on the PPU and logs it as a "[bench]" line.

#ifndef PS3_RSX_PPU_BENCH_H
#define PS3_RSX_PPU_BENCH_H

namespace gfx {

// rsxMemory: at least 1 MB of the RSX's memory to time copies with (or NULL).
void ppuBench(void (*log)(const char*), void* rsxMemory, unsigned rsxBytes);

} // namespace gfx

#endif
