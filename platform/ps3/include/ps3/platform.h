#ifndef _PS3_PLATFORM_H
#define _PS3_PLATFORM_H

// PS3 port: entry point into the platform layer, called first thing in main().

#ifdef __cplusplus
extern "C" {
#endif

void ps3PlatformInit(void);
void ps3BootStage(const char* what); // writes a "[boot]" progress line to the log
void ps3HeapExhausted(const char* heapName, int requested, int freeBytes); // logs and stops the game
void ps3Report(const char* fmt, ...); // a line in the PS3 log (and on screen)
void ps3ReportStack(const char* what); // a line in the log plus the calling thread's call stack

#ifdef __cplusplus
}
#endif

#endif // _PS3_PLATFORM_H
