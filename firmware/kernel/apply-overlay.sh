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
add_before "$T/drivers/mmc/host/Kconfig" 'config MMC_MTK' 'config MMC_MT3351
	tristate "MediaTek MT3351 MSDC SD host (polled PIO)"
	depends on ARCH_MT3351 || COMPILE_TEST
	help
	  SD card slot of the MediaTek MT3351 (FOSTON FS-707DC).
'
add "$T/drivers/mmc/host/Makefile" 'obj-$(CONFIG_MMC_MT3351)		+= mt3351-msdc.o'
add_before "$T/drivers/video/backlight/Kconfig" 'config BACKLIGHT_PWM' 'config BACKLIGHT_MT3351
	tristate "FOSTON FS-707DC (MT3351 PMU) backlight"
	depends on ARCH_MT3351 || COMPILE_TEST
	help
	  LCD backlight level register of the FOSTON FS-707DC.
'
add "$T/drivers/video/backlight/Makefile" 'obj-$(CONFIG_BACKLIGHT_MT3351)	+= mt3351_bl.o'
add_before "$T/drivers/power/reset/Kconfig" 'config POWER_RESET_MT6323' 'config POWER_RESET_MT3351
	bool "MediaTek MT3351 restart through the watchdog"
	depends on ARCH_MT3351 || COMPILE_TEST
	help
	  Restart of the FOSTON FS-707DC, as its Windows CE OAL does it.
'
add "$T/drivers/power/reset/Makefile" 'obj-$(CONFIG_POWER_RESET_MT3351)	+= mt3351-reboot.o'
add_before "$T/sound/arm/Kconfig" 'config SND_PXA2XX_AC97' 'config SND_MT3351
	tristate "FOSTON FS-707DC speaker (MediaTek MT3351 audio DMA)"
	depends on ARCH_MT3351 || COMPILE_TEST
	select SND_PCM
	select SND_JACK
	help
	  Speaker playback of the FOSTON FS-707DC, with headphone detection.
'
add "$T/sound/arm/Makefile" 'obj-$(CONFIG_SND_MT3351)	+= mt3351-audio.o'
add_before "$T/drivers/tty/serial/8250/Kconfig" 'config SERIAL_8250_MT6577' 'config SERIAL_8250_MT3351
	tristate "MediaTek MT3351 UARTs"
	depends on SERIAL_8250 && (ARCH_MT3351 || COMPILE_TEST)
	help
	  UARTs of the MediaTek MT3351 (FOSTON FS-707DC), with the fixups
	  needed after Windows CE hands the SoC over.
'
add "$T/drivers/tty/serial/8250/Makefile" 'obj-$(CONFIG_SERIAL_8250_MT3351)	+= 8250_mt3351.o'
add_before "$T/drivers/input/misc/Kconfig" 'config INPUT_MAX77650_ONKEY' 'config INPUT_MT3351_PWRKEY
	tristate "FOSTON FS-707DC (MT3351) power button"
	depends on ARCH_MT3351 || COMPILE_TEST
	help
	  Power button of the FOSTON FS-707DC (a key of the MT3351 keypad
	  controller, polled), reported as KEY_POWER.
'
add "$T/drivers/input/misc/Makefile" 'obj-$(CONFIG_INPUT_MT3351_PWRKEY)	+= mt3351-pwrkey.o'
# the boot console draws with the VGA 8x8 font but has no fbcon: let it satisfy
# FONT_8x8's dependency (otherwise "unmet direct dependencies" at every build)
grep -q 'STI_CONSOLE || DRM_PANIC || MT3351_BOOTCON' "$T/lib/fonts/Kconfig" ||
    awk '/^config FONT_8x8$/ { b = 1 } b && /^\tdepends on / { $0 = $0 " || MT3351_BOOTCON"; b = 0 } { print }' \
        "$T/lib/fonts/Kconfig" > "$T/lib/fonts/Kconfig.tmp" && mv "$T/lib/fonts/Kconfig.tmp" "$T/lib/fonts/Kconfig"
echo "BigFoston MT3351 support applied to $T"
