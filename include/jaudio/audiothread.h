#ifndef _JAUDIO_AUDIOTHREAD_H
#define _JAUDIO_AUDIOTHREAD_H

#include "Dolphin/OS/OSMessage.h"
#include "types.h"

BEGIN_SCOPE_EXTERN_C

/////////// JAUDIO AUDIO THREAD DEFINITIONS ///////////
// Defines.
#define AUDIO_THREAD_FLAG_DVD   (1 << 0)
#define AUDIO_THREAD_FLAG_AUDIO (1 << 1)
#define AUDIO_THREAD_FLAG_NEOS  (1 << 2)

#define AUDIO_STACK_SIZE       0x1000
#define AUDIOPROC_MQ_BUF_COUNT 16

// Integer IDs, usable as `case` labels in standard C++; the OSMessage forms below are sent through the queue.
#define AUDIOPROC_MESSAGE_ID_UPDATE_DAC 0
#define AUDIOPROC_MESSAGE_ID_DSP_SYNC   1
#define AUDIOPROC_MESSAGE_ID_NEOS_SYNC  2
#define AUDIOPROC_MESSAGE_ID_3          3

#define AUDIOPROC_MESSAGE_UPDATE_DAC ((OSMessage)AUDIOPROC_MESSAGE_ID_UPDATE_DAC)
#define AUDIOPROC_MESSAGE_DSP_SYNC   ((OSMessage)AUDIOPROC_MESSAGE_ID_DSP_SYNC)
#define AUDIOPROC_MESSAGE_NEOS_SYNC  ((OSMessage)AUDIOPROC_MESSAGE_ID_NEOS_SYNC)
#define AUDIOPROC_MESSAGE_3          ((OSMessage)AUDIOPROC_MESSAGE_ID_3)

///////////////////////////////////////////////////////

//////////// JAUDIO AUDIO THREAD FUNCTIONS ////////////
void DspSyncCountClear(int count);
int DspSyncCountCheck();
void StopAudioThread();
void StartAudioThread(void* heap, s32 heapSize, u32 aramSize, u32 flags);

///////////////////////////////////////////////////////

END_SCOPE_EXTERN_C

#endif
