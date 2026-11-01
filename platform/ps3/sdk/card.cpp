// PS3 port: the GameCube memory card (CARD API), kept in a file: /dev_hdd0/data/pikmin/MemoryCardA.USA.raw
// (ps3b_save_dir; up to build 51 it was next to the game data, and is copied from there the first time).
//
// The file is a raw image of a "Memory Card 251" (2 MB) in the GameCube's own format, the same format
// as Dolphin's MemoryCardA.USA.raw: block 0 header, blocks 1-2 directory (two copies), blocks 3-4
// block allocation table (two copies), then 8 KB data blocks. A card made by Dolphin can be copied in,
// and this one can be used in Dolphin. Slot A only; slot B is always empty.
//
// The whole image is held in memory; every change writes the touched blocks back to the file at once
// (a power cut can't lose more than the operation in progress). All operations complete immediately;
// the asynchronous variants report their result through CARDGetResultCode and their callback, run like
// the GameCube's EXI interrupt would.
//
// The file-system rules follow the Dolphin SDK (src/card in this repository): access by game code and
// maker code or the public bit, contiguous-or-chained block allocation from the last allocated block,
// 16-bit checksums with the 0xFFFF -> 0 rule, and icon offsets as in CARDStat.c.

#include "sdk_internal.h"

#include "../backend/ps3_backend.h"
#include "Dolphin/card.h"
#include "Dolphin/dvd.h"
#include <stdio.h>
#include <string.h>

#define VCARD_SIZE_MBITS 16                                   // Memory Card 251
#define VCARD_BLOCKS     (VCARD_SIZE_MBITS * 1024 * 1024 / 8 / CARD_SYSTEM_BLOCK_SIZE) // 256
#define VCARD_SECTOR     CARD_SYSTEM_BLOCK_SIZE                // 8 KB
#define VCARD_FILE       "MemoryCardA.USA.raw"

struct VCard {
	int loaded;  // image in memory (and on disk)
	int mounted; // between CARDMount and CARDUnmount
	int broken;  // image failed its checks; only CARDFormat helps
	u8* image;
	u32 size;    // bytes
	u16 cBlock;  // blocks on the card, system blocks included
	CARDDirectoryBlock* dir;
	CARDFatBlock* fat;
	s32 result; // CARDGetResultCode
	s32 xferred;
	char path[256];
};

static VCard sCard;

static u16* fatMap() { return (u16*)sCard.fat; } // indexed by block number (entries 0-4 are the header)

// ------------------------------------------------------------------ checksums and image layout

static void checkSum(const void* data, int length, u16* sum, u16* inv)
{
	const u16* p = (const u16*)data;
	u16 s = 0, i = 0;
	for (int n = 0; n < length / 2; n++) {
		s += p[n];
		i += (u16)~p[n];
	}
	*sum = (s == 0xFFFF) ? 0 : s;
	*inv = (i == 0xFFFF) ? 0 : i;
}

static CARDID* header() { return (CARDID*)sCard.image; }
static CARDDirectoryBlock* dirCopy(int i) { return (CARDDirectoryBlock*)(sCard.image + (1 + i) * VCARD_SECTOR); }
static CARDFatBlock* fatCopy(int i) { return (CARDFatBlock*)(sCard.image + (3 + i) * VCARD_SECTOR); }

static int dirValid(CARDDirectoryBlock* d)
{
	u16 s, i;
	checkSum(d, VCARD_SECTOR - 4, &s, &i);
	return s == d->check.checkSum && i == d->check.checkSumInv;
}

static int fatValid(CARDFatBlock* f)
{
	u16 s, i;
	checkSum(&f->checkCode, VCARD_SECTOR - 4, &s, &i);
	return s == f->checkSum && i == f->checkSumInv;
}

// ------------------------------------------------------------------ the image file

static int writeBlocks(int first, int count)
{
	int fd = ps3b_file_open_rw(sCard.path, 1);
	if (fd < 0) {
		ps3Log("[card] FAILED to open %s for writing\n", sCard.path);
		return 0;
	}
	unsigned int len = (unsigned int)count * VCARD_SECTOR;
	int ok           = ps3b_file_pwrite(fd, sCard.image + first * VCARD_SECTOR, (unsigned long long)first * VCARD_SECTOR, len) == (int)len;
	ps3b_file_close(fd);
	if (!ok) {
		ps3Log("[card] FAILED writing %s\n", sCard.path);
	}
	return ok;
}

