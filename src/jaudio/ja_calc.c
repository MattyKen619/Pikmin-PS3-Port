#include "jaudio/ja_calc.h"

#include <math.h>

#define SINTABLE_LENGTH (257)
static f32 SINTABLE[SINTABLE_LENGTH];

/**
 * @TODO: Documentation
 */
f32 sqrtf2(f32 x)
{
	return std::sqrtf(x);
}

/**
 * @TODO: Documentation
 * @note UNUSED Size: 000020
 */
void cosf2(f32)
{
	TRAP_UNIMPLEMENTED;
}

/**
 * @TODO: Documentation
 */
f32 atanf2(f32 x, f32 y)
{
	return atan2(x, y);
}

/**
 * @TODO: Documentation
 * @note UNUSED Size: 000020
 */
void sinf2(f32)
{
	TRAP_UNIMPLEMENTED;
}

/**
 * @TODO: Documentation
 */
void Jac_InitSinTable()
{
	for (u32 i = 0; i < SINTABLE_LENGTH; i++) {
		SINTABLE[i] = sinf(i * HALF_PI / 256.0f);
	}
}

/**
 * @TODO: Documentation
 */
f32 sinf3(f32 x)
{
#if defined(TARGET_PS3)
	// A NaN argument converts to the index 0x80000000. On the GameCube the 32-bit address arithmetic wraps
	// SINTABLE[0x80000000] back to SINTABLE[0]; on the PS3 the same index points far outside the table and
	// crashes (test 10). Compute the address the GameCube way.
	int index = (int)(256.0f * x);
	return *(f32*)(uptr)((u32)(uptr)SINTABLE + (u32)index * 4u);
#else
	return SINTABLE[(int)(256.0f * x)];
#endif
}
