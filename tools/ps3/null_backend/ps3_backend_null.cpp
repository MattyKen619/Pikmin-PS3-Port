// PS3 port: do-nothing backend used only by tools/ps3/link_check.py, so the whole port (game + SDK
// layer) can be compile- and link-checked without PSL1GHT installed. Never part of the PS3 build.
#include "../../../platform/ps3/backend/ps3_backend.h"

extern "C" {
void ps3b_init(void) { }
int ps3b_exit_requested(void) { return 0; }
void ps3b_exit(int code) { (void)code; for (;;) { } }
void* ps3b_memalign(unsigned int align, unsigned int size) { (void)align; (void)size; return 0; }
unsigned long long ps3b_timebase(void) { return 0; }
unsigned long long ps3b_timebase_freq(void) { return 79800000ULL; }
long long ps3b_epoch_seconds(void) { return 0; }
void ps3b_sleep_us(unsigned int usec) { (void)usec; }
int ps3b_mutex_create(unsigned int* out) { *out = 0; return 0; }
void ps3b_mutex_lock(unsigned int m) { (void)m; }
void ps3b_mutex_unlock(unsigned int m) { (void)m; }
int ps3b_cond_create(unsigned int* out, unsigned int mutex) { (void)mutex; *out = 0; return 0; }
int ps3b_cond_wait(unsigned int c, unsigned int t) { (void)c; (void)t; return 1; }
void ps3b_cond_signal(unsigned int c) { (void)c; }
void ps3b_cond_broadcast(unsigned int c) { (void)c; }
int ps3b_thread_create(void (*entry)(void*), void* arg, int prio, unsigned int stack, const char* name) { (void)entry; (void)arg; (void)prio; (void)stack; (void)name; return 0; }
void ps3b_thread_exit(void) { for (;;) { } }
int ps3b_file_exists(const char* path) { (void)path; return 0; }
int ps3b_file_open(const char* path) { (void)path; return -1; }
long long ps3b_file_size(int fd) { (void)fd; return -1; }
int ps3b_file_pread(int fd, void* buf, unsigned long long off, unsigned int len) { (void)fd; (void)buf; (void)off; (void)len; return -1; }
void ps3b_file_close(int fd) { (void)fd; }
int ps3b_file_read_all(const char* path, char** data, unsigned int* size) { (void)path; (void)data; (void)size; return 0; }
const char* ps3b_data_root(void) { return 0; }
void ps3b_log_write(const char* text) { (void)text; }
void ps3b_console_init(void) { }
void ps3b_pad_init(void) { }
void ps3b_pad_read(int port, Ps3bPad* out) { (void)port; out->connected = 0; }
}
