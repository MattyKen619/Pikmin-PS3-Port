// PS3 port: video timing (VI), controllers (PAD) and ARAM (AR/ARQ).

#include "sdk_internal.h"

#include "../backend/ps3_backend.h"
#include "Dolphin/ar.h"
#include "Dolphin/card.h"
#include "Dolphin/pad.h"
#include "Dolphin/vi.h"
#include <string.h>

// ================================================================== VI: 59.94 Hz retrace

static volatile u32 sRetraceCount;
static volatile u32 sPendingRetraces;
static VIRetraceCallback sPostRetraceCallback;
static int sRetraceKey; // wait key for VIWaitForRetrace
static int sViStarted;

static void retraceInterrupt(void*)
{
	__sync_fetch_and_sub(&sPendingRetraces, 1);
	sRetraceCount++;
	if (sPostRetraceCallback) {
		sPostRetraceCallback(sRetraceCount);
	}
	ps3WakeAll(&sRetraceKey);
}

static void viTimerThread(void*)
{
	unsigned long long freq   = ps3b_timebase_freq();
	unsigned long long period = freq * 1001ULL / 60000ULL; // NTSC 59.94 Hz
	unsigned long long next   = ps3b_timebase() + period;
	for (;;) {
		unsigned long long now = ps3b_timebase();
		if (now < next) {
			ps3b_sleep_us((unsigned int)((next - now) * 1000000ULL / freq) + 1);
			continue;
		}
		next += period;
		if (now > next + period * 10) {
			next = now + period; // fell far behind (e.g. a long load): don't try to catch up
		}
		// Never queue more than a few retraces; the GameCube can't either.
		if (sPendingRetraces < 4) {
			__sync_fetch_and_add(&sPendingRetraces, 1);
			ps3PostInterrupt(retraceInterrupt, NULL);
		}
	}
}

extern "C" void ps3ViStart(void)
{
	if (!sViStarted) {
		sViStarted = 1;
		ps3b_thread_create(viTimerThread, NULL, 900, 0x8000, "pikmin_vi");
	}
}

extern "C" unsigned int ps3FrameCounter(void) { return sRetraceCount; }

void VIInit(void) { PS3_ACTIVITY("VIInit"); }
void VIConfigure(const GXRenderModeObj* rm) { (void)rm; }
void VIFlush(void) { }
void VISetNextFrameBuffer(void* fb) { (void)fb; }
void VISetBlack(BOOL black) { (void)black; }
u32 VIGetDTVStatus(void) { return 0; } // no component cable: the game won't offer progressive mode

void VIWaitForRetrace(void)
{
	PS3_ACTIVITY("VIWaitForRetrace");
	ps3DeliverInterrupts();
	u32 start = sRetraceCount;
	while (sRetraceCount == start) {
		ps3WaitOn(&sRetraceKey, "VIWaitForRetrace");
	}
}

u32 VIGetRetraceCount(void)
{
	ps3DeliverInterrupts();
	return sRetraceCount;
}

VIRetraceCallback VISetPostRetraceCallback(VIRetraceCallback callback)
{
	BOOL enabled          = OSDisableInterrupts();
	VIRetraceCallback old = sPostRetraceCallback;
	sPostRetraceCallback  = callback;
	OSRestoreInterrupts(enabled);
	return old;
}

// ================================================================== PAD: DualShock 3 -> GameCube pad
//
//   Cross = A, Circle = B, Square = X, Triangle = Y, Start = START
//   L1 = L, R1 = R, L2 = L (analog), R2 = Z, D-pad = D-pad
//   Left stick = control stick, right stick = C-stick

static s8 toS8(int v)
{
	if (v > 127) {
		return 127;
	}
	if (v < -128) {
		return -128;
	}
	return (s8)v;
}

extern "C" void ps3RsxDebugButtons(unsigned int buttons); // (rsx_port.cpp)

BOOL PADInit(void)
{
	ps3b_pad_init();
	ps3Log("[ps3] controller: Cross=A Circle=B Square=X Triangle=Y L1/L2=L R1=R R2=Z, right stick=C-stick\n");
	return TRUE;
}

