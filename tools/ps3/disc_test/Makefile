# PS3 port: host checks of reading the game straight from a disc image (build 52). Your own image is needed;
# never share it or anything read from it.
#
#   make -C tools/ps3/disc_test             builds disc_test (the reader), dvd_test (the DVD layer) and icon_test
#                                           (the XMB icon from the disc's banner, see icon_test.cpp), and
#                                           png_selftest (its PNG writer, on made-up pictures; needs zlib)
#   tools/ps3/disc_test/disc_test IMAGE > a.txt && python3 tools/ps3/disc_test/check.py IMAGE a.txt
#   tools/ps3/disc_test/dvd_test IMAGE > b.txt && python3 tools/ps3/disc_test/check.py IMAGE b.txt
#
# dvd_test builds dvd.cpp as the PS3 build does, with the game's headers (`make dvd.ii` writes it and the shim
# preprocessed, dvd.ii and dvd_shim.ii, for building on a machine without the rest of the repository).

ROOT    := ../../..
DEFS    := -DBUILD_VERSION=5 -DVERSION_GPIE01_01 -DDTK_CONFIG_NONMATCHING=1 -DNDEBUG=1 -DTARGET_PS3=1
GAMEINC := -nostdinc -isystem $(ROOT)/include/stl -I$(ROOT)/include -I$(ROOT)/platform/ps3/include
GAMEFLAGS := -std=gnu++14 -O1 -g -funsigned-char -fno-exceptions -fno-rtti -w $(DEFS) $(GAMEINC)

all: disc_test dvd_test icon_test png_selftest

disc_test: disc_test.cpp $(ROOT)/platform/ps3/include/ps3/disc_image.h
	g++ -O2 -I$(ROOT)/platform/ps3/include -o $@ disc_test.cpp

icon_test: icon_test.cpp $(ROOT)/platform/ps3/include/ps3/banner_icon.h $(ROOT)/platform/ps3/include/ps3/disc_image.h
	g++ -O2 -I$(ROOT)/platform/ps3/include -o $@ icon_test.cpp

png_selftest: png_selftest.cpp $(ROOT)/platform/ps3/include/ps3/banner_icon.h
	g++ -O2 -I$(ROOT)/platform/ps3/include -o $@ png_selftest.cpp -lz

dvd.ii: $(ROOT)/platform/ps3/sdk/dvd.cpp dvd_shim.cpp $(ROOT)/platform/ps3/include/ps3/disc_image.h
	g++ $(GAMEFLAGS) -E -P $(ROOT)/platform/ps3/sdk/dvd.cpp > dvd.ii
	g++ $(GAMEFLAGS) -E -P dvd_shim.cpp > dvd_shim.ii

II_FLAGS := -std=gnu++14 -O1 -g -funsigned-char -fno-exceptions -fno-rtti -w -x c++

dvd_test: dvd_test.cpp dvd.ii
	g++ $(II_FLAGS) -c dvd.ii -o dvd_layer.o
	g++ $(II_FLAGS) -c dvd_shim.ii -o dvd_shim.o
	g++ -O2 -g -I$(ROOT)/platform/ps3/include -o $@ dvd_test.cpp dvd_layer.o dvd_shim.o

clean:
	rm -f disc_test dvd_test icon_test png_selftest dvd.ii dvd_shim.ii dvd_layer.o dvd_shim.o

.PHONY: all clean
