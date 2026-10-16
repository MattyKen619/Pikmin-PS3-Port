# PS3 port: host (x86-64 Linux) build of Nintendo's GX library (src/gx) and the port's graphics command
# processor (platform/ps3/gpu), for testing them without a console.
#
#   make -C tools/ps3/gx_host          builds and runs the self-test
#
# The GX library and the platform code are compiled exactly as for the PS3 (C++, the game's headers,
# TARGET_PS3); host_os.cpp stands in for the Dolphin OS and PS3 services they use.

ROOT     := ../../..
BUILD    := $(ROOT)/build/gx_host
DEFS     := -DBUILD_VERSION=5 -DVERSION_GPIE01_01 -DDTK_CONFIG_NONMATCHING=1 -DNDEBUG=1 -DTARGET_PS3=1
GAMEINC  := -nostdinc -isystem $(ROOT)/include/stl -I$(ROOT)/include -I$(ROOT)/platform/ps3/include
CXXFLAGS := -std=gnu++14 -O1 -g -funsigned-char -fno-exceptions -fno-rtti -w $(DEFS)

GX_SRC   := $(filter-out $(ROOT)/src/gx/GXPerf.c,$(wildcard $(ROOT)/src/gx/*.c))
GPU_SRC  := $(wildcard $(ROOT)/platform/ps3/gpu/*.cpp)
OBJS     := $(patsubst $(ROOT)/%,$(BUILD)/%.o,$(GX_SRC) $(GPU_SRC)) $(BUILD)/host_os.o $(BUILD)/gx_selftest.o

all: $(BUILD)/gx_selftest $(BUILD)/gx_replay
	$(BUILD)/gx_selftest

# Replays frame traces recorded on the PS3 (needs only the command processor)
$(BUILD)/gx_replay: gx_replay.cpp gx_trace_file.h $(ROOT)/platform/ps3/gpu/gpu_cp.cpp $(ROOT)/platform/ps3/gpu/gpu_cp.h
	@mkdir -p $(BUILD)
	g++ -std=gnu++14 -O2 -g -Wall -o $@ gx_replay.cpp $(ROOT)/platform/ps3/gpu/gpu_cp.cpp

$(BUILD)/gx_selftest: $(OBJS)
	g++ -o $@ $(OBJS) -lpthread

$(BUILD)/src/gx/%.c.o: $(ROOT)/src/gx/%.c
	@mkdir -p $(dir $@)
	g++ $(CXXFLAGS) -fpermissive $(GAMEINC) -x c++ -c $< -o $@

$(BUILD)/platform/ps3/gpu/%.cpp.o: $(ROOT)/platform/ps3/gpu/%.cpp
	@mkdir -p $(dir $@)
	g++ $(CXXFLAGS) $(GAMEINC) -c $< -o $@

# The stand-ins and the test use the host's C library, with the game's headers for the GX API.
$(BUILD)/%.o: %.cpp
	@mkdir -p $(dir $@)
	g++ $(CXXFLAGS) -I$(ROOT)/include -I$(ROOT)/platform/ps3/include -c $< -o $@

clean:
	rm -rf $(BUILD)

.PHONY: all clean
