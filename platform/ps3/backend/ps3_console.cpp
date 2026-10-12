// PS3 port: on-screen text console. Until the GX->RSX graphics layer exists (Phase 4) the game draws
// nothing, so the boot log is shown here instead. Drawn by the CPU straight into an RSX framebuffer
// (display setup follows PSL1GHT's "blitting" sample). A background thread redraws it when it changes
// and also answers the XMB "Quit Game" request.

#include "ps3_backend.h"

#include <malloc.h>
#include <ppu-types.h>
#include <stdio.h>
#include <string.h>

#include <rsx/rsx.h>
#include <sysutil/video.h>

#include "font8x8_basic.h"

#define CON_CB_SIZE   0x100000
#define CON_HOST_SIZE (2 * 1024 * 1024)
#define CON_LINES     200
#define CON_COLS      160
#define CON_SCALE     2 // 8x8 font drawn at 16x16

static gcmContextData* sCtx = NULL;
static u32 sWidth, sHeight, sPitch;
static u32* sColor[2];
static u32 sColorOffset[2];
static u32 sCurrent      = 0;
static int sFirstFlip    = 1;
static int sScreenReady  = 0;

static char sLines[CON_LINES][CON_COLS + 1];
static int sHead       = 0; // index of the line being written
static int sCol        = 0;
static int sTotalLines = 1;
static volatile int sDirty = 1;
static unsigned int sConMutex = 0;

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
		if (x + 8 * CON_SCALE > (int)sWidth) {
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

static void waitFlip()
{
	while (gcmGetFlipStatus() != 0) {
		ps3b_sleep_us(200);
	}
	gcmResetFlipStatus();
}

static void redraw()
{
	u32* fb = sColor[sCurrent];
	// background
	for (u32 i = 0; i < sHeight * (sPitch / 4); i++) {
		fb[i] = 0x00101828;
	}
	const int lineH = 8 * CON_SCALE + 2;
	const int top   = 40;
	drawText(fb, 40, 12, "Pikmin PS3 port - boot log (graphics not implemented yet)  |  PS button > Quit Game to exit", 0x00FFD040);
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
}

static void consoleThread(void*)
{
	for (;;) {
		if (ps3b_exit_requested()) {
			ps3b_exit(0);
		}
		if (sScreenReady && sDirty) {
			redraw();
		}
		ps3b_sleep_us(100 * 1000);
	}
}

extern "C" void ps3b_console_init(void)
{
	if (!sConMutex) {
		ps3b_mutex_create(&sConMutex);
	}
	void* host = memalign(1024 * 1024, CON_HOST_SIZE);
	rsxInit(&sCtx, CON_CB_SIZE, CON_HOST_SIZE, host);

	videoState state;
	videoResolution res;
	videoGetState(0, 0, &state);
	videoGetResolution(state.displayMode.resolution, &res);

	videoConfiguration vconfig;
	memset(&vconfig, 0, sizeof(vconfig));
	vconfig.resolution = state.displayMode.resolution;
	vconfig.format     = VIDEO_BUFFER_FORMAT_XRGB;
	vconfig.pitch      = res.width * sizeof(u32);
	videoConfigure(0, &vconfig, NULL, 0);
	videoGetState(0, 0, &state);
	gcmSetFlipMode(GCM_FLIP_VSYNC);

	sWidth  = res.width;
	sHeight = res.height;
	sPitch  = sWidth * sizeof(u32);
	for (int i = 0; i < 2; i++) {
		sColor[i] = (u32*)rsxMemalign(64, sHeight * sPitch);
		rsxAddressToOffset(sColor[i], &sColorOffset[i]);
		gcmSetDisplayBuffer(i, sColorOffset[i], sPitch, sWidth, sHeight);
	}
	sScreenReady = 1;
	sDirty       = 1;
	ps3b_thread_create(consoleThread, NULL, 1000, 0x10000, "pikmin_console");
}
