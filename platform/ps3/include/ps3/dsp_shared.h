// PS3 port: the block of main memory that the PPU and the SPU sound mixer share (test build 44).
//
// The audio DSP's mixing (platform/ps3/sdk/dsp_mix.h) runs on an SPU (platform/ps3/spu/dsp_spu.cpp). The PPU
// side (platform/ps3/sdk/audio.cpp, dsp_hle.cpp) writes the commands into the first 256 bytes: the set-up
// (command 0x81), the frame (command 0x82), the number of sub-frames asked for (mail 0), and stop requests.
// The SPU reads them with DMA and writes back its progress (the number of sub-frames done, statistics) and the
// lines it would log, which the PPU prints.
//
// Plain C types only: this header is included by the game-side SDK code, the PSL1GHT backend, the SPU program
// and the host test.

#ifndef PS3_DSP_SHARED_H
#define PS3_DSP_SHARED_H

#ifdef __cplusplus
extern "C" {
#endif

enum {
	PS3_DSP_TRACKED_VOICES = 128, // voices with stop requests and start/stop log lines
	PS3_DSP_LOG_ENTRIES    = 64,
	PS3_DSP_EVENT_PORT     = 2, // the SPU's event port for "sub-frame done"
};

// A line the mixer logs. The PPU formats it (dsp_hle.cpp, ps3DspFormatLog), so the text is the same whichever
// processor mixes.
struct Ps3DspLogEntry {
	unsigned short kind;
	unsigned short voice;
	unsigned int a[7];
};

// The mixer's counters (dsp_mix.h). The SPU also counts the sub-frames it has finished, and its log lines.
struct Ps3DspStats {
	unsigned int subframes;   // sub-frames mixed
	unsigned int playing;     // voices processed in the last sub-frame
	unsigned int finished;    // voices that reached "done"
	unsigned int badVoices;   // voices stopped because their data was out of range
	unsigned int voicesMixed; // voices processed (all sub-frames)
	unsigned int done;        // (SPU) sub-frames finished: the PPU raises the DSP interrupt for each one
	unsigned int logWrite;    // (SPU) log entries written
	unsigned int logDropped;  // (SPU) log entries left out because the PPU had not read the earlier ones
	unsigned long long mixTicks;      // timebase ticks spent mixing
	unsigned long long sourceTicks;   // ... reading and resampling the voices' samples
	unsigned long long filterTicks;   // ... their filters
	unsigned long long mixVoiceTicks; // ... mixing them into the buses
};

// FXBuffer (jaudio/dspinterface.h) as the PPU's compiler lays it out (its pointer is 8 bytes there, 4 on the
// SPU): the SPU reads the reverb lines' parameters with these offsets.
struct Ps3DspFxLayout {
	unsigned short stride;   // sizeof(FXBuffer)
	unsigned short base;     // offset of circularBufferBase
	unsigned short baseSize; // its size (4 or 8)
	unsigned short dest;     // offset of dest[0] (dest[1] follows, 4 bytes each)
	unsigned short coeffs;   // offset of filterCoeffs[8]
	unsigned short pad;
};

struct Ps3DspShared {
	// ---- written by the PPU (bytes 0-127): the SPU reads them as one line, atomically
	unsigned int asked; // sub-frames asked for (mail 0)
	// command 0x81: changes when setupSeq does
	unsigned int setupSeq;
	unsigned int voiceCount, voices, tables, afcCoeffs, reverb;
	struct Ps3DspFxLayout fx;
	// command 0x82: changes when frameSeq does
	unsigned int frameSeq;
	unsigned int subframes, outputVolume, outL, outR;
	unsigned int aram, aramSize; // where the ARAM's emulation is
	unsigned int logRead;        // log entries the PPU has printed
	unsigned int pad0[14];
	// (bytes 128-255) per voice: ps3DspNoteAramWrite counts stops; the mixer stops a voice when its count
	// changes (its sound data in ARAM is being replaced)
	unsigned char stopSeq[PS3_DSP_TRACKED_VOICES];

	// ---- written by the SPU (from byte 256)
	struct Ps3DspStats spu;
	unsigned int pad1[16];
	struct Ps3DspLogEntry log[PS3_DSP_LOG_ENTRIES];
} __attribute__((aligned(128)));

#define PS3_DSP_STOPSEQ_OFFSET 128
#define PS3_DSP_STATS_OFFSET   256
#define PS3_DSP_LOG_OFFSET     384

#ifdef __cplusplus
}
#endif

#endif
