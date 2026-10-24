// PS3 port: the SPU sound mixer's thread (test build 44). See platform/ps3/spu/dsp_spu.cpp.
//
// One SPU thread in a thread group of its own runs the mixer program, which is built into the ELF
// (build/ps3/gen/dsp_spu_elf.h). The PPU wakes it by writing its signal notification register 1; after each
// sub-frame it sends an event to an event queue, and a PPU thread waiting on that queue calls back into the
// SDK layer (audio.cpp), which raises the DSP's interrupt for the game.

#include "ps3_backend.h"

#include <ppu-types.h>
#include <string.h>

#include <sys/event_queue.h>
#include <sys/spu.h>

#include "dsp_spu_elf.h"

namespace {

enum {
	EVENT_PORT  = 2,   // (PS3_DSP_EVENT_PORT in ps3/dsp_shared.h)
	GROUP_PRIO  = 100, // SPU thread group priority (16-255, lower first); nothing else of ours uses SPUs
	EVENT_SLOTS = 32,
};

sysSpuImage sImage;
sys_spu_group_t sGroup;
sys_spu_thread_t sThread;
sys_event_queue_t sQueue;
void (*volatile sOnDone)(void);
volatile int sRunning;

void eventThread(void*)
{
	for (;;) {
		sys_event_t ev;
		if (sysEventQueueReceive(sQueue, &ev, 0) != 0) {
			break; // (the queue was destroyed)
		}
		void (*done)(void) = sOnDone;
		if (done) {
			done();
		}
	}
	ps3b_thread_exit();
}

} // namespace

// Starts the mixer program with the shared block at `shared`; `onDone` is called (on a PPU thread of its own)
// after each sub-frame it finishes. 0, or an lv2 error code with *failedStep naming the call that failed.
extern "C" int ps3b_dsp_spu_start(unsigned int shared, void (*onDone)(void), const char** failedStep)
{
	const char* step = "";
	int err          = 0;
#define TRY(call)                                                                                                                           \
	do {                                                                                                                                    \
		step = #call;                                                                                                                       \
		err  = (int)(call);                                                                                                                 \
		if (err != 0) {                                                                                                                     \
			goto failed;                                                                                                                    \
		}                                                                                                                                   \
	} while (0)
	{
		sOnDone = onDone;
		sysSpuInitialize(6, 0); // (may report that it was done already)
		TRY(sysSpuImageImport(&sImage, ps3_dsp_spu_elf, SPU_IMAGE_PROTECT));

		static const char kGroupName[]  = "pikmin_dsp";
		static const char kThreadName[] = "pikmin_dsp_spu";
		sysSpuThreadGroupAttribute group;
		memset(&group, 0, sizeof(group));
		sysSpuThreadGroupAttributeInitialize(group);
		sysSpuThreadGroupAttributeName(group, kGroupName);
		TRY(sysSpuThreadGroupCreate(&sGroup, 1, GROUP_PRIO, &group));

		sysSpuThreadAttribute attr;
		memset(&attr, 0, sizeof(attr));
		sysSpuThreadAttributeInitialize(attr);
		sysSpuThreadAttributeName(attr, kThreadName);
		sysSpuThreadAttributeOption(attr, SPU_THREAD_ATTR_DEC_SYNC_TB_ENABLE);
		sysSpuThreadArgument args;
		sysSpuThreadArgumentInitialize(args);
		args.arg0 = shared;
		TRY(sysSpuThreadInitialize(&sThread, sGroup, 0, &sImage, &attr, &args));
		TRY(sysSpuThreadSetConfiguration(sThread, SPU_SIGNAL1_OVERWRITE | SPU_SIGNAL2_OVERWRITE));

		sys_event_queue_attr_t qattr;
		memset(&qattr, 0, sizeof(qattr));
		qattr.attr_protocol = SYS_EVENT_QUEUE_PRIO;
		qattr.type          = SYS_EVENT_QUEUE_PPU;
		memcpy(qattr.name, "pkdsp", 6);
		TRY(sysEventQueueCreate(&sQueue, &qattr, SYS_EVENT_QUEUE_KEY_LOCAL, EVENT_SLOTS));
		TRY(sysSpuThreadConnectEvent(sThread, sQueue, SPU_THREAD_EVENT_USER, EVENT_PORT));
		step = "the event thread";
		err  = ps3b_thread_create(eventThread, NULL, 900, 0x8000, "pikmin_dsp_done");
		if (err != 0) {
			goto failed;
		}
		TRY(sysSpuThreadGroupStart(sGroup));
		sRunning = 1;
		return 0;
	}
#undef TRY
failed:
	if (failedStep) {
		*failedStep = step;
	}
	return err ? err : -1;
}

// Wakes the mixer program (it then mixes every sub-frame asked for in the shared block).
extern "C" void ps3b_dsp_spu_wake(void)
{
	if (sRunning) {
		sysSpuThreadWriteSignal(sThread, 0, 1);
	}
}

// Stops the mixer program for good (it stopped answering: the PPU mixes instead). Returns the lv2 result.
extern "C" int ps3b_dsp_spu_stop(void)
{
	if (!sRunning) {
		return 0;
	}
	sRunning = 0;
	sOnDone  = 0;
	return sysSpuThreadGroupTerminate(sGroup, 0);
}
