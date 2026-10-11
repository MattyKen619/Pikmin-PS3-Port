#ifndef _PS3_STUB_H
#define _PS3_STUB_H

// PS3 port: placeholder ("stub") support. A stub reports the first time it is called, so a test run
// shows which SDK functions the game actually reaches before they have real PS3 implementations.

#ifdef __cplusplus
extern "C" {
#endif

void ps3StubReport(const char* name);

#ifdef __cplusplus
}
#endif

#define PS3_STUB(name)                     \
	do {                                   \
		static int s_ps3StubReported = 0;  \
		if (!s_ps3StubReported) {          \
			s_ps3StubReported = 1;         \
			ps3StubReport(name);           \
		}                                  \
	} while (0)

#endif // _PS3_STUB_H
