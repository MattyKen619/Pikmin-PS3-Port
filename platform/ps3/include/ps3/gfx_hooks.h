// PS3 port: -finstrument-functions hooks for the graphics thread's code (the command processor, the
// renderer and the RSX device: Makefile.ps3, GFX_PROFILE), for the sampling profiler (profiler.cpp).
//
// Like the game's hooks (hooks.h), they are compiled into each file so the compiler inlines them. They only
// note where the graphics thread is: the last function it entered or the call site it returned to (self
// time), and the stack pointer there (the profiler walks the stack from it: inclusive time). The profiler
// looks at them while the graphics thread is busy with commands (ps3GfxPhase is not idle). Only the graphics
// thread runs this code once the game is going (start-up and status reports call into it from other threads
// now and then: a sample or two may be off).

#ifndef PS3_GFX_HOOKS_H
#define PS3_GFX_HOOKS_H

#ifdef __cplusplus
extern "C" {
#endif

// (Test build 45: only stores, like the game's hooks, ps3/hooks.h. The profiler walks the graphics thread's
// stack from lastSp.)
struct Ps3GfxHookState {
	void* lastPc; // the function entered last (its descriptor), or the call site returned to
	void* lastSp; // the stack pointer there
};

// (Not volatile: the profiler only reads it, and a sample may be slightly out of date.)
extern struct Ps3GfxHookState ps3GfxHook;

#ifndef PS3_GFX_HOOKS_NO_DEFINITIONS

static inline void* ps3GfxHookSp(void) __attribute__((no_instrument_function, always_inline, unused));
static inline void* ps3GfxHookSp(void)
{
	register void* r1 __asm__("r1");
	void* sp;
	__asm__("mr %0,%1" : "=r"(sp) : "r"(r1));
	return sp;
}

static inline void __cyg_profile_func_enter(void* fn, void* site) __attribute__((no_instrument_function, unused));
static inline void __cyg_profile_func_exit(void* fn, void* site) __attribute__((no_instrument_function, unused));

static inline void __cyg_profile_func_enter(void* fn, void* site)
{
	(void)site;
	ps3GfxHook.lastSp = ps3GfxHookSp();
	ps3GfxHook.lastPc = fn;
}

static inline void __cyg_profile_func_exit(void* fn, void* site)
{
	(void)fn;
	ps3GfxHook.lastSp = ps3GfxHookSp();
	ps3GfxHook.lastPc = site;
}

#endif // PS3_GFX_HOOKS_NO_DEFINITIONS

#ifdef __cplusplus
}
#endif

#endif