// The SDK writes the backup copy and switches to it; here both copies are kept identical.
static int commitDir()
{
	CARDDirectoryBlock* d = sCard.dir;
	d->check.checkCode++;
	checkSum(d, VCARD_SECTOR - 4, &d->check.checkSum, &d->check.checkSumInv);
	for (int i = 0; i < 2; i++) {
		if (dirCopy(i) != d) {
			memcpy(dirCopy(i), d, VCARD_SECTOR);
		}
	}
	return writeBlocks(1, 2);
}

static int commitFat()
{
	CARDFatBlock* f = sCard.fat;
	f->checkCode++;
	checkSum(&f->checkCode, VCARD_SECTOR - 4, &f->checkSum, &f->checkSumInv);
	for (int i = 0; i < 2; i++) {
		if (fatCopy(i) != f) {
			memcpy(fatCopy(i), f, VCARD_SECTOR);
		}
	}
	return writeBlocks(3, 2);
}

static void formatImage()
{
	memset(sCard.image, 0xFF, sCard.size);
	CARDID* id = header();
	OSTime now = OSGetTime();
	u32 rnd    = (u32)now;
	for (int i = 0; i < 12; i++) {
		rnd           = rnd * 1103515245u + 12345u;
		id->serial[i] = (u8)(rnd >> 16);
	}
	memcpy(&id->serial[12], &now, sizeof(now));
	memset(&id->serial[20], 0, 12); // counter bias, language (English), DTV status
	id->deviceID = 0;
	id->size     = (u16)(sCard.cBlock * VCARD_SECTOR * 8 / (1024 * 1024));
	id->encode   = CARD_ENCODE_ANSI;
	checkSum(id, sizeof(CARDID) - 4, &id->checkSum, &id->checkSumInv);

	for (int i = 0; i < 2; i++) {
		CARDDirectoryBlock* d = dirCopy(i);
		memset(d, 0xFF, VCARD_SECTOR);
		d->check.checkCode = (s16)i;
		checkSum(d, VCARD_SECTOR - 4, &d->check.checkSum, &d->check.checkSumInv);
		CARDFatBlock* f = fatCopy(i);
		memset(f, 0, VCARD_SECTOR);
		f->checkCode      = (u16)i;
		f->freeBlocks     = (u16)(sCard.cBlock - CARD_NUM_SYSTEM_BLOCK);
		f->lastAllocBlock = CARD_NUM_SYSTEM_BLOCK - 1;
		checkSum(&f->checkCode, VCARD_SECTOR - 4, &f->checkSum, &f->checkSumInv);
	}
	sCard.dir    = dirCopy(1);
	sCard.fat    = fatCopy(1);
	sCard.broken = 0;
}

// Picks the newer valid copy of the directory and the allocation table (as CARDMount does).
static int selectCurrent()
{
	u16 s, i;
	checkSum(header(), sizeof(CARDID) - 4, &s, &i);
	if (s != header()->checkSum || i != header()->checkSumInv) {
		return 0;
	}
	int d0 = dirValid(dirCopy(0)), d1 = dirValid(dirCopy(1));
	int f0 = fatValid(fatCopy(0)), f1 = fatValid(fatCopy(1));
	if (!(d0 || d1) || !(f0 || f1)) {
		return 0;
	}
	if (d0 && d1) {
		sCard.dir = ((s16)(dirCopy(0)->check.checkCode - dirCopy(1)->check.checkCode) >= 0) ? dirCopy(0) : dirCopy(1);
	} else {
		sCard.dir = d0 ? dirCopy(0) : dirCopy(1);
	}
	if (f0 && f1) {
		sCard.fat = ((s16)(fatCopy(0)->checkCode - fatCopy(1)->checkCode) >= 0) ? fatCopy(0) : fatCopy(1);
	} else {
		sCard.fat = f0 ? fatCopy(0) : fatCopy(1);
	}
	return 1;
}

static int countFreeEntries()
{
	int n = 0;
	for (int i = 0; i < CARD_MAX_FILE; i++) {
		if (sCard.dir->entries[i].gameName[0] == 0xFF) {
			n++;
		}
	}
	return n;
}

// Up to build 51 the card was in the PIKMIN folder with the game data (usually on the USB drive). The first time
// there is no card in the save folder, a copy of that one is made (the old one is left where it is).
static void copyOldCard()
{
	const char* places[3] = { ps3b_usb_folder(), ps3b_data_root(), "/dev_hdd0/PIKMIN" };
	for (int i = 0; i < 3; i++) {
		char old[256];
		if (!places[i]) {
			continue;
		}
		snprintf(old, sizeof(old), "%s/%s", places[i], VCARD_FILE);
		if (strcmp(old, sCard.path) == 0 || !ps3b_file_exists(old)) {
			continue;
		}
		if (ps3b_file_copy(old, sCard.path)) {
			ps3Log("[card] copied the memory card from %s (left there as it was; no longer used)\n", old);
		} else {
			ps3Log("[card] FAILED to copy the memory card from %s\n", old);
		}
		return;
	}
}

