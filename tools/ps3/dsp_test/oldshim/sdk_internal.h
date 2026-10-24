// tools/ps3/dsp_test: what an earlier PPU-only dsp_hle.cpp needs from the SDK layer, on the host (OLD=...).
#pragma once
#include <stdint.h>
#include <stddef.h>
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int8_t s8;
typedef int16_t s16;
typedef int32_t s32;
typedef int64_t s64;
typedef uintptr_t uptr;
extern "C" void ps3Log(const char* fmt, ...);
extern "C" void ps3LogOnce(int* flag, const char* fmt, ...);
