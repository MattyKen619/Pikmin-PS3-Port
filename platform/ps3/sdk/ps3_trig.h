// PS3 port: sinf and cosf for the PPU (math_ps3.cpp, test build 47; tools/ps3/trig_test checks them).
//
// newlib's sinf and cosf convert between floats and integers to reduce the angle (its quadrant as an
// integer, and back): on the PPU each conversion goes through memory and stalls the pipeline (a load that
// hits the store just made), three per call, and the game calls them six times for every joint of every
// model it animates (Matrix4f::makeSRT, makeConcatSRT). These stay in floating point: the angle is reduced
// in double precision, the quadrant found with the round-to-nearest trick (adding and subtracting 1.5 * 2^52),
// and the polynomials are FreeBSD's for float results (k_sinf.c, k_cosf.c: under 1 ulp).
//
// The includer defines nothing first; it needs sin and cos for angles the reduction does not cover.

#ifndef PS3_TRIG_H
#define PS3_TRIG_H

namespace ps3trig {

static const double kRound      = 6755399441055744.0;      // 1.5 * 2^52: x + kRound - kRound rounds x to an integer
static const double kTwoOverPi  = 6.36619772367581382433e-01;
static const double kPio2Hi     = 1.57079631090164184570e+00; // pi/2, first 33 bits
static const double kPio2Lo     = 1.58932547735281966916e-08; // pi/2 - kPio2Hi
static const double kLargest    = 1.0e5;                       // larger angles (and NaN, infinities): sin, cos
static const double kTiny       = 1.0 / 4096.0;                // smaller ones: sin x = x

// FreeBSD k_sinf.c and k_cosf.c (|x| <= pi/4)
static inline double sinPoly(double x)
{
	const double S1 = -0x15555554cbac77.0p-55, S2 = 0x111110896efbb2.0p-59, S3 = -0x1a00f9e2cae774.0p-65, S4 = 0x16cd878c3b46a7.0p-71;
	const double z = x * x, w = z * z, r = S3 + z * S4, s = z * x;
	return (x + s * (S1 + z * S2)) + s * w * r;
}

static inline double cosPoly(double x)
{
	const double C0 = -0x1ffffffd0c5e81.0p-54, C1 = 0x155553e1053a42.0p-57, C2 = -0x16c087e80f1e27.0p-62, C3 = 0x199342e0ee5069.0p-68;
	const double z = x * x, w = z * z, r = C2 + z * C3;
	return ((1.0 + z * C0) + w * C1) + (w * z) * r;
}

// x = r + q * pi/2, |r| <= pi/4 (and a little), q in -2..2 (as a double: no integer conversion)
static inline double reduce(double x, double* q)
{
	const double k = (x * kTwoOverPi + kRound) - kRound;
	*q             = k - 4.0 * ((k * 0.25 + kRound) - kRound);
	return (x - k * kPio2Hi) - k * kPio2Lo;
}

template <typename Fallback>
static inline float sinf(float xf, Fallback fallback)
{
	const double x = xf;
	if (!(x < kLargest && x > -kLargest)) {
		return (float)fallback(x);
	}
	if (x < kTiny && x > -kTiny) {
		return xf; // (sin x = x in float precision; keeps the sign of zero)
	}
	double q;
	const double r = reduce(x, &q);
	double v;
	if (q == 0.0) {
		v = sinPoly(r);
	} else if (q == 1.0) {
		v = cosPoly(r);
	} else if (q == -1.0) {
		v = -cosPoly(r);
	} else {
		v = -sinPoly(r);
	}
	return (float)v;
}

template <typename Fallback>
static inline float cosf(float xf, Fallback fallback)
{
	const double x = xf;
	if (!(x < kLargest && x > -kLargest)) {
		return (float)fallback(x);
	}
	double q;
	const double r = reduce(x, &q);
	double v;
	if (q == 0.0) {
		v = cosPoly(r);
	} else if (q == 1.0) {
		v = -sinPoly(r);
	} else if (q == -1.0) {
		v = sinPoly(r);
	} else {
		v = -cosPoly(r);
	}
	return (float)v;
}

} // namespace ps3trig

#endif