// Loads the image (once), creating a freshly formatted card if there is none.
static s32 loadCard()
{
	if (sCard.loaded) {
		return sCard.broken ? CARD_RESULT_BROKEN : CARD_RESULT_READY;
	}
	const char* dir = ps3b_save_dir();
	if (!dir) {
		return CARD_RESULT_NOCARD;
	}
	snprintf(sCard.path, sizeof(sCard.path), "%s/%s", dir, VCARD_FILE);
	if (!ps3b_file_exists(sCard.path)) {
		copyOldCard();
	}

	char* data        = NULL;
	unsigned int size = 0;
	if (ps3b_file_read_all(sCard.path, &data, &size)) {
		CARDID* id     = (CARDID*)data;
		u32 expected   = (size >= sizeof(CARDID)) ? (u32)id->size * 1024 * 1024 / 8 : 0;
		if (expected >= 0x80000 && expected <= 0x1000000 && size >= expected) {
			sCard.size   = expected;
			sCard.cBlock = (u16)(expected / VCARD_SECTOR);
			sCard.image  = (u8*)data;
			sCard.loaded = 1;
			if (!selectCurrent()) {
				sCard.broken = 1;
				ps3Log("[card] %s is damaged; the game will offer to format it\n", sCard.path);
				return CARD_RESULT_BROKEN;
			}
			ps3Log("[card] memory card: %s (%d blocks free)\n", sCard.path, (int)sCard.fat->freeBlocks);
			return CARD_RESULT_READY;
		}
		ps3Log("[card] %s is not a memory card image (%u bytes); making a new one\n", sCard.path, size);
		ps3b_free(data);
	}

	sCard.size   = VCARD_BLOCKS * VCARD_SECTOR;
	sCard.cBlock = VCARD_BLOCKS;
	sCard.image  = (u8*)ps3b_memalign(32, sCard.size);
	if (!sCard.image) {
		return CARD_RESULT_FATAL_ERROR;
	}
	sCard.loaded = 1;
	formatImage();
	if (!writeBlocks(0, sCard.cBlock)) {
		return CARD_RESULT_IOERROR;
	}
	ps3Log("[card] new memory card (251 blocks): %s\n", sCard.path);
	return CARD_RESULT_READY;
}

// ------------------------------------------------------------------ helpers

static s32 finish(s32 result)
{
	sCard.result = result;
	return result;
}

struct PendingCallback {
	CARDCallback cb;
	s32 chan;
	s32 result;
};

static void runCallback(void* arg)
{
	PendingCallback* p = (PendingCallback*)arg;
	p->cb(p->chan, p->result);
}

// Async calls: the work is done; the callback runs like the GameCube's completion interrupt.
static s32 finishAsync(s32 chan, s32 result, CARDCallback cb)
{
	finish(result);
	if (cb) {
		static PendingCallback sPending[8];
		static int sNext;
		PendingCallback* p = &sPending[sNext++ & 7];
		p->cb              = cb;
		p->chan            = chan;
		p->result          = result;
		ps3PostInterrupt(runCallback, p);
	}
	return CARD_RESULT_READY;
}

static s32 ready(s32 chan)
{
	if (chan != 0) {
		return CARD_RESULT_NOCARD;
	}
	if (!sCard.mounted) {
		return CARD_RESULT_NOCARD;
	}
	if (sCard.broken) {
		return CARD_RESULT_BROKEN;
	}
	return CARD_RESULT_READY;
}

static const DVDDiskID* diskId() { return DVDGetCurrentDiskID(); }

static s32 access(CARDDir* e)
{
	if (e->gameName[0] == 0xFF) {
		return CARD_RESULT_NOFILE;
	}
	if (memcmp(e->gameName, diskId()->gameName, 4) == 0 && memcmp(e->company, diskId()->company, 2) == 0) {
		return CARD_RESULT_READY;
	}
	return CARD_RESULT_NOPERM;
}

static s32 isPublic(CARDDir* e)
{
	if (e->gameName[0] == 0xFF) {
		return CARD_RESULT_NOFILE;
	}
	return (e->permission & CARD_ATTR_PUBLIC) ? CARD_RESULT_READY : CARD_RESULT_NOPERM;
}

