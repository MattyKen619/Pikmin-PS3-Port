# tools/ps3/dsp_test: checks the sound mixer on the host (see dsp_test.cpp).
#
#   make -C tools/ps3/dsp_test && tools/ps3/dsp_test/dsp_test [sessions] [frames]
#   make -C tools/ps3/dsp_test OLD=/path/to/an/earlier/dsp_hle.cpp   (also compare with an earlier PPU-only mixer)

ROOT     := ../../..
CXX      ?= g++
CXXFLAGS := -O2 -g -std=gnu++14 -Wall -Wno-unused-function -I$(ROOT)/platform/ps3/include -I$(ROOT)/platform/ps3/sdk
BUILD    := build
DEPS     := $(ROOT)/platform/ps3/sdk/dsp_mix.h $(ROOT)/platform/ps3/include/ps3/dsp_shared.h
OBJS     := $(BUILD)/dsp_test.o $(BUILD)/dsp_spu_host.o

ifneq ($(OLD),)
OBJS     += $(BUILD)/old/sdk/dsp_hle_old.o
CXXFLAGS += -DWITH_OLD
endif

.PHONY: all clean
all: dsp_test

dsp_test: $(OBJS)
	$(CXX) -o $@ $^

$(BUILD)/dsp_test.o: dsp_test.cpp $(DEPS) Makefile
	@mkdir -p $(BUILD)
	$(CXX) $(CXXFLAGS) -c $< -o $@

# the SPU program, its DMA emulated (shim/)
$(BUILD)/dsp_spu_host.o: $(ROOT)/platform/ps3/spu/dsp_spu.cpp $(DEPS) shim/spu_mfcio.h shim/spu_intrinsics.h shim/sys/spu_event.h
	@mkdir -p $(BUILD)
	$(CXX) $(CXXFLAGS) -DDSP_SPU_HOST_TEST -Ishim -c $< -o $@

# an earlier mixer (OLD=...): its SDK calls stand-ins in oldshim/, its functions renamed
OLD_RENAME := -Dps3DspSetup=old_ps3DspSetup -Dps3DspStartFrame=old_ps3DspStartFrame -Dps3DspRenderSubframe=old_ps3DspRenderSubframe \
              -Dps3DspNoteAramWrite=old_ps3DspNoteAramWrite -Dps3DspVoicesReadingAram=old_ps3DspVoicesReadingAram -Dps3DspStatus=old_ps3DspStatus
$(BUILD)/old/sdk/dsp_hle_old.o: $(OLD) oldshim/sdk_internal.h oldshim/backend/ps3_backend.h Makefile
	@mkdir -p $(BUILD)/old/sdk $(BUILD)/old/backend $(BUILD)/old/jaudio
	cp $(OLD) $(BUILD)/old/sdk/dsp_hle_old.cpp
	cp oldshim/backend/ps3_backend.h $(BUILD)/old/backend/
	sed -e '/audiostruct.h/d' -e 's|#include "types.h"|#include "sdk_internal.h"\ntypedef struct Wave_ Wave_;|' $(ROOT)/include/jaudio/dspinterface.h > $(BUILD)/old/jaudio/dspinterface.h
	$(CXX) -O2 -g -std=gnu++14 -w -Ioldshim -I$(BUILD)/old $(OLD_RENAME) -c $(BUILD)/old/sdk/dsp_hle_old.cpp -o $@

clean:
	rm -rf $(BUILD) dsp_test