u32 PADRead(PADStatus* status)
{
	PS3_ACTIVITY("PADRead");
	ps3DeliverInterrupts();
	u32 mask = 0;
	for (int ch = 0; ch < PAD_MAX_CONTROLLERS; ch++) {
		Ps3bPad p;
		ps3b_pad_read(ch, &p);
		PADStatus* s = &status[ch];
		memset(s, 0, sizeof(*s));
		if (!p.connected) {
			s->err = PAD_ERR_NO_CONTROLLER;
			continue;
		}
		if (ch == 0) {
			ps3RsxDebugButtons(p.buttons); // (R3, L3: test switches for the renderer)
		}
		u16 b = 0;
		if (p.buttons & PS3B_BTN_CROSS) b |= PAD_BUTTON_A;
		if (p.buttons & PS3B_BTN_CIRCLE) b |= PAD_BUTTON_B;
		if (p.buttons & PS3B_BTN_SQUARE) b |= PAD_BUTTON_X;
		if (p.buttons & PS3B_BTN_TRIANGLE) b |= PAD_BUTTON_Y;
		if (p.buttons & PS3B_BTN_START) b |= PAD_BUTTON_START;
		if (p.buttons & (PS3B_BTN_L1 | PS3B_BTN_L2)) b |= PAD_TRIGGER_L;
		if (p.buttons & PS3B_BTN_R1) b |= PAD_TRIGGER_R;
		if (p.buttons & PS3B_BTN_R2) b |= PAD_TRIGGER_Z;
		if (p.buttons & PS3B_BTN_LEFT) b |= PAD_BUTTON_LEFT;
		if (p.buttons & PS3B_BTN_RIGHT) b |= PAD_BUTTON_RIGHT;
		if (p.buttons & PS3B_BTN_UP) b |= PAD_BUTTON_UP;
		if (p.buttons & PS3B_BTN_DOWN) b |= PAD_BUTTON_DOWN;
		s->button       = b;
		s->stickX       = toS8(p.lx - 128);
		s->stickY       = toS8(128 - p.ly); // GameCube: up is positive
		s->substickX    = toS8(p.rx - 128);
		s->substickY    = toS8(128 - p.ry);
		s->triggerLeft  = (p.buttons & PS3B_BTN_L1) ? 255 : (u8)p.l2;
		s->triggerRight = (p.buttons & PS3B_BTN_R1) ? 255 : 0;
		s->analogA      = (b & PAD_BUTTON_A) ? 255 : 0;
		s->analogB      = (b & PAD_BUTTON_B) ? 255 : 0;
		s->err          = PAD_ERR_NONE;
		mask |= PAD_CHAN0_BIT >> ch;

		// Log the first button changes so a test log shows the controller reaching the game.
		static u16 sLastButtons[PAD_MAX_CONTROLLERS];
		static int sLoggedChanges;
		if (b != sLastButtons[ch] && sLoggedChanges < 40) {
			sLoggedChanges++;
			ps3Log("[pad] controller %d: %s%s%s%s%s%s%s%s%s%s%s%s%s\n", ch + 1, b ? "" : "(released)",
			       (b & PAD_BUTTON_A) ? " A" : "", (b & PAD_BUTTON_B) ? " B" : "", (b & PAD_BUTTON_X) ? " X" : "",
			       (b & PAD_BUTTON_Y) ? " Y" : "", (b & PAD_BUTTON_START) ? " START" : "", (b & PAD_TRIGGER_L) ? " L" : "",
			       (b & PAD_TRIGGER_R) ? " R" : "", (b & PAD_TRIGGER_Z) ? " Z" : "", (b & PAD_BUTTON_UP) ? " UP" : "",
			       (b & PAD_BUTTON_DOWN) ? " DOWN" : "", (b & PAD_BUTTON_LEFT) ? " LEFT" : "", (b & PAD_BUTTON_RIGHT) ? " RIGHT" : "");
		}
		sLastButtons[ch] = b;
	}
	return mask;
}

// Same dead zones and limits as the GameCube SDK's PADClamp (octagonal stick gates).
static void clampStick(s8* px, s8* py, int max, int xy, int min)
{
	int x = *px, y = *py;
	int signX = 1, signY = 1;
	if (x < 0) {
		signX = -1;
		x     = -x;
	}
	if (y < 0) {
		signY = -1;
		y     = -y;
	}
	x = (x <= min) ? 0 : x - min;
	y = (y <= min) ? 0 : y - min;
	if (x == 0 && y == 0) {
		*px = *py = 0;
		return;
	}
	int d = (xy * y <= xy * x) ? (xy * x + (max - xy) * y) : (xy * y + (max - xy) * x);
	if (xy * max < d) {
		x = xy * max * x / d;
		y = xy * max * y / d;
	}
	*px = (s8)(signX * x);
	*py = (s8)(signY * y);
}

static void clampTrigger(u8* t, int min, int max)
{
	if (*t <= min) {
		*t = 0;
	} else {
		if (*t > max) {
			*t = (u8)max;
		}
		*t = (u8)(*t - min);
	}
}

void PADClamp(PADStatus* status)
{
	for (int ch = 0; ch < PAD_MAX_CONTROLLERS; ch++) {
		PADStatus* s = &status[ch];
		if (s->err != PAD_ERR_NONE) {
			continue;
		}
		clampStick(&s->stickX, &s->stickY, 72, 40, 15);
		clampStick(&s->substickX, &s->substickY, 59, 31, 15);
		clampTrigger(&s->triggerLeft, 30, 180);
		clampTrigger(&s->triggerRight, 30, 180);
	}
}

BOOL PADReset(u32 mask) { (void)mask; return TRUE; }
BOOL PADRecalibrate(u32 mask) { (void)mask; return TRUE; }
void PADSetSpec(u32 spec) { (void)spec; }
void PADControlMotor(s32 chan, u32 command) { (void)chan; (void)command; } // rumble: later

