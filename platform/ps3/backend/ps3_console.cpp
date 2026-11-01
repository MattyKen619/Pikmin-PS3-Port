// PS3 port: on-screen text console. Until the game draws its first frames (the renderer in
// platform/ps3/rsx takes the display over, ps3b_console_take_display), the boot log is shown here.
// Drawn by the CPU straight into an RSX framebuffer.
// The display set-up is a step-by-step copy of PSL1GHT's "blitting" sample (known to work on the
// user's console), with every step logged so a failure shows up in the log file.
// A background thread redraws the screen, answers the XMB "Quit Game" request and watches for the
// Start+Select exit combination.

#include "ps3_backend.h"

#include <malloc.h>
#include <ppu-types.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include <rsx/rsx.h>
#include <sysutil/video.h>

#include "font8x8_basic.h"

#define CON_CB_SIZE    0x100000            // command buffer, as in the sample
#define CON_HOST_SIZE  (32 * 1024 * 1024)  // RSX-mapped main memory, as in the sample
#define CON_LABEL      255
#define CON_LINES      200
#define CON_COLS       160
#define CON_SCALE      2 // 8x8 font drawn at 16x16
#define CON_IDLE_FLIPS 5 // redraw at least every 5 x 100 ms so the display keeps receiving flips

static gcmContextData* sCtx = NULL;
static u32 sWidth, sHeight, sPitch;
static u32* sColor[2];
static u32 sColorOffset[2];
static u32 sCurrent      = 0;
static int sFirstFlip    = 1;
static volatile int sScreenReady = 0;
static u32 sLabelValue   = 1;
static unsigned int sRsxMutex = 0; // RSX command buffer: console thread vs. shutdown and the hand-over
static void* sHost        = NULL;
static volatile int sTaken = 0;    // the renderer owns the display
static void (*sOwnerStop)(void) = NULL;

static char sLines[CON_LINES][CON_COLS + 1];
static int sHead       = 0; // index of the line being written
static int sCol        = 0;
static int sTotalLines = 1;
static volatile int sDirty = 1;
static unsigned int sConMutex = 0;

#ifndef PS3_TEST_BUILD
#define PS3_TEST_BUILD 0
#endif
#define CON_STR2(x) #x
#define CON_STR(x)  CON_STR2(x)
static const char sTitle[] = "Pikmin PS3 port - build " CON_STR(PS3_TEST_BUILD)
                             " - boot log   (exit: PS button > Quit Game, or hold Start+Select)";

static void conLog(const char* fmt, ...)
{
	char buf[256];
	va_list args;
	va_start(args, fmt);
	vsnprintf(buf, sizeof(buf), fmt, args);
	va_end(args);
	ps3b_log_write(buf);
}

extern "C" void ps3b_console_append(const char* text)
{
	if (!sConMutex) {
		ps3b_mutex_create(&sConMutex);
	}
	ps3b_mutex_lock(sConMutex);
	for (const char* p = text; *p; p++) {
		if (*p == '\n' || sCol >= CON_COLS) {
			sHead              = (sHead + 1) % CON_LINES;
			sLines[sHead][0]   = 0;
			sCol               = 0;
			sTotalLines++;
			if (*p == '\n') {
				continue;
			}
		}
		char c = *p;
		if (c == '\t') {
			c = ' ';
		}
		if ((unsigned char)c < 32 || (unsigned char)c >= 127) {
			c = '?';
		}
		sLines[sHead][sCol++] = c;
		sLines[sHead][sCol]   = 0;
	}
	sDirty = 1;
	ps3b_mutex_unlock(sConMutex);
}

static void drawText(u32* fb, int x, int y, const char* s, u32 colour)
{
	for (; *s; s++, x += 8 * CON_SCALE) {
		if (x + 8 * CON_SCALE > (int)sWidth || y + 8 * CON_SCALE > (int)sHeight) {
			return;
		}
		const char* glyph = font8x8_basic[(unsigned char)*s & 0x7F];
		for (int row = 0; row < 8; row++) {
			unsigned char bits = (unsigned char)glyph[row];
			if (!bits) {
				continue;
			}
			for (int col = 0; col < 8; col++) {
				if (bits & (1 << col)) {
					for (int sy = 0; sy < CON_SCALE; sy++) {
						u32* line = fb + (y + row * CON_SCALE + sy) * (sPitch / 4) + x + col * CON_SCALE;
						for (int sx = 0; sx < CON_SCALE; sx++) {
							line[sx] = colour;
						}
					}
				}
			}
		}
	}
}

// ---- RSX synchronisation, as in the sample's rsxutil.cpp