static BOOL sameName(CARDDir* e, const char* name)
{
	const char* n = (const char*)e->fileName;
	for (int i = 0; i < CARD_FILENAME_MAX; i++) {
		if (n[i] != name[i]) {
			return FALSE;
		}
		if (name[i] == 0) {
			return TRUE;
		}
	}
	return name[CARD_FILENAME_MAX] == 0;
}

static u32 nowSeconds() { return (u32)OSTicksToSeconds(OSGetTime()); }

// Block number holding byte `offset` of a file starting at `start` (follows the allocation chain).
static int blockAt(u16 start, u32 offset)
{
	u16 b = start;
	for (u32 n = offset / VCARD_SECTOR; n > 0; n--) {
		if (!CARDIsValidBlockNo(&sCard, b)) {
			return -1;
		}
		b = fatMap()[b];
	}
	return CARDIsValidBlockNo(&sCard, b) ? b : -1;
}

static void updateIconOffsets(CARDDir* ent, CARDStat* st)
{
	u32 offset = ent->iconAddr;
	if (offset == 0xFFFFFFFF) {
		st->bannerFormat = 0;
		st->iconFormat   = 0;
		st->iconSpeed    = 0;
		offset           = 0;
	}
	BOOL iconTlut = FALSE;
	switch (CARDGetBannerFormat(ent)) {
	case CARD_STAT_BANNER_C8:
		st->offsetBanner = offset;
		offset += CARD_BANNER_WIDTH * CARD_BANNER_HEIGHT;
		st->offsetBannerTlut = offset;
		offset += 2 * 256;
		break;
	case CARD_STAT_BANNER_RGB5A3:
		st->offsetBanner = offset;
		offset += 2 * CARD_BANNER_WIDTH * CARD_BANNER_HEIGHT;
		st->offsetBannerTlut = 0xFFFFFFFF;
		break;
	default:
		st->offsetBanner     = 0xFFFFFFFF;
		st->offsetBannerTlut = 0xFFFFFFFF;
		break;
	}
	for (int i = 0; i < CARD_ICON_MAX; i++) {
		switch (CARDGetIconFormat(ent, i)) {
		case CARD_STAT_ICON_C8:
			st->offsetIcon[i] = offset;
			offset += CARD_ICON_WIDTH * CARD_ICON_HEIGHT;
			iconTlut = TRUE;
			break;
		case CARD_STAT_ICON_RGB5A3:
			st->offsetIcon[i] = offset;
			offset += 2 * CARD_ICON_WIDTH * CARD_ICON_HEIGHT;
			break;
		default:
			st->offsetIcon[i] = 0xFFFFFFFF;
			break;
		}
	}
	if (iconTlut) {
		st->offsetIconTlut = offset;
		offset += 2 * 256;
	} else {
		st->offsetIconTlut = 0xFFFFFFFF;
	}
	st->offsetData = offset;
}

// ------------------------------------------------------------------ mount, check, format

void CARDInit(void) { }

// Called at boot, before the game starts: reading the 2 MB image when the title screen first mounts the
// card held up the game (and its sound) for a moment.
extern "C" void ps3CardPreload(void)
{
	loadCard();
}

BOOL CARDProbe(s32 chan)
{
	ps3DeliverInterrupts();
	return chan == 0 && ps3b_save_dir() != NULL;
}

s32 CARDProbeEx(s32 chan, s32* memSize, s32* sectorSize)
{
	if (chan != 0 || !ps3b_save_dir()) {
		return CARD_RESULT_NOCARD;
	}
	if (memSize) {
		*memSize = VCARD_SIZE_MBITS;
	}
	if (sectorSize) {
		*sectorSize = VCARD_SECTOR;
	}
	return CARD_RESULT_READY;
}

static s32 mount(s32 chan)
{
	PS3_ACTIVITY("CARDMount");
	if (chan != 0) {
		return CARD_RESULT_NOCARD;
	}
	s32 r = loadCard();
	if (r == CARD_RESULT_READY || r == CARD_RESULT_BROKEN) {
		sCard.mounted = 1;
	}
	return r;
}

s32 CARDMount(s32 chan, CARDMemoryCard* work, CARDCallback detach)
{
	(void)work;
	(void)detach;
	return finish(mount(chan));
}

s32 CARDMountAsync(s32 chan, CARDMemoryCard* work, CARDCallback detach, CARDCallback attach)
{
	(void)work;
	(void)detach;
	if (chan != 0) {
		return CARD_RESULT_NOCARD;
	}
	return finishAsync(chan, mount(chan), attach);
}