// ================================================================== ARAM: 16 MB of ordinary memory

#define ARAM_SIZE (16 * 1024 * 1024)
#define ARAM_BASE 0x4000 // the SDK reserves the first 16 KB

static u8* sAram;

u32 ARInit(u32* stack, u32 numEntries)
{
	(void)stack;
	(void)numEntries;
	if (!sAram) {
		sAram = (u8*)ps3b_memalign(32, ARAM_SIZE);
		if (sAram) {
			memset(sAram, 0, ARAM_SIZE);
		} else {
			ps3Log("[ps3] FAILED to allocate ARAM\n");
		}
	}
	return ARAM_BASE;
}

u32 ARGetBaseAddress(void) { return ARAM_BASE; }

// For the DSP (dsp_hle.cpp): ARAM bytes [address, address + size), or NULL if out of range.
extern "C" u8* ps3AramRange(u32 address, u32 size)
{
	if (!sAram || (u64)address + size > ARAM_SIZE) {
		return NULL;
	}
	return sAram + address;
}
extern "C" u32 ps3AramSize(void) { return ARAM_SIZE; }
void ARQInit(void) { }

extern "C" void ps3DspNoteAramWrite(u32 dest, u32 length, int stop); // (dsp_hle.cpp)
extern "C" int ps3DspVoicesReadingAram(u32 dest, u32 length);
extern "C" int ps3CanYield(void); // (os_sched.cpp)

// JAudio's sound bank loader (DVDT_LoadtoARAM: the disc to ARAM, 64 KB at a time) names itself this way.
enum { JAUDIO_LOADER = 0x12345678, BANK_WAIT_RETRACES = 60 };
static u32 sBankWaits, sBankWaitedRetraces;

// Before the loader writes over the sound data of voices that are still playing (music fading out when a
// level or menu loads): it waits for them, as the GameCube's slower disc did, up to about a second.
// (dsp_hle.cpp: ps3DspNoteAramWrite.)
static void waitForVoices(u32 dest, u32 length)
{
	// (after a wait that ran out, the rest of that load does not wait again: a few seconds at most in all)
	static u32 sGaveUpAt;
	static bool sGaveUp;
	if (!ps3CanYield() || !ps3DspVoicesReadingAram(dest, length) || (sGaveUp && sRetraceCount - sGaveUpAt < 2 * BANK_WAIT_RETRACES)) {
		return;
	}
	const u32 start = sRetraceCount;
	while (ps3DspVoicesReadingAram(dest, length) && sRetraceCount - start < BANK_WAIT_RETRACES) {
		const u32 at = sRetraceCount;
		while (sRetraceCount == at) {
			ps3WaitOn(&sRetraceKey, "sound bank load (voices still playing its ARAM)");
		}
	}
	if (ps3DspVoicesReadingAram(dest, length)) {
		sGaveUp   = true;
		sGaveUpAt = sRetraceCount;
	}
	sBankWaits++;
	sBankWaitedRetraces += sRetraceCount - start;
	if (sBankWaits <= 8) {
		ps3Log("[sound] a sound bank load waited %u ms for voices playing from ARAM 0x%06x-0x%06x%s\n",
		       (unsigned)((sRetraceCount - start) * 1001 / 60), (unsigned)dest, (unsigned)(dest + length),
		       ps3DspVoicesReadingAram(dest, length) ? " (some still playing: stopped)" : "");
	}
}

// Transfers complete immediately and call back before returning: the game busy-waits for some of
// these callbacks (System::copyWaitUntilDone), which would never end if they were deferred.
void ARQPostRequest(ARQRequest* request, u32 owner, u32 type, u32 priority, u32 source, u32 dest, u32 length, ARQCallback callback)
{
	PS3_ACTIVITY("ARQPostRequest");
	request->next     = NULL;
	request->owner    = owner;
	request->type     = type;
	request->priority = priority;
	request->source   = source;
	request->dest     = dest;
	request->length   = length;
	request->callback = callback;
	if (type == ARQ_TYPE_MRAM_TO_ARAM) {
		if (!sAram || (u64)dest + length > ARAM_SIZE) {
			ps3Log("[ps3] FAILED: ARAM write out of range (0x%x + 0x%x)\n", (unsigned)dest, (unsigned)length);
		} else {
			if (owner == JAUDIO_LOADER) {
				waitForVoices(dest, length);
			}
			// (any write: test build 43's log showed JAudio's ARAM heap compaction, which moves sound banks down
			// when one is freed, also writing over voices still playing)
			ps3DspNoteAramWrite(dest, length, 1);
			memcpy(sAram + dest, (void*)(uptr)source, length);
		}
	} else {
		if (!sAram || (u64)source + length > ARAM_SIZE) {
			ps3Log("[ps3] FAILED: ARAM read out of range (0x%x + 0x%x)\n", (unsigned)source, (unsigned)length);
		} else {
			memcpy((void*)(uptr)dest, sAram + source, length);
		}
	}
	if (callback) {
		callback((u32)(uptr)request);
	}
}

// CARD (memory card): see card.cpp.