static int waitFinish()
{
	rsxSetWriteBackendLabel(sCtx, CON_LABEL, sLabelValue);
	rsxFlushBuffer(sCtx);
	int spins = 0;
	while (*(vu32*)gcmGetLabelAddress(CON_LABEL) != sLabelValue) {
		ps3b_sleep_us(30);
		if (++spins > 100000) { // 3 s
			return 0;
		}
	}
	++sLabelValue;
	return 1;
}

static int waitRSXIdle()
{
	rsxSetWriteBackendLabel(sCtx, CON_LABEL, sLabelValue);
	rsxSetWaitLabel(sCtx, CON_LABEL, sLabelValue);
	++sLabelValue;
	return waitFinish();
}

static void waitFlip()
{
	int spins = 0;
	while (gcmGetFlipStatus() != 0) {
		ps3b_sleep_us(200);
		if (++spins > 10000) { // 2 s: don't hang the console thread on a stuck display
			break;
		}
	}
	gcmResetFlipStatus();
}

static void redraw()
{
	ps3b_mutex_lock(sRsxMutex);
	if (!sScreenReady) {
		ps3b_mutex_unlock(sRsxMutex);
		return;
	}
	u32* fb = sColor[sCurrent];
	for (u32 i = 0; i < sHeight * (sPitch / 4); i++) {
		fb[i] = 0x00101828;
	}
	const int lineH = 8 * CON_SCALE + 2;
	const int top   = 40;
	drawText(fb, 40, 12, sTitle, 0x00FFD040);
	int rows = ((int)sHeight - top - 30) / lineH;
	ps3b_mutex_lock(sConMutex);
	int avail = sTotalLines < CON_LINES ? sTotalLines : CON_LINES;
	int n     = avail < rows ? avail : rows;
	for (int i = 0; i < n; i++) {
		int idx = (sHead - (n - 1 - i) + CON_LINES) % CON_LINES;
		u32 col = 0x00E0E0E0;
		if (strstr(sLines[idx], "STALL") || strstr(sLines[idx], "PANIC") || strstr(sLines[idx], "FAILED")) {
			col = 0x00FF6060;
		} else if (strstr(sLines[idx], "[ps3 stub]")) {
			col = 0x0080A0FF;
		} else if (strstr(sLines[idx], "[boot]")) {
			col = 0x0080FF80;
		}
		drawText(fb, 40, top + i * lineH, sLines[idx], col);
	}
	sDirty = 0;
	ps3b_mutex_unlock(sConMutex);

	if (!sFirstFlip) {
		waitFlip();
	} else {
		gcmResetFlipStatus();
		sFirstFlip = 0;
	}
	gcmSetFlip(sCtx, sCurrent);
	rsxFlushBuffer(sCtx);
	gcmSetWaitFlip(sCtx);
	sCurrent ^= 1;
	ps3b_mutex_unlock(sRsxMutex);
}

static void consoleThread(void*)
{
	int idle      = 0;
	int comboHeld = 0;
	for (;;) {
		if (ps3b_exit_requested()) {
			ps3b_exit(0);
		}
		// Start+Select held for 2 seconds on any controller: leave without needing the XMB.
		int combo = 0;
		for (int port = 0; port < 4; port++) {
			Ps3bPad pad;
			ps3b_pad_read(port, &pad);
			if ((pad.buttons & (PS3B_BTN_START | PS3B_BTN_SELECT)) == (PS3B_BTN_START | PS3B_BTN_SELECT)) {
				combo = 1;
			}
		}
		comboHeld = combo ? comboHeld + 1 : 0;
		if (comboHeld == 20) {
			ps3b_log_write("[ps3] Start+Select held: quitting\n");
			ps3b_exit(0);
		}
		if (sScreenReady && (sDirty || ++idle >= CON_IDLE_FLIPS)) {
			redraw();
			idle = 0;
		}
		ps3b_sleep_us(100 * 1000);
	}
}

