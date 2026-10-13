#ifndef _JAUDIO_CMDQUEUE_H
#define _JAUDIO_CMDQUEUE_H

#include "jaudio/audiomesg.h"
#include "jaudio/jammain_2.h"

#include "types.h"

BEGIN_SCOPE_EXTERN_C

typedef struct cmdqueue_ CmdQueue;

struct cmdqueue_ {
	Jac_MessageQueue msgQueue; // _00
	OSMessage message;         // _20
#if defined(__LP64__)
	// Jal_AddCmdQueue gives msgQueue 16 messages starting at `message`. The GameCube layout reserves
	// 16 * 4 bytes for them; with 8-byte messages the 9th and later overwrote `track` (test build 5).
	OSMessage _moreMessages[15];
#else
	u8 _24[0x60 - 0x24];       // _24
#endif
	seqp_* track;              // _60
	u8 mPortId;                // _64
	CmdQueue* next;            // _68;
};

extern CmdQueue* queue_list; // Pointer to the start of a linked list

void Jal_AddCmdQueue(CmdQueue* cmdQueue, seqp_* track, u8 portId); // TODO: Types uncertain
void Jal_SendCmdQueue_Noblock(CmdQueue* queue, u16 msg);
void Jal_SendCmdQueue_Force(CmdQueue* queue, u16 msg);
void Jal_SendCmdQueue(CmdQueue* queue, u16 msg);
void Jal_CmdQueue_Init(void);

END_SCOPE_EXTERN_C

#endif
