// PS3 port: sinf and cosf for the whole program, in place of newlib's (the linker takes these and leaves
// libm's out). See ps3_trig.h for why; tools/ps3/trig_test checks them against the exact values.

#include "types.h"
#include <math.h>
#include "ps3_trig.h"

namespace {

// Angles the fast path leaves out (|x| >= 1e5, infinities, NaN): newlib's double versions.
double sinFallback(double x)
{
	return sin(x);
}

double cosFallback(double x)
{
	return cos(x);
}

} // namespace

extern "C" {

f32 sinf(f32 x)
{
	return ps3trig::sinf(x, sinFallback);
}

f32 cosf(f32 x)
{
	return ps3trig::cosf(x, cosFallback);
}

} // extern "C"
