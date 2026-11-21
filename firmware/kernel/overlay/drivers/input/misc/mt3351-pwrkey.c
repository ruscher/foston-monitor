// SPDX-License-Identifier: GPL-2.0-only
/*
 * Power button of the FOSTON FS-707DC (MediaTek MT3351), as KEY_POWER.
 *
 * The button is a GPIO input. The Windows CE keypad driver of the device
 * (keypad_Us.dll) reads it through the GPIO driver of buses.dll every 50 ms
 * (pin 106, "GIO1:" IOCTL 0x220c00), and buses.dll reads pin n at
 * 0x80002400 + 0x10 * (n / 32), bit n % 32. The line is active low: the
 * bit reads 1 while the button is up (checked with devmem on Linux).
 *
 * No GPIO interrupt is known, so the word is polled while the input device
 * is open, and a change is reported only after it stays the same for
 * "debounce" polls in a row. The driver only reads that one word: it never
 * writes GPIO direction, pull or pin muxing, which Windows CE leaves set up.
 *
 * Holding the button for about 10 s still cuts the power in hardware (timer
 * at 0x80035200 armed by Windows CE spc.dll); this driver does not touch it.
 */
#include <linux/bitops.h>
#include <linux/input.h>
#include <linux/io.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/property.h>

static unsigned int poll_ms = 20;
module_param(poll_ms, uint, 0444);
MODULE_PARM_DESC(poll_ms, "polling interval in ms while the device is open");

static unsigned int debounce = 2;
module_param(debounce, uint, 0644);
MODULE_PARM_DESC(debounce, "polls in a row with the same level before a change is reported");

struct mt3351_pwrkey {
	void __iomem *reg;
	u32 mask;
	bool active_low;
	bool down;		/* reported state */
	unsigned int same;	/* polls in a row that disagree with "down" */
};

static bool mt3351_pwrkey_read(struct mt3351_pwrkey *k)
{
	bool level = readl_relaxed(k->reg) & k->mask;

	return level != k->active_low;
}

static void mt3351_pwrkey_poll(struct input_dev *input)
{
	struct mt3351_pwrkey *k = input_get_drvdata(input);
	bool now = mt3351_pwrkey_read(k);

	if (now == k->down) {
		k->same = 0;
		return;
	}
	if (++k->same < max(debounce, 1u))
		return;
	k->same = 0;
	k->down = now;
	input_report_key(input, KEY_POWER, now);
	input_sync(input);
}

static int mt3351_pwrkey_open(struct input_dev *input)
{
	struct mt3351_pwrkey *k = input_get_drvdata(input);

	/* a button already held when the device opens is not a new press */
	k->down = mt3351_pwrkey_read(k);
	k->same = 0;
	return 0;
}

static int mt3351_pwrkey_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct mt3351_pwrkey *k;
	struct input_dev *input;
	struct resource *res;
	u32 bit;
	int ret;

	k = devm_kzalloc(dev, sizeof(*k), GFP_KERNEL);
	if (!k)
		return -ENOMEM;

	/*
	 * The GPIO block is shared with other users of the same words (USB
	 * VBUS is bit 7 of this block's first input word), so map without
	 * claiming the region.
	 */
	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res || resource_size(res) < 4)
		return -EINVAL;
	k->reg = devm_ioremap(dev, res->start, 4);
	if (!k->reg)
		return -ENOMEM;

	ret = device_property_read_u32(dev, "mediatek,bit", &bit);
	if (ret || bit > 31)
		return dev_err_probe(dev, -EINVAL, "missing or bad mediatek,bit\n");
	k->mask = BIT(bit);
	k->active_low = device_property_read_bool(dev, "mediatek,active-low");

	input = devm_input_allocate_device(dev);
	if (!input)
		return -ENOMEM;
	input->name = "MT3351 power button";
	input->phys = "mt3351-pwrkey/input0";
	input->id.bustype = BUS_HOST;
	input->open = mt3351_pwrkey_open;
	input_set_capability(input, EV_KEY, KEY_POWER);
	input_set_drvdata(input, k);

	ret = input_setup_polling(input, mt3351_pwrkey_poll);
	if (ret)
		return ret;
	input_set_poll_interval(input, clamp(poll_ms, 5u, 200u));

	ret = input_register_device(input);
	if (ret)
		return ret;

	dev_info(dev, "power button at %pa bit %u (%s), %u ms polling\n",
		 &res->start, bit, k->active_low ? "active low" : "active high",
		 clamp(poll_ms, 5u, 200u));
	return 0;
}

static const struct of_device_id mt3351_pwrkey_of_match[] = {
	{ .compatible = "foston,fs707dc-pwrkey" },
	{ }
};
MODULE_DEVICE_TABLE(of, mt3351_pwrkey_of_match);

static struct platform_driver mt3351_pwrkey_driver = {
	.probe = mt3351_pwrkey_probe,
	.driver = {
		.name = "mt3351-pwrkey",
		.of_match_table = mt3351_pwrkey_of_match,
	},
};
module_platform_driver(mt3351_pwrkey_driver);

MODULE_DESCRIPTION("FOSTON FS-707DC (MT3351) power button");
MODULE_LICENSE("GPL");
