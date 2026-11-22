# Bluetooth do FOSTON (docs/hardware-enablement/06)
#   make   -> build/bfbt-attach, build/btprobe (ARM, compilador do Buildroot)
TOP   := $(abspath ../..)
CROSS ?= $(TOP)/firmware/build/out/host/bin/arm-buildroot-linux-musleabi-
CC    := $(CROSS)gcc
STRIP := $(CROSS)strip
CFLAGS ?= -O2 -g
LDFLAGS ?= -static
CFLAGS += -std=gnu11 -Wall -Wextra -Wno-unused-parameter
PROGS := build/bfbt-attach build/btprobe build/bfhci build/bfsnoop build/bfpbap
all: $(PROGS)
build/%: %.c Makefile
	@mkdir -p build
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $<
	$(STRIP) -o $@.stripped $@
# bfbt-agent usa o libdbus (bibliotecas compartilhadas): só com AGENT=1 e DBUS_CFLAGS/DBUS_LIBS
build/bfbt-agent: bfbt-agent.c Makefile
	@mkdir -p build
	$(CC) $(CFLAGS) $(DBUS_CFLAGS) -o $@ $< $(DBUS_LIBS)
	$(STRIP) -o $@.stripped $@
build/bfphone: bfphone.c Makefile
	@mkdir -p build
	$(CC) $(CFLAGS) $(DBUS_CFLAGS) -o $@ $< $(DBUS_LIBS)
	$(STRIP) -o $@.stripped $@
ifeq ($(AGENT),1)
all: build/bfbt-agent build/bfphone
endif
clean:
	rm -rf build
.PHONY: all clean
