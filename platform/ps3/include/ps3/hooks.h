// PS3 port: the -finstrument-functions hooks, compiled into each game source file (the Makefile
// force-includes this header into game code; platform/ps3/sdk/os_sched.cpp keeps the state).
//
// Every game function calls __cyg_profile_func_enter on entry and __cyg_profile_func_exit on return. The
// hooks record where the thread is (lastPc, lastSp) for the profiler and stall reports, and are the points
// where queued GameCube interrupts run and threads switch (see "preemption points" in os_sched.cpp).
//
// Up to test build 35 they were functions in os_sched.cpp. The program has several TOCs, so nearly every
// call to them went through a linker stub that saves and restores r2, whose store and reload of the same
// stack slot stall the PPU's pipeline (a load-hit-store) on every call. Defined here, static in each file,
// the compiler inlines them into the functions themselves: no call at all. Test builds up to 35 also
// called a hook at every basic block (-fsanitize-coverage=trace-pc), three times as often; lastPc now
// comes from these hooks: a function's entry, or the call site it returned to.

#ifndef PS3_HOOKS_H
#define PS3_HOOKS_H

#ifdef __cplusplus
extern "C" {
#endif

// Test build 45: the hooks only store, never load. Up to test build 44 they also kept a shadow call stack
// (its depth stored at every entry and exit and loaded back by the next one), counted calls (the same), and
// looked up each function's code address in its descriptor (four halfword loads: GCC could not tell the
// descriptor is aligned): some 28 instructions per call, 100,000 calls a frame, and the loads of what the
// previous hook had just stored stalled the PPU each time (load-hit-store). The profiler and the stall
// reports now walk the thread's stack from lastSp instead (os_sched.cpp, ps3WalkStack).
struct Ps3HookState {
	void* lastPc; // the function entered last (its descriptor), or the call site returned to
	void* lastSp; // the stack pointer there
};

// The baton holder's state. The scheduler keeps each thread's own copy while it does not hold the baton
// and swaps it in and out here (os_sched.cpp: switchHooks). Not volatile: the threads that look at it from
// outside (watchdog, profiler) only read it.
extern struct Ps3HookState ps3Hook;
// Nonzero while GameCube interrupts are queued (set by the threads that post them).
extern volatile unsigned int ps3HookPending;
// Runs the queued interrupts, if they may run now, on the baton holder (pc: where it was).
void ps3HookDeliver(void* pc) __attribute__((no_instrument_function));

#ifndef PS3_HOOKS_NO_DEFINITIONS

static inline void* ps3HookSp(void) __attribute__((no_instrument_function, always_inline, unused));
static inline void* ps3HookSp(void)
{
	// (r1 as an input: the compiler keeps this after the function's prologue sets up its stack frame, so the
	// walk starts at the function's own frame and finds its caller)
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
	ps3Hook.lastSp = ps3HookSp();
	ps3Hook.lastPc = fn;
}

// (Queued interrupts run here, as the function returns: checked on entry, the call that runs them would make
// the compiler set up a stack frame in every function that calls nothing.)
static inline void __cyg_profile_func_exit(void* fn, void* site)
{
	(void)fn;
	ps3Hook.lastSp = ps3HookSp();
	ps3Hook.lastPc = site;
	if (__builtin_expect(ps3HookPending != 0, 0)) {
		ps3HookDeliver(site);
	}
}

#endif // PS3_HOOKS_NO_DEFINITIONS

#ifdef __cplusplus
}
#endif

#endif
