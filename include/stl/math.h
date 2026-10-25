#ifndef _STL_MATH_H
#define _STL_MATH_H

#include "types.h"

BEGIN_SCOPE_EXTERN_C

#define signbit(x)    ((int)(__HI(x) & 0x80000000))
#define ispositive(x) ((((u8*)&x)[0] & 0x80) != 0)

#define INFINITY (*(f32*)__float_huge)
#define NAN      (*(f32*)__float_nan)
#define HUGE_VAL (*(f64*)__double_huge)
#define DBL_NAN  (*(f64*)__double_nan)

#define FP_NAN       1
#define FP_INFINITE  2
#define FP_ZERO      3
#define FP_NORMAL    4
#define FP_SUBNORMAL 5

extern const f32 __float_huge[];
extern const f32 __float_nan[];
extern const f64 __double_huge[];
extern const f64 __double_nan[];

f64 cos(f64);
f32 cosf(f32);
f64 sin(f64);
f32 sinf(f32);
f64 tan(f64);
f32 tanf(f32);

f64 acos(f64);
f32 acosf(f32);
f64 asin(f64);
f64 atan(f64);
f32 atanf(f32);
f64 atan2(f64, f64);
f32 atan2f(f32, f32);

f64 ceil(f64);
f64 floor(f64);
f64 frexp(f64, int*);
f64 ldexp(f64, int);
f64 sqrt(f64);

f64 pow(f64, f64);
f64 log(f64);
f64 log10(f64);
f64 fmod(f64, f64);
f64 scalbn(f64, int);

// C99 `inline` (non-CodeWarrior C builds, e.g. the PS3 port's mtx/*.c) would emit an external
// definition in every C file that includes this header, so those builds use `static inline`.
#if !defined(__MWERKS__) && !defined(__cplusplus)
#define MSL_MATH_INLINE static inline
#else
#define MSL_MATH_INLINE inline
#endif

#define fabs(x)  __fabs(x)
#define fabsf(x) __fabsf(x)

MSL_MATH_INLINE f128 fabsl(f128 x)
{
	return __fabs((f64)x);
}

#if defined(TARGET_PS3) && defined(__powerpc64__) // (not the port's host tools)
// PS3 port: one instruction (the PPU has fsqrts, correctly rounded). The original's three refinement steps
// are a dozen dependent multiplies, and its volatile result is stored and loaded back at once (a stall).
MSL_MATH_INLINE f32 sqrtf(f32 x)
{
	if (x > 0.0f) {
		f32 y;
		__asm__("fsqrts %0,%1" : "=f"(y) : "f"(x));
		return y;
	}
	return x;
}
#else
MSL_MATH_INLINE f32 sqrtf(f32 x)
{
	// these REALLY don't have to be static.
	static const f64 _half  = .5;
	static const f64 _three = 3.0;

	vf32 y;
	if (x > 0.0f) {

		f64 guess = __frsqrte((f64)x);                            // returns an approximation to
		guess     = _half * guess * (_three - guess * guess * x); // now have 12 sig bits
		guess     = _half * guess * (_three - guess * guess * x); // now have 24 sig bits
		guess     = _half * guess * (_three - guess * guess * x); // now have 32 sig bits
		y         = (f32)(x * guess);
		return y;
	}
	return x;
}
#endif

END_SCOPE_EXTERN_C

#ifdef __cplusplus
namespace std {

#if defined(TARGET_PS3) && defined(__powerpc64__) // (not the port's host tools)
// PS3 port: one instruction (the PPU has fsqrts, correctly rounded). The original's three refinement steps
// are a dozen dependent multiplies, and its volatile result is stored and loaded back at once (a stall).
MSL_MATH_INLINE f32 sqrtf(f32 x)
{
	if (x > 0.0f) {
		f32 y;
		__asm__("fsqrts %0,%1" : "=f"(y) : "f"(x));
		return y;
	}
	return x;
}
#else
MSL_MATH_INLINE f32 sqrtf(f32 x)
{
	// these REALLY don't have to be static.
	static const f64 _half  = 0.5;
	static const f64 _three = 3.0;

	vf32 y;
	if (x > 0.0f) {

		f64 guess = __frsqrte((f64)x);                            // returns an approximation to
		guess     = _half * guess * (_three - guess * guess * x); // now have 12 sig bits
		guess     = _half * guess * (_three - guess * guess * x); // now have 24 sig bits
		guess     = _half * guess * (_three - guess * guess * x); // now have 32 sig bits
		y         = (f32)(x * guess);
		return y;
	}
	return x;
}
#endif

MSL_MATH_INLINE f32 fmodf(f32 x, f32 m)
{
	f32 b, a;
	a = fabsf(m);
	b = fabsf(x);
	if (a > b) {
		return x;
	}

	long long c = (long long)(x / m);
	return x - m * c;
}
} // namespace std

MSL_MATH_INLINE f32 fmod(f32 x, f32 m)
{
	return std::fmodf(x, m);
}
#endif

#endif
