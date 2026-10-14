// PS3 port: sound output through the PS3's audio server (libaudio, module "cellAudio").
//
// A backend thread opens a 2-channel port of 16 blocks (256 frames each, 48 kHz, 32-bit float) and keeps
// a few blocks queued ahead of the block the audio server is playing. It gets the samples from a pull
// function (the SDK layer's ring of resampled GameCube audio, sdk/audio_null.cpp).
//
// The audio library lives in a module that is not loaded when the process starts, and calling it before
// that crashes the calling thread (test build 1 crashed that way on the file-system library). So
// everything happens on this thread, in steps that are each logged: if one fails, the game goes on
// silently and the log shows which step.

#include "ps3_backend.h"

#include <ppu-types.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include <audio/audio.h>
#include <sys/event_queue.h>
#include <sysmodule/sysmodule.h>
// PSL1GHT's sys/prx.h (types) and lv2/prx.h (functions) share one include guard, so whichever comes
// second is skipped. Include the types, then the functions.
#include <sys/prx.h>
#undef __LV2_PRX_H__
#include <lv2/prx.h>

static void logf(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
static void logf(const char* fmt, ...)
{
	char buf[256];
	va_list args;
	va_start(args, fmt);
	vsnprintf(buf, sizeof(buf), fmt, args);
	va_end(args);
	ps3b_log_write(buf);
}

// ---------------------------------------------------------------- loaded system modules

typedef void (*ModuleVisitor)(const char* name, void* ctx);

static int forEachModule(ModuleVisitor visit, void* ctx)
{
	static sysPrxId ids[128];
	static u32 levels[128];
	sysPrxModuleList list;
	memset(&list, 0, sizeof(list));
	list.size      = sizeof(list);
	list.max       = 128;
	list.idlist    = (u32)(u64)ids;
	list.levellist = (u32)(u64)levels;
	if (sysPrxGetModuleList(0, &list) != 0) {
		return -1;
	}
	for (u32 i = 0; i < list.count && i < 128; i++) {
		static char filename[SYS_PRX_MODULE_FILENAME_SIZE];
		static sysPrxSegmentInfo segments[16];
		sysPrxModuleInfo info;
		memset(&info, 0, sizeof(info));
		info.size          = sizeof(info);
		info.filename      = (u32)(u64)filename;
		info.filename_size = sizeof(filename);
		info.segments      = (u32)(u64)segments;
		info.segments_num  = 16;
		if (sysPrxGetModuleInfo(ids[i], 0, &info) == 0) {
			char name[sizeof(info.name) + 1];
			memcpy(name, info.name, sizeof(info.name));
			name[sizeof(info.name)] = 0;
			visit(name, ctx);
		}
	}
	return (int)list.count;
}

struct ListCtx {
	char text[900];
	int n;
};

static void appendName(const char* name, void* p)
{
	ListCtx* c = (ListCtx*)p;
	c->n += snprintf(c->text + c->n, sizeof(c->text) - c->n, "%s%s", c->n ? ", " : "", name);
	if (c->n >= (int)sizeof(c->text)) {
		c->n = sizeof(c->text) - 1;
	}
}

extern "C" void ps3b_log_modules(void)
{
	ListCtx c;
	c.text[0] = 0;
	c.n       = 0;
	int count = forEachModule(appendName, &c);
	if (count < 0) {
		logf("[ps3] system modules: could not list them\n");
	} else {
		logf("[ps3] system modules loaded (%d): %s\n", count, c.text);
	}
}

struct FindCtx {
	const char* needle;
	int found;
};

static void findName(const char* name, void* p)
{
	FindCtx* c = (FindCtx*)p;
	if (strstr(name, c->needle)) {
		c->found = 1;
	}
}

static int moduleLoaded(const char* needle)
{
	FindCtx c = { needle, 0 };
	forEachModule(findName, &c);
	return c.found;
}

// ---------------------------------------------------------------- output thread

enum { AHEAD = 4 }; // blocks queued after the one being played (about 21 ms)

static ps3b_audio_pull_fn sPull;
static volatile int sStop;
static volatile int sPlaying;
static u32 sPort;

static void fillBlock(float* dst, u32 channels)
{
	unsigned int got = sPull ? sPull(dst, AUDIO_BLOCK_SAMPLES) : 0;
	if (got < AUDIO_BLOCK_SAMPLES) {
		memset(dst + got * channels, 0, (AUDIO_BLOCK_SAMPLES - got) * channels * sizeof(float));
	}
}

static int step(const char* what, int result)
{
	if (result != 0) {
		logf("[ps3] audio output: %s failed (0x%08x); the game goes on without sound\n", what, (unsigned)result);
	}
	return result;
}

static void audioThread(void*)
{
	// Listing the modules is new too, so it also happens here rather than on the main thread.
	ps3b_log_modules();
	logf("[ps3] audio output: loading the audio module\n");
	if (!moduleLoaded("ysmodule")) {
		// The module loader itself (cellSysmodule) is not loaded either: load it from the system flash.
		sysPrxId id = sysPrxLoadModule("/dev_flash/sys/external/libsysmodule.sprx", 0, NULL);
		if (id < 0) {
			step("loading libsysmodule.sprx", id);
			ps3b_thread_exit();
		}
		s32 modres = 0;
		if (step("starting libsysmodule.sprx", sysPrxStartModule(id, 0, NULL, &modres, 0, NULL)) != 0) {
			ps3b_thread_exit();
		}
		logf("[ps3] audio output: module loader started\n");
	}
	if (step("loading the audio module", sysModuleLoad(SYSMODULE_AUDIO)) != 0) {
		ps3b_thread_exit();
	}
	if (step("audioInit", audioInit()) != 0) {
		ps3b_thread_exit();
	}

	audioPortParam params;
	params.numChannels = AUDIO_PORT_2CH;
	params.numBlocks   = AUDIO_BLOCK_16;
	params.attrib      = AUDIO_PORT_INITLEVEL;
	params.level       = 1.0f;
	if (step("opening an audio port", audioPortOpen(&params, &sPort)) != 0) {
		ps3b_thread_exit();
	}
	audioPortConfig config;
	if (step("reading the port setup", audioGetPortConfig(sPort, &config)) != 0) {
		ps3b_thread_exit();
	}
	sys_event_queue_t queue;
	sys_ipc_key_t key;
	if (step("creating the event queue", audioCreateNotifyEventQueue(&queue, &key)) != 0
	    || step("connecting the event queue", audioSetNotifyEventQueue(key)) != 0) {
		ps3b_thread_exit();
	}
	sysEventQueueDrain(queue);

	const u32 channels = (u32)config.channelCount;
	const u32 blocks   = (u32)config.numBlocks;
	float* data        = (float*)(u64)config.audioDataStart;
	volatile u64* readIndex = (volatile u64*)(u64)config.readIndex;
	if (channels != 2 || blocks == 0 || !data) {
		logf("[ps3] audio output: unexpected port setup (%u channels, %u blocks)\n", (unsigned)channels, (unsigned)blocks);
		ps3b_thread_exit();
	}
	u32 last = (u32)(*readIndex % blocks);
	for (u32 i = 0; i < AHEAD; i++) {
		last = (last + 1) % blocks;
		fillBlock(data + last * channels * AUDIO_BLOCK_SAMPLES, channels);
	}
	if (step("starting the port", audioPortStart(sPort)) != 0) {
		ps3b_thread_exit();
	}
	sPlaying = 1;
	logf("[ps3] audio output: playing (48 kHz stereo)\n");

	while (!sStop) {
		sys_event_t event;
		sysEventQueueReceive(queue, &event, 100 * 1000); // one event per block played; 100 ms timeout
		u32 current = (u32)(*readIndex % blocks);
		u32 ahead   = (last + blocks - current) % blocks;
		while (ahead < AHEAD && !sStop) {
			last = (last + 1) % blocks;
			fillBlock(data + last * channels * AUDIO_BLOCK_SAMPLES, channels);
			ahead++;
		}
	}
	ps3b_thread_exit();
}

extern "C" void ps3b_audio_start(ps3b_audio_pull_fn pull)
{
	static int sStarted;
	if (sStarted) {
		return;
	}
	sStarted = 1;
	sPull    = pull;
	if (ps3b_thread_create(audioThread, NULL, 800, 0x10000, "pikmin_sound") != 0) {
		logf("[ps3] audio output: could not create its thread\n");
	}
}

extern "C" void ps3b_audio_shutdown(void)
{
	sStop = 1;
	if (sPlaying) {
		sPlaying = 0;
		audioPortStop(sPort);
	}
}