s32 CARDUnmount(s32 chan)
{
	if (chan != 0) {
		return CARD_RESULT_NOCARD;
	}
	sCard.mounted = 0;
	return CARD_RESULT_READY;
}

s32 CARDCheck(s32 chan) { return finish(ready(chan)); }

s32 CARDCheckAsync(s32 chan, CARDCallback cb)
{
	s32 r = ready(chan);
	if (r == CARD_RESULT_NOCARD) {
		return r;
	}
	return finishAsync(chan, r, cb);
}

s32 CARDCheckExAsync(s32 chan, s32* xferBytes, CARDCallback cb)
{
	if (xferBytes) {
		*xferBytes = 0;
	}
	return CARDCheckAsync(chan, cb);
}

static s32 format(s32 chan)
{
	if (chan != 0 || !sCard.mounted) {
		return CARD_RESULT_NOCARD;
	}
	formatImage();
	if (!writeBlocks(0, sCard.cBlock)) {
		return CARD_RESULT_IOERROR;
	}
	ps3Log("[card] memory card formatted\n");
	return CARD_RESULT_READY;
}

s32 CARDFormat(s32 chan) { return finish(format(chan)); }

s32 CARDFormatAsync(s32 chan, CARDCallback cb)
{
	if (chan != 0 || !sCard.mounted) {
		return CARD_RESULT_NOCARD;
	}
	return finishAsync(chan, format(chan), cb);
}

s32 CARDGetResultCode(s32 chan)
{
	ps3DeliverInterrupts();
	return chan == 0 ? sCard.result : CARD_RESULT_NOCARD;
}

s32 CARDGetXferredBytes(s32 chan) { return chan == 0 ? sCard.xferred : 0; }

s32 CARDFreeBlocks(s32 chan, s32* byteNotUsed, s32* filesNotUsed)
{
	s32 r = ready(chan);
	if (r < 0) {
		return r;
	}
	if (byteNotUsed) {
		*byteNotUsed = (s32)sCard.fat->freeBlocks * VCARD_SECTOR;
	}
	if (filesNotUsed) {
		*filesNotUsed = countFreeEntries();
	}
	return CARD_RESULT_READY;
}

s32 CARDGetSectorSize(s32 chan, u32* size)
{
	s32 r = ready(chan);
	if (r < 0) {
		return r;
	}
	*size = VCARD_SECTOR;
	return CARD_RESULT_READY;
}

s32 CARDGetSerialNo(s32 chan, u64* serialNo)
{
	s32 r = ready(chan);
	if (r < 0) {
		return r;
	}
	u64 code = 0;
	for (int i = 0; i < 4; i++) {
		u64 part;
		memcpy(&part, &header()->serial[i * 8], 8);
		code ^= part;
	}
	*serialNo = code;
	return CARD_RESULT_READY;
}

// ------------------------------------------------------------------ directory

s32 CARDGetStatus(s32 chan, s32 fileNo, CARDStat* st)
{
	if (fileNo < 0 || fileNo >= CARD_MAX_FILE) {
		return CARD_RESULT_FATAL_ERROR;
	}
	s32 r = ready(chan);
	if (r < 0) {
		return r;
	}
	CARDDir* ent = &sCard.dir->entries[fileNo];
	r            = access(ent);
	if (r == CARD_RESULT_NOPERM) {
		r = isPublic(ent);
	}
	if (r >= 0) {
		memcpy(st->gameName, ent->gameName, 4);
		memcpy(st->company, ent->company, 2);
		st->length = (u32)ent->length * VCARD_SECTOR;
		memcpy(st->fileName, ent->fileName, CARD_FILENAME_MAX);
		st->time         = ent->time;
		st->bannerFormat = ent->bannerFormat;
		st->iconAddr     = ent->iconAddr;
		st->iconFormat   = ent->iconFormat;
		st->iconSpeed    = ent->iconSpeed;
		st->commentAddr  = ent->commentAddr;
		updateIconOffsets(ent, st);
	}
	return r;
}

