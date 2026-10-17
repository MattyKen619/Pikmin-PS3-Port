// PS3 port: GameCube texture formats in software: decoding the tiled texture formats (I4 ... CMPR, and the
// color-indexed formats with a palette from texture memory) to 32-bit texels, and a fast content hash for
// noticing when a texture's memory changed.
//
// Shared by the PS3 renderer and the host tools. Portable C++ (no PS3 or game headers).

#ifndef PS3_GPU_TEX_H
#define PS3_GPU_TEX_H

#include "gpu_cp.h"

namespace gpu {

enum TexelOrder {
	TEXELS_RGBA, // 0xRRGGBBAA
	TEXELS_ARGB, // 0xAARRGGBB (the RSX's A8R8G8B8)
};

// Palette entries live in texture memory (TMEM) at byte offset `tlut`, format `tlutFormat` (0 IA8,
// 1 RGB565, 2 RGB5A3). `tmem` is the texture memory image (1 MB), used by the color-indexed formats only.
// Writes width x height texels to out (rows `outPitch` texels apart).
void decodeTexture(const u8* src, u32 width, u32 height, u32 format, const u8* tmem, u32 tlut, u32 tlutFormat, u32* out,
                   u32 outPitch, TexelOrder order);

static inline bool isIndexedFormat(u32 format) { return format == TF_C4 || format == TF_C8 || format == TF_C14X2; }

// Number of palette entries a color-indexed format can use.
static inline u32 paletteEntries(u32 format) { return format == TF_C4 ? 16 : format == TF_C8 ? 256 : 16384; }

// 64-bit content hash (not cryptographic).
u64 hashMemory(const u8* p, u32 size);

} // namespace gpu

#endif
