# Bluetooth do FOSTON (docs/hardware-enablement/06)
#   make   -> build/bfbt-attach, build/btprobe (ARM, compilador do Buildroot)
TOP   := $(abspath ../..)
CROSS ?= $(TOP)/firmware/build/out/host/bin/arm-buildroot-linux-musleabi-
CC    := $(CROSS)gcc
STRIP := $(CROSS)strip
CFLAGS ?= -O2 -g
LDFLAGS ?= -static
CFLAGS += -std=gnu11 -Wall -Wextra -Wno-unused-parameter
PROGS := build/bfbt-attach build/btprobe build/bfhci
all: $(PROGS)
build/%: %.c Makefile
	@mkdir -p build
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $<
	$(STRIP) -o $@.stripped $@
clean:
	rm -rf build
.PHONY: all clean