s32 CARDSetStatus(s32 chan, s32 fileNo, CARDStat* st)
{
	if (fileNo < 0 || fileNo >= CARD_MAX_FILE || (st->iconAddr != 0xFFFFFFFF && st->iconAddr >= CARD_READ_SIZE)
	    || (st->commentAddr != 0xFFFFFFFF && st->commentAddr % CARD_SYSTEM_BLOCK_SIZE > CARD_SYSTEM_BLOCK_SIZE - CARD_COMMENT_SIZE)) {
		return CARD_RESULT_FATAL_ERROR;
	}
	s32 r = ready(chan);
	if (r < 0) {
		return finish(r);
	}
	CARDDir* ent = &sCard.dir->entries[fileNo];
	r            = access(ent);
	if (r < 0) {
		return finish(r);
	}
	ent->bannerFormat = st->bannerFormat;
	ent->iconAddr     = st->iconAddr;
	ent->iconFormat   = st->iconFormat;
	ent->iconSpeed    = st->iconSpeed;
	ent->commentAddr  = st->commentAddr;
	updateIconOffsets(ent, st);
	if (ent->iconAddr == 0xFFFFFFFF) {
		CARDSetIconSpeed(ent, 0, CARD_STAT_SPEED_FAST);
	}
	ent->time = nowSeconds();
	return finish(commitDir() ? CARD_RESULT_READY : CARD_RESULT_IOERROR);
}

s32 CARDSetStatusAsync(s32 chan, s32 fileNo, CARDStat* st, CARDCallback cb)
{
	return finishAsync(chan, CARDSetStatus(chan, fileNo, st), cb);
}

static s32 openEntry(s32 chan, s32 fileNo, CARDFileInfo* info)
{
	CARDDir* ent = &sCard.dir->entries[fileNo];
	info->chan   = chan;
	info->fileNo = fileNo;
	info->offset = 0;
	info->length = (s32)ent->length * VCARD_SECTOR;
	info->iBlock = ent->startBlock;
	return CARD_RESULT_READY;
}

s32 CARDOpen(s32 chan, const char* name, CARDFileInfo* info)
{
	info->chan = -1;
	s32 r      = ready(chan);
	if (r < 0) {
		return r;
	}
	for (int i = 0; i < CARD_MAX_FILE; i++) {
		CARDDir* ent = &sCard.dir->entries[i];
		if (access(ent) == CARD_RESULT_READY && sameName(ent, name)) {
			return openEntry(chan, i, info);
		}
	}
	return CARD_RESULT_NOFILE;
}

s32 CARDFastOpen(s32 chan, s32 fileNo, CARDFileInfo* info)
{
	if (fileNo < 0 || fileNo >= CARD_MAX_FILE) {
		return CARD_RESULT_FATAL_ERROR;
	}
	info->chan = -1;
	s32 r      = ready(chan);
	if (r < 0) {
		return r;
	}
	CARDDir* ent = &sCard.dir->entries[fileNo];
	r            = access(ent);
	if (r == CARD_RESULT_NOPERM) {
		r = isPublic(ent);
	}
	if (r < 0) {
		return r;
	}
	return openEntry(chan, fileNo, info);
}

s32 CARDClose(CARDFileInfo* info)
{
	if (info->chan != 0) {
		return CARD_RESULT_NOCARD;
	}
	info->chan = -1;
	return CARD_RESULT_READY;
}

s32 CARDCreate(s32 chan, const char* name, u32 size, CARDFileInfo* info)
{
	PS3_ACTIVITY("CARDCreate");
	if (strlen(name) > CARD_FILENAME_MAX) {
		return finish(CARD_RESULT_NAMETOOLONG);
	}
	s32 r = ready(chan);
	if (r < 0) {
		return finish(r);
	}
	if (size == 0 || size % VCARD_SECTOR != 0) {
		return finish(CARD_RESULT_FATAL_ERROR);
	}
	int freeNo = -1;
	for (int i = 0; i < CARD_MAX_FILE; i++) {
		CARDDir* ent = &sCard.dir->entries[i];
		if (ent->gameName[0] == 0xFF) {
			if (freeNo < 0) {
				freeNo = i;
			}
		} else if (access(ent) == CARD_RESULT_READY && sameName(ent, name)) {
			return finish(CARD_RESULT_EXIST);
		}
	}
	if (freeNo < 0) {
		return finish(CARD_RESULT_NOENT);
	}
	u32 count = size / VCARD_SECTOR;
	if (sCard.fat->freeBlocks < count) {
		return finish(CARD_RESULT_INSSPACE);
	}

	// Allocate as __CARDAllocBlock does: free blocks after the last allocated one, chained.
	u16* map   = fatMap();
	u16 start  = 0xFFFF, prev = 0, block = sCard.fat->lastAllocBlock;
	u32 left   = count;
	int guard  = 0;
	while (left > 0) {
		if (++guard > sCard.cBlock) {
			return finish(CARD_RESULT_BROKEN);
		}
		block++;
		if (!CARDIsValidBlockNo(&sCard, block)) {
			block = CARD_NUM_SYSTEM_BLOCK;
		}
		if (map[block] == 0) {
			if (start == 0xFFFF) {
				start = block;
			} else {
				map[prev] = block;
			}
			prev       = block;
			map[block] = 0xFFFF;
			left--;
		}
	}
	sCard.fat->freeBlocks -= (u16)count;
	sCard.fat->lastAllocBlock = block;

	CARDDir* ent = &sCard.dir->entries[freeNo];
	memset(ent, 0xFF, sizeof(*ent));
	memcpy(ent->gameName, diskId()->gameName, 4);
	memcpy(ent->company, diskId()->company, 2);
	memset(ent->fileName, 0, CARD_FILENAME_MAX);
	strncpy((char*)ent->fileName, name, CARD_FILENAME_MAX);
	ent->permission   = CARD_ATTR_PUBLIC;
	ent->copyTimes    = 0;
	ent->startBlock   = start;
	ent->length       = (u16)count;
	ent->bannerFormat = CARD_STAT_BANNER_NONE;
	ent->iconAddr     = 0xFFFFFFFF;
	ent->iconFormat   = CARD_STAT_ICON_NONE;
	ent->iconSpeed    = CARD_STAT_SPEED_END;
	ent->commentAddr  = 0xFFFFFFFF;
	CARDSetIconSpeed(ent, 0, CARD_STAT_SPEED_FAST);
	ent->time = nowSeconds();

	if (!commitFat() || !commitDir()) {
		return finish(CARD_RESULT_IOERROR);
	}
	openEntry(chan, freeNo, info);
	ps3Log("[card] created \"%s\" (%u blocks)\n", name, (unsigned)count);
	return finish(CARD_RESULT_READY);
}

