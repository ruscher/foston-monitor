# bfagent: BigFoston diagnostic agent for Windows CE 5 (FOSTON FS-707DC, MT3351).
#   make            -> build/bfagent.exe (CeGCC arm-mingw32ce, see ../../../scripts/env.sh)
#   make host-test  -> build/host_bfp (protocol check against tools/bigfoston/bfp.py)

TOP := $(abspath ../../..)
CROSS ?= $(TOP)/toolchain/opt/mingw32ce/bin/arm-mingw32ce-
export LD_LIBRARY_PATH := $(TOP)/toolchain/deps32/lib:$(LD_LIBRARY_PATH)
CC = $(CROSS)gcc
STRIP = $(CROSS)strip
CFLAGS ?= -O2 -Wall -Wextra -Wno-unused-parameter
ARCH_FLAGS ?= -march=armv5te -mtune=arm926ej-s
DEFS = -DUNICODE -D_UNICODE -D_WIN32_WCE=0x0500
LIBS = -lcoredll

all: build/bfagent.exe

build/bfagent.exe: bfagent.c bfp.c bfp.h boot.c boot.h sha256.c sha256.h
	mkdir -p build
	$(CC) $(CFLAGS) $(ARCH_FLAGS) $(DEFS) -o $@ bfagent.c bfp.c boot.c sha256.c $(LIBS)
	$(STRIP) $@
	cd build && sha256sum bfagent.exe > bfagent.exe.sha256

build/host_bfp: host_bfp.c bfp.c bfp.h
	mkdir -p build
	cc -O2 -Wall -Wextra -std=c99 -o $@ host_bfp.c bfp.c

host-test: build/host_bfp

clean:
	rm -rf build

.PHONY: all clean host-test
