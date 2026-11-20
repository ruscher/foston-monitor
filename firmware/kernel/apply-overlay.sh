#!/bin/sh
# Add the BigFoston MT3351 support (overlay/ + build glue) to a Linux 6.12 tree.
# Idempotent. Usage: apply-overlay.sh LINUX_TREE
# SPDX-License-Identifier: GPL-2.0-only
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
T=$1
cp -r "$HERE/overlay/." "$T/"

add() { # file, line: append the line once
    grep -qxF "$2" "$1" || printf '%s\n' "$2" >> "$1"
}
add_before() { # file, anchor, line: insert the line before the first anchor line, once
    grep -qxF "$(printf '%s\n' "$3" | head -n1)" "$1" && return 0   # first line marks it as applied
    A="$2" L="$3" awk '!done && $0 == ENVIRON["A"] { print ENVIRON["L"]; done = 1 } { print }' "$1" > "$1.tmp"
    mv "$1.tmp" "$1"
}

add_before "$T/arch/arm/Kconfig" 'source "arch/arm/mach-mmp/Kconfig"' 'source "arch/arm/mach-mt3351/Kconfig"'

# MT3351 defect: an odd-address STRH right after a copy-on-write fault loses the
# byte stored just before it in the same word. Never let the compiler emit
# unaligned stores (user space gets the same flag from BR2_TARGET_OPTIMIZATION).
add_before "$T/arch/arm/Makefile" 'ifeq ($(CONFIG_FRAME_POINTER),y)' 'KBUILD_CFLAGS	+= $(if $(CONFIG_ARCH_MT3351),-mno-unaligned-access)'

cat_once() { # file, marker, text
    grep -qF "$2" "$1" || printf '%s\n' "$3" >> "$1"
}
cat_once "$T/drivers/irqchip/Kconfig" "config MT3351_CIRQ" '
config MT3351_CIRQ
	bool
	select IRQ_DOMAIN
	help
	  MediaTek MT3351 CIRQ interrupt controller.'
add "$T/drivers/irqchip/Makefile" 'obj-$(CONFIG_MT3351_CIRQ)		+= irq-mt3351-cirq.o'

cat_once "$T/drivers/clocksource/Kconfig" "config MT3351_GPT_TIMER" '
config MT3351_GPT_TIMER
	bool
	select TIMER_OF
	select CLKSRC_MMIO
	help
	  MediaTek MT3351 general purpose timer (clocksource and clockevent).'
add "$T/drivers/clocksource/Makefile" 'obj-$(CONFIG_MT3351_GPT_TIMER)	+= timer-mt3351-gpt.o'

add "$T/drivers/video/fbdev/Makefile" 'obj-$(CONFIG_MT3351_BOOTCON)	+= mt3351-bootcon.o'
add "$T/arch/arm/boot/dts/mediatek/Makefile" 'dtb-$(CONFIG_ARCH_MT3351) += mt3351-foston-fs707dc.dtb'
add_before "$T/drivers/usb/musb/Kconfig" 'config USB_MUSB_MEDIATEK' 'config USB_MUSB_MT3351
	tristate "MediaTek MT3351"
	depends on ARCH_MT3351 || COMPILE_TEST
	depends on OF
	depends on NOP_USB_XCEIV
	help
	  Mentor MUSB function controller of the MediaTek MT3351 (FOSTON FS-707DC).
'
add "$T/drivers/usb/musb/Makefile" 'obj-$(CONFIG_USB_MUSB_MT3351)			+= mt3351.o'
add_before "$T/drivers/input/touchscreen/Kconfig" 'config TOUCHSCREEN_MTOUCH' 'config TOUCHSCREEN_MT3351
	tristate "MediaTek MT3351 resistive touchscreen"
	depends on ARCH_MT3351 || COMPILE_TEST
	help
	  Resistive touchscreen ADC of the MediaTek MT3351 (FOSTON FS-707DC).
'
add "$T/drivers/input/touchscreen/Makefile" 'obj-$(CONFIG_TOUCHSCREEN_MT3351)	+= mt3351-ts.o'
echo "BigFoston MT3351 support applied to $T"