s32 CARDCreateAsync(s32 chan, const char* name, u32 size, CARDFileInfo* info, CARDCallback cb)
{
	return finishAsync(chan, CARDCreate(chan, name, size, info), cb);
}

static s32 freeChain(u16 start)
{
	u16* map = fatMap();
	u16 b    = start;
	int guard = 0;
	while (b != 0xFFFF) {
		if (!CARDIsValidBlockNo(&sCard, b) || ++guard > sCard.cBlock) {
			return CARD_RESULT_BROKEN;
		}
		u16 next = map[b];
		map[b]   = 0;
		sCard.fat->freeBlocks++;
		b = next;
	}
	return CARD_RESULT_READY;
}

s32 CARDFastDelete(s32 chan, s32 fileNo)
{
	if (fileNo < 0 || fileNo >= CARD_MAX_FILE) {
		return CARD_RESULT_FATAL_ERROR;
	}
	s32 r = ready(chan);
	if (r < 0) {
		return finish(r);
	}
	CARDDir* ent = &sCard.dir->entries[fileNo];
	r            = access(ent);
	if (r < 0) {
		return finish(r);
	}
	char name[CARD_FILENAME_MAX + 1];
	memcpy(name, ent->fileName, CARD_FILENAME_MAX);
	name[CARD_FILENAME_MAX] = 0;
	u16 start               = ent->startBlock;
	memset(ent, 0xFF, sizeof(*ent));
	if (!commitDir()) {
		return finish(CARD_RESULT_IOERROR);
	}
	r = freeChain(start);
	if (!commitFat()) {
		return finish(CARD_RESULT_IOERROR);
	}
	ps3Log("[card] deleted \"%s\"\n", name);
	return finish(r);
}

s32 CARDFastDeleteAsync(s32 chan, s32 fileNo, CARDCallback cb) { return finishAsync(chan, CARDFastDelete(chan, fileNo), cb); }

s32 CARDRename(s32 chan, const char* oldName, const char* newName)
{
	if (oldName[0] == 0xFF || newName[0] == 0xFF || oldName[0] == 0 || newName[0] == 0) {
		return CARD_RESULT_FATAL_ERROR;
	}
	if (strlen(oldName) > CARD_FILENAME_MAX || strlen(newName) > CARD_FILENAME_MAX) {
		return CARD_RESULT_NAMETOOLONG;
	}
	s32 r = ready(chan);
	if (r < 0) {
		return finish(r);
	}
	int oldNo = -1, newNo = -1;
	for (int i = 0; i < CARD_MAX_FILE; i++) {
		CARDDir* ent = &sCard.dir->entries[i];
		if (ent->gameName[0] == 0xFF || access(ent) != CARD_RESULT_READY) {
			continue;
		}
		if (sameName(ent, oldName)) {
			oldNo = i;
		}
		if (sameName(ent, newName)) {
			newNo = i;
		}
	}
	if (oldNo < 0) {
		return finish(CARD_RESULT_NOFILE);
	}
	if (newNo >= 0) {
		return finish(CARD_RESULT_EXIST);
	}
	CARDDir* ent = &sCard.dir->entries[oldNo];
	memset(ent->fileName, 0, CARD_FILENAME_MAX);
	strncpy((char*)ent->fileName, newName, CARD_FILENAME_MAX);
	ent->time = nowSeconds();
	return finish(commitDir() ? CARD_RESULT_READY : CARD_RESULT_IOERROR);
}

