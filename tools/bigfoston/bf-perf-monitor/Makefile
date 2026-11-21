# bf-perf-monitor: amostrador leve (só /proc). make CROSS_COMPILE=arm-buildroot-linux-musleabi-
CC ?= $(CROSS_COMPILE)gcc
CFLAGS ?= -O2 -Wall -Wextra
ARMFLAGS = -march=armv6 -mfloat-abi=soft -marm -mno-unaligned-access -static
bf-perf-monitor: bf-perf-monitor.c
	$(CROSS_COMPILE)gcc $(CFLAGS) $(if $(CROSS_COMPILE),$(ARMFLAGS)) -o $@ $<
clean:
	rm -f bf-perf-monitor
.PHONY: clean
