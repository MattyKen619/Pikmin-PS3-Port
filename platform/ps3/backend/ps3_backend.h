#ifndef _PS3_BACKEND_H
#define _PS3_BACKEND_H

// PS3 port: the boundary between the game-facing SDK layer (compiled with the game's headers) and the
// PS3 system code (compiled with PSL1GHT/newlib headers). The two header sets define `u64` differently,
// so nothing here uses either; only plain C types cross this line.

#ifdef __cplusplus
extern "C" {
#endif

// ---- process / startup
void ps3b_init(void);              // sysutil callbacks, timebase; call first
int ps3b_exit_requested(void);     // XMB "Quit Game" was chosen
void ps3b_exit(int code) __attribute__((noreturn));

// ---- memory (newlib heap)
void* ps3b_memalign(unsigned int align, unsigned int size);
void ps3b_free(void* p);

// ---- time
unsigned long long ps3b_timebase(void);       // PPU timebase counter
unsigned long long ps3b_timebase_freq(void);  // ticks per second (79.8 MHz on retail consoles)
long long ps3b_epoch_seconds(void);           // wall clock, seconds since 1970-01-01 (UTC)
void ps3b_sleep_us(unsigned int usec);

// ---- kernel synchronisation (lv2 mutex / condition variable)
int ps3b_mutex_create(unsigned int* out);
void ps3b_mutex_lock(unsigned int m);
void ps3b_mutex_unlock(unsigned int m);
int ps3b_cond_create(unsigned int* out, unsigned int mutex);
int ps3b_cond_wait(unsigned int c, unsigned int timeout_usec); // 0 = signalled, 1 = timed out; mutex held
void ps3b_cond_signal(unsigned int c);
void ps3b_cond_broadcast(unsigned int c);

// ---- threads
int ps3b_thread_create(void (*entry)(void*), void* arg, int priority, unsigned int stack_size, const char* name);
void ps3b_thread_exit(void) __attribute__((noreturn));

// ---- files (read-only game data + log)
int ps3b_file_exists(const char* path);
int ps3b_file_open(const char* path);                     // read-only; returns fd or -1
long long ps3b_file_size(int fd);
int ps3b_file_pread(int fd, void* buf, unsigned long long offset, unsigned int len); // bytes read or -1
void ps3b_file_close(int fd);
int ps3b_file_read_all(const char* path, char** out_data, unsigned int* out_size); // malloc'd, NUL-terminated
int ps3b_file_open_rw(const char* path, int create);     // read/write (memory card image); fd or -1
int ps3b_file_pwrite(int fd, const void* buf, unsigned long long offset, unsigned int len); // bytes written or -1

// ---- data location: the folder holding fst.txt and files/ (see tools/ps3/extract_disc.py)
const char* ps3b_data_root(void); // NULL if not found

// ---- logging: TTY, log file next to the game data, and the on-screen console.
// Lines are queued in memory and written out by a background thread about ten times a second, each
// batch with the file closed again at once, so the log survives a crash or a forced power-off (minus
// the last fraction of a second). The file is pikmin_log.txt in the data folder, or at the top of the
// first USB drive if the data folder is missing.
void ps3b_log_write(const char* text);   // queued; a background thread writes it out
void ps3b_log_flush(void);                // writes out everything queued, now
const char* ps3b_log_path(void);          // NULL if no writable location was found
void ps3b_log_memory(const char* when);   // logs lv2's user-memory totals

// ---- on-screen text console (shown while graphics are not implemented)
void ps3b_console_init(void);
void ps3b_console_shutdown(void);         // finish pending RSX work before the process exits

// ---- system modules
void ps3b_log_modules(void); // logs the names of the PRX modules loaded in this process (sound thread)

// ---- sound output (ps3_audio.cpp): 48 kHz stereo, 32-bit float frames, interleaved left/right.
// The pull function runs on the sound thread; it fills `frames` frames and returns how many it had.
typedef unsigned int (*ps3b_audio_pull_fn)(float* dst, unsigned int frames);
void ps3b_audio_start(ps3b_audio_pull_fn pull); // loads the audio module and starts playing (once)
void ps3b_audio_shutdown(void);                 // stops the port before the process exits

// ---- controllers
enum {
	PS3B_BTN_LEFT = 1 << 0,
	PS3B_BTN_DOWN = 1 << 1,
	PS3B_BTN_RIGHT = 1 << 2,
	PS3B_BTN_UP = 1 << 3,
	PS3B_BTN_START = 1 << 4,
	PS3B_BTN_R3 = 1 << 5,
	PS3B_BTN_L3 = 1 << 6,
	PS3B_BTN_SELECT = 1 << 7,
	PS3B_BTN_SQUARE = 1 << 8,
	PS3B_BTN_CROSS = 1 << 9,
	PS3B_BTN_CIRCLE = 1 << 10,
	PS3B_BTN_TRIANGLE = 1 << 11,
	PS3B_BTN_R1 = 1 << 12,
	PS3B_BTN_L1 = 1 << 13,
	PS3B_BTN_R2 = 1 << 14,
	PS3B_BTN_L2 = 1 << 15,
};

typedef struct Ps3bPad {
	int connected;
	unsigned int buttons;          // PS3B_BTN_* bits
	int lx, ly, rx, ry;            // 0..255, 128 = centre; y grows downwards
	int l2, r2;                    // 0..255
} Ps3bPad;

void ps3b_pad_init(void);
void ps3b_pad_read(int port, Ps3bPad* out);

#ifdef __cplusplus
}
#endif

#endif // _PS3_BACKEND_H
