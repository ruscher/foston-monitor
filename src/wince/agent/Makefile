# bfagent: BigFoston agent and Dual Boot screen for Windows CE 5 (FOSTON FS-707DC, MT3351).
#   make            -> build/bfagent.exe (CeGCC arm-mingw32ce, see ../../../scripts/env.sh)
#   make host-test  -> build/host_bfp (protocol check against tools/bigfoston/bfp.py)
#   make preview    -> build/preview-shots/*.png (the Dual Boot screen on the PC, via winegcc)
#   make imports    -> checks every coredll import against the device's coredll exports

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

build/bfagent.exe: bfagent.c bfp.c bfp.h boot.c boot.h sha256.c sha256.h ui.c ui.h art.h
	mkdir -p build
	$(CC) $(CFLAGS) $(ARCH_FLAGS) $(DEFS) -o $@ bfagent.c bfp.c boot.c sha256.c ui.c $(LIBS)
	$(STRIP) $@
	cd build && sha256sum bfagent.exe > bfagent.exe.sha256

build/host_bfp: host_bfp.c bfp.c bfp.h
	mkdir -p build
	cc -O2 -Wall -Wextra -std=c99 -o $@ host_bfp.c bfp.c

host-test: build/host_bfp

# the device silently refuses to start an exe that imports a missing function: compare the
# imports with the exports of the coredll.dll from the NAND backup
COREDLL := $(TOP)/backup/wince-referencia/rom/regiao1-8c003000/mod/coredll.dll
imports: build/bfagent.exe
	python3 tools/check_imports.py $(CROSS)objdump build/bfagent.exe $(COREDLL)

# The Dual Boot screen on the PC (winegcc), without the device: build/preview-shots/*.png.
# Uses the Tahoma from the FOSTON's ROM (backup/wince-referencia) in a wine prefix of its own.
PREVIEW_PREFIX ?= $(CURDIR)/build/wineprefix
ROMFONTS := $(TOP)/backup/wince-referencia/rom/regiao2-8c705000/files
build/preview.exe: preview.c ui.c ui.h art.h
	mkdir -p build
	winegcc -O2 -Wall -Wextra -Wno-unused-parameter -DUNICODE -D_UNICODE -mno-cygwin -o build/preview preview.c ui.c -lgdi32

preview: build/preview.exe
	@test -d $(PREVIEW_PREFIX) || WINEPREFIX=$(PREVIEW_PREFIX) WINEDEBUG=-all wineboot -i >/dev/null 2>&1
	@mkdir -p $(PREVIEW_PREFIX)/drive_c/windows/Fonts build/preview-shots
	@cp -f $(ROMFONTS)/tahoma.ttf $(ROMFONTS)/arial.ttf $(PREVIEW_PREFIX)/drive_c/windows/Fonts/
	BF_PREVIEW_DIR="$$(WINEPREFIX=$(PREVIEW_PREFIX) WINEDEBUG=-all winepath -w $(CURDIR)/build/preview-shots 2>/dev/null)" \
		WINEPREFIX=$(PREVIEW_PREFIX) WINEDEBUG=-all ./build/preview.exe >/dev/null 2>&1
	python3 tools/preview.py build/preview-shots

art.h: tools/make_art.py
	python3 tools/make_art.py

clean:
	rm -rf build

.PHONY: all clean host-test preview imports
