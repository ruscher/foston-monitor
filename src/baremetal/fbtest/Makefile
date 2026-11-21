# Bare-metal framebuffer test for the BigFoston loader (raw binary, load/entry at PA 0x8000).
TOP := $(abspath ../../..)
CROSS ?= $(TOP)/toolchain/opt/mingw32ce/bin/arm-mingw32ce-
export LD_LIBRARY_PATH := $(TOP)/toolchain/deps32/lib:$(LD_LIBRARY_PATH)

build/fbtest.bin: fbtest.S
	mkdir -p build
	$(CROSS)as -march=armv6 -o build/fbtest.o fbtest.S
	$(CROSS)objcopy -O binary -j .text build/fbtest.o $@
	$(CROSS)objdump -d build/fbtest.o > build/fbtest.lst

clean:
	rm -rf build
.PHONY: clean
