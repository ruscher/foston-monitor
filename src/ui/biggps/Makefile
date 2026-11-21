# Big GPS engine (map pack reader, rasterizer, renderer) and the bgview tool.
#   make                      host build (PC)
#   make CROSS=1              device build with the Buildroot toolchain
# SPDX-License-Identifier: GPL-3.0-or-later
ifdef CROSS
CC = ../../../firmware/build/out/host/bin/arm-buildroot-linux-musleabi-gcc
OUT = build/arm
LDFLAGS += -static
else
OUT = build/host
endif
CFLAGS ?= -O2 -g
CFLAGS += -std=c11 -D_GNU_SOURCE -Wall -Wextra -Wno-unused-parameter
LIB = bgmap.c raster.c bgrender.c bgroute.c bgnav.c bgrouter.c bgicons.c

all: $(OUT)/bgview $(OUT)/bgroute
$(OUT)/bgview: bgview.c $(LIB) *.h
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ bgview.c $(LIB) -lm
$(OUT)/bgroute: bgroute-cli.c $(LIB) *.h
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ bgroute-cli.c $(LIB) -lm
clean:
	rm -rf build
.PHONY: all clean
