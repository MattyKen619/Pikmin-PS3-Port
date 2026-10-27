#ifndef _PS3_SDK_INTERNAL_H
#define _PS3_SDK_INTERNAL_H

// PS3 port: internals shared by the SDK replacement files (compiled with the game's headers).

#include "Dolphin/os.h"
#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif

// ---- logging (TTY + log file + on-screen console)
void ps3Log(const char* fmt, ...);
void ps3LogOnce(int* flag, const char* fmt, ...); // logs only the first time for a given flag

// ---- scheduler (os_sched.cpp)
// The GameCube runs one thread at a time, highest priority first, and the game relies on that.
// PS3 threads are used, but only the holder of the "baton" runs game code.
void ps3SchedInit(void);

// Interrupts: work posted by background threads (video timer, I/O) or by SDK code that must run "later"
// like a GameCube interrupt handler. Delivered at the next scheduling point by the running thread.
typedef void (*Ps3InterruptFn)(void* arg);
void ps3PostInterrupt(Ps3InterruptFn fn, void* arg);
void ps3DeliverInterrupts(void); // call at SDK entry points; cheap when nothing is pending

// Blocking helpers for SDK code (must be called by the running thread, never from an interrupt).
void ps3WaitOn(void* key, const char* reason); // sleep until ps3WakeAll(key)
void ps3WakeAll(void* key);                      // safe from interrupts
int ps3InInterrupt(void);

// Diagnostics
void ps3NoteActivity(const char* what); // remembers the last SDK call, for stall reports
void ps3ProfileSection(const char* name); // a game section starts: report the profile of the last one
void ps3ProfilerStart(void);
unsigned int ps3ProfilerMoves(void); // samples where the game thread had moved on (watchdog)
unsigned int ps3ProfilerTicks(void); // times the profiler has woken up to sample (0: not started)
void ps3SchedExitReport(void);       // every thread's state, when the game quits (profiler.cpp: its exit hook)

// What the running game thread is doing (os_sched.cpp, for the profiler).
#define PS3_SAMPLE_DEPTH 48
typedef struct Ps3SchedSample {
	int thread;  // OS thread index
	int waiting; // the thread holding the baton is waiting (nothing else can run)
	void* pc;    // last game code it ran: a function's descriptor (entered) or a call site (returned to)
	void* sp;    // the stack pointer there
	int depth;
	void* pcs[PS3_SAMPLE_DEPTH]; // return addresses on its stack, innermost first (ps3WalkStack)
} Ps3SchedSample;
int ps3SchedSample(Ps3SchedSample* out);
// Walks a stack from sp, within [lo, hi): fills pcs with up to max return addresses (innermost first).
int ps3WalkStack(const void* sp, unsigned long long lo, unsigned long long hi, void** pcs, int max);

// Use at the top of SDK functions: logs "[trace] first call: NAME" once, then notes the activity.
// The trace shows how far the game got if it stops without an error message.
#define PS3_ACTIVITY(name)                                     \
	do {                                                       \
		static int ps3_seen_;                                  \
		if (!ps3_seen_) {                                      \
			ps3_seen_ = 1;                                     \
			ps3Log("[trace] first call: %s\n", (name));        \
		}                                                      \
		ps3NoteActivity(name);                                 \
	} while (0)
unsigned int ps3FrameCounter(void);

// ---- other subsystems
void ps3DvdInit(void);
void ps3ViStart(void);

#ifdef __cplusplus
}
#endif

#endif // _PS3_SDK_INTERNAL_H