// Sets up the display. Returns 0 if the screen could not be set up (the console thread still runs,
// so quitting from the XMB and with Start+Select keeps working).
static int setupDisplay()
{
	conLog("[boot] video: reserving %d MB of RSX-mapped memory\n", CON_HOST_SIZE >> 20);
	void* host = memalign(1024 * 1024, CON_HOST_SIZE);
	sHost      = host;
	if (!host) {
		conLog("[boot] video: FAILED to allocate the RSX memory\n");
		return 0;
	}
	conLog("[boot] video: rsxInit\n");
	s32 r = rsxInit(&sCtx, CON_CB_SIZE, CON_HOST_SIZE, host);
	if (r != 0 || !sCtx) {
		conLog("[boot] video: FAILED rsxInit (error 0x%08x)\n", (unsigned)r);
		return 0;
	}

	videoState state;
	videoResolution res;
	memset(&state, 0, sizeof(state));
	memset(&res, 0, sizeof(res));
	r = videoGetState(0, 0, &state);
	conLog("[boot] video: output state %d, resolution id %d (result 0x%08x)\n", (int)state.state,
	       (int)state.displayMode.resolution, (unsigned)r);
	r = videoGetResolution(state.displayMode.resolution, &res);
	if (r != 0 || res.width == 0 || res.height == 0) {
		conLog("[boot] video: FAILED videoGetResolution (error 0x%08x)\n", (unsigned)r);
		return 0;
	}
	conLog("[boot] video: %u x %u\n", (unsigned)res.width, (unsigned)res.height);

	videoConfiguration vconfig;
	memset(&vconfig, 0, sizeof(vconfig));
	vconfig.resolution = state.displayMode.resolution;
	vconfig.format     = VIDEO_BUFFER_FORMAT_XRGB;
	vconfig.pitch      = res.width * sizeof(u32);

	if (!waitRSXIdle()) {
		conLog("[boot] video: FAILED waiting for the RSX to become idle\n");
	}
	r = videoConfigure(0, &vconfig, NULL, 0);
	if (r != 0) {
		conLog("[boot] video: FAILED videoConfigure (error 0x%08x)\n", (unsigned)r);
	}
	videoGetState(0, 0, &state);
	gcmSetFlipMode(GCM_FLIP_VSYNC);

	sWidth  = res.width;
	sHeight = res.height;
	sPitch  = sWidth * sizeof(u32);
	for (int i = 0; i < 2; i++) {
		sColor[i] = (u32*)rsxMemalign(64, sHeight * sPitch);
		if (!sColor[i]) {
			conLog("[boot] video: FAILED to allocate frame buffer %d\n", i);
			return 0;
		}
		rsxAddressToOffset(sColor[i], &sColorOffset[i]);
		gcmSetDisplayBuffer(i, sColorOffset[i], sPitch, sWidth, sHeight);
	}
	gcmResetFlipStatus();
	sScreenReady = 1;

	conLog("[boot] video: drawing the first frame\n");
	redraw(); // now, on this thread, so the screen is up even if something fails right after
	conLog("[boot] video: ready\n");
	return 1;
}

extern "C" void ps3b_console_init(void)
{
	if (!sConMutex) {
		ps3b_mutex_create(&sConMutex);
	}
	ps3b_mutex_create(&sRsxMutex);
	ps3b_pad_init();
	setupDisplay();
	if (ps3b_thread_create(consoleThread, NULL, 1000, 0x10000, "pikmin_console") != 0) {
		conLog("[boot] FAILED to start the console thread\n");
	}
}

extern "C" int ps3b_console_take_display(Ps3bDisplay* out)
{
	if (!sRsxMutex || !sScreenReady) {
		return -1;
	}
	ps3b_mutex_lock(sRsxMutex);
	if (!sScreenReady) {
		ps3b_mutex_unlock(sRsxMutex);
		return -1;
	}
	sScreenReady = 0;
	sTaken       = 1;
	if (!waitRSXIdle()) {
		conLog("[boot] video: the RSX did not become idle for the hand-over\n");
	}
	out->context     = sCtx;
	out->width       = sWidth;
	out->height      = sHeight;
	out->pitch       = sPitch;
	out->buffers[0]  = sColor[0];
	out->buffers[1]  = sColor[1];
	out->offsets[0]  = sColorOffset[0];
	out->offsets[1]  = sColorOffset[1];
	out->next        = sCurrent;
	out->flipped     = !sFirstFlip;
	out->host        = sHost;
	out->hostSize    = CON_HOST_SIZE;
	out->commandSize = CON_CB_SIZE;
	ps3b_mutex_unlock(sRsxMutex);
	return 0;
}

extern "C" void ps3b_console_set_display_owner_stop(void (*stop)(void)) { sOwnerStop = stop; }

extern "C" void ps3b_console_shutdown(void)
{
	if (sTaken) {
		if (sOwnerStop) {
			sOwnerStop(); // (the renderer stops and waits for the RSX)
		}
		return;
	}
	if (!sScreenReady || !sRsxMutex) {
		return;
	}
	ps3b_mutex_lock(sRsxMutex);
	sScreenReady = 0;
	gcmSetWaitFlip(sCtx);
	rsxFinish(sCtx, 1);
	ps3b_mutex_unlock(sRsxMutex);
}
