# bgrecv for the device (static, ARMv6 soft-float); uses the agent's SHA-256.
# SPDX-License-Identifier: GPL-3.0-or-later
CC = ../../../firmware/build/out/host/bin/arm-buildroot-linux-musleabi-gcc
CFLAGS = -O2 -std=c11 -Wall -Wextra -I../../../src/wince/agent
bgrecv: bgrecv.c ../../../src/wince/agent/sha256.c
	$(CC) $(CFLAGS) -static -o $@ $^
clean:
	rm -f bgrecv
.PHONY: clean
