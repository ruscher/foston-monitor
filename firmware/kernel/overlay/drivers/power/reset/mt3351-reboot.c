// SPDX-License-Identifier: GPL-2.0-only
/*
 * Restart of the FOSTON FS-707DC (MediaTek MT3351) through the hardware
 * watchdog, exactly as the Windows CE OAL does it (nk.exe
 * OALIoCtlHalReboot): three 32-bit writes to the watchdog block at
 * 0x80003000, then wait. The device resets about a second later and boots
 * Windows CE again. Tested from Linux with devmem on 2026-11-20.
 *
 * On this device the reset leaves it switched off (the power button starts
 * it again, 2026-11-20), and no separate power-off control is known, so the
 * same sequence also serves as power-off: without it "poweroff" only halted
 * the CPU with the screen lit.
 *
 * CE's values (field meanings inferred from the MT6516 watchdog):
 *   +0x04 = 0x0828  timeout, key 0x08
 *   +0x08 = 0x1971  restart key (reload the counter)
 *   +0x00 = 0x2201  mode, key 0x22, enable
 */
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/reboot.h>

static int mt3351_restart(struct sys_off_data *data)
{
	void __iomem *wdt = data->cb_data;

	writel(0x0828, wdt + 0x04);
	writel(0x1971, wdt + 0x08);
	writel(0x2201, wdt + 0x00);
	mdelay(3000);
	pr_emerg("mt3351-reboot: watchdog reset did not happen\n");
	return NOTIFY_DONE;
}

static int mt3351_reboot_probe(struct platform_device *pdev)
{
	void __iomem *wdt = devm_platform_ioremap_resource(pdev, 0);
	int ret;

	if (IS_ERR(wdt))
		return PTR_ERR(wdt);
	ret = devm_register_sys_off_handler(&pdev->dev, SYS_OFF_MODE_RESTART,
					    SYS_OFF_PRIO_DEFAULT, mt3351_restart, (void *)wdt);
	if (ret)
		return ret;
	return devm_register_sys_off_handler(&pdev->dev, SYS_OFF_MODE_POWER_OFF,
					     SYS_OFF_PRIO_DEFAULT, mt3351_restart, (void *)wdt);
}

static const struct of_device_id mt3351_reboot_of_match[] = {
	{ .compatible = "mediatek,mt3351-wdt-reboot" },
	{ }
};

static struct platform_driver mt3351_reboot_driver = {
	.probe = mt3351_reboot_probe,
	.driver = {
		.name = "mt3351-reboot",
		.of_match_table = mt3351_reboot_of_match,
	},
};
builtin_platform_driver(mt3351_reboot_driver);
