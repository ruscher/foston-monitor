# bigfoston-bigscreen: Big Screen Monitor Display client for BigFoston OS (Linux).
#   make                       host build (tests)
#   make CC=arm-buildroot-linux-musleabi-gcc   device build
CC ?= cc
CFLAGS ?= -O2 -Wall -Wextra -std=c11
OUT ?= build

all: $(OUT)/bigfoston-bigscreen

$(OUT)/bigfoston-bigscreen: fsp-linux.c fsp.c fsp.h
	mkdir -p $(OUT)
	$(CC) $(CFLAGS) -o $@ fsp-linux.c fsp.c

clean:
	rm -rf $(OUT)
.PHONY: all clean
