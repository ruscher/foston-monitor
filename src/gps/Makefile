# bfgps: carregador do gps.dll do FOSTON (docs/gps.md)
#   make            -> build/bfgps (ARM, estático, compilador do Buildroot)
#   make CROSS=     -> não usado (o DLL só roda em ARM)
TOP   := $(abspath ../..)
CROSS ?= $(TOP)/firmware/build/out/host/bin/arm-buildroot-linux-musleabi-
CC    := $(CROSS)gcc
STRIP := $(CROSS)strip
# -marm: o código do CE é ARMv4 (volta com "mov pc"); estático e ligado em
# 0x10000000 para que o heap (brk) nunca chegue aos endereços fixos do DLL
# (0x01DC0000-0x01F14000)
CFLAGS  ?= -O2 -g
CFLAGS  += -marm -std=gnu11 -Wall -Wextra -Wshadow -Wno-unused-parameter
LDFLAGS += -static -Wl,-Ttext-segment=0x10000000
LDLIBS  := -lpthread -lm

SRCS := gpsce.c cefmt.c thunks.S
build/bfgps: $(SRCS) cefmt.h Makefile
	@mkdir -p build
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(SRCS) $(LDLIBS)
	$(STRIP) -o $@.stripped $@
	@ls -l $@.stripped | awk '{print "==> " $$9 ": " $$5 " bytes"}'

clean:
	rm -rf build
.PHONY: clean