s32 CARDRenameAsync(s32 chan, const char* oldName, const char* newName, CARDCallback cb)
{
	return finishAsync(chan, CARDRename(chan, oldName, newName), cb);
}

// ------------------------------------------------------------------ data

static s32 checkFile(CARDFileInfo* info, s32 length, s32 offset, CARDDir** outEnt)
{
	if (info->chan != 0 || info->fileNo < 0 || info->fileNo >= CARD_MAX_FILE) {
		return CARD_RESULT_FATAL_ERROR;
	}
	s32 r = ready(info->chan);
	if (r < 0) {
		return r;
	}
	CARDDir* ent = &sCard.dir->entries[info->fileNo];
	r            = access(ent);
	if (r == CARD_RESULT_NOPERM) {
		r = isPublic(ent);
	}
	if (r < 0) {
		return r;
	}
	u32 fileSize = (u32)ent->length * VCARD_SECTOR;
	if (offset < 0 || length < 0 || (u32)offset >= fileSize || (u32)offset + (u32)length > fileSize) {
		return CARD_RESULT_LIMIT;
	}
	*outEnt = ent;
	return CARD_RESULT_READY;
}

s32 CARDRead(CARDFileInfo* info, void* buf, s32 length, s32 offset)
{
	PS3_ACTIVITY("CARDRead");
	if (offset % CARD_SEG_SIZE != 0 || length % CARD_SEG_SIZE != 0) {
		return finish(CARD_RESULT_FATAL_ERROR);
	}
	CARDDir* ent;
	s32 r = checkFile(info, length, offset, &ent);
	if (r < 0) {
		return finish(r);
	}
	u8* dst = (u8*)buf;
	while (length > 0) {
		int b = blockAt(ent->startBlock, (u32)offset);
		if (b < 0) {
			return finish(CARD_RESULT_BROKEN);
		}
		s32 inBlock = offset % VCARD_SECTOR;
		s32 n       = VCARD_SECTOR - inBlock;
		if (n > length) {
			n = length;
		}
		memcpy(dst, sCard.image + b * VCARD_SECTOR + inBlock, n);
		dst += n;
		offset += n;
		length -= n;
		sCard.xferred += n;
	}
	info->offset = offset;
	return finish(CARD_RESULT_READY);
}

s32 CARDReadAsync(CARDFileInfo* info, void* buf, s32 length, s32 offset, CARDCallback cb)
{
	return finishAsync(info->chan, CARDRead(info, buf, length, offset), cb);
}

s32 CARDWrite(CARDFileInfo* info, void* buf, s32 length, s32 offset)
{
	PS3_ACTIVITY("CARDWrite");
	if (offset % VCARD_SECTOR != 0 || length % VCARD_SECTOR != 0) {
		return finish(CARD_RESULT_FATAL_ERROR);
	}
	CARDDir* ent;
	s32 r = checkFile(info, length, offset, &ent);
	if (r < 0) {
		return finish(r);
	}
	const u8* src = (const u8*)buf;
	while (length > 0) {
		int b = blockAt(ent->startBlock, (u32)offset);
		if (b < 0) {
			return finish(CARD_RESULT_BROKEN);
		}
		memcpy(sCard.image + b * VCARD_SECTOR, src, VCARD_SECTOR);
		if (!writeBlocks(b, 1)) {
			return finish(CARD_RESULT_IOERROR);
		}
		src += VCARD_SECTOR;
		offset += VCARD_SECTOR;
		length -= VCARD_SECTOR;
		sCard.xferred += VCARD_SECTOR;
	}
	info->offset = offset;
	ent->time    = nowSeconds();
	if (!commitDir()) {
		return finish(CARD_RESULT_IOERROR);
	}
	static int sLogged;
	ps3LogOnce(&sLogged, "[card] saving works (first write to \"%.32s\")\n", (const char*)ent->fileName);
	return finish(CARD_RESULT_READY);
}

s32 CARDWriteAsync(CARDFileInfo* info, void* buf, s32 length, s32 offset, CARDCallback cb)
{
	return finishAsync(info->chan, CARDWrite(info, buf, length, offset), cb);
}
