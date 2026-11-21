// SPDX-License-Identifier: GPL-2.0-only
/*
 * LCD backlight level of the FOSTON FS-707DC (MediaTek MT3351).
 *
 * From the Windows CE BKL driver (buses.dll) of the device: the level is a
 * single PMU register, 0x80061308, holding 0..19; CE never writes more than
 * 19 (0x13), and below 3 it switches the LED enable (PWM channel 5) to a
 * 25 % pattern. This driver writes only that register and only 3..19, so
 * the PWM pattern CE left (constantly on) stays valid. Every other PMU word
 * (SD power, charger, suspend) is out of its mapping on purpose.
 *
 * brightness 0..16  ->  register 3..19
 */
#include <linux/backlight.h>
#include <linux/io.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/platform_device.h>

#define MT3351_BL_MIN	3
#define MT3351_BL_MAX	19	/* 0x13: the highest value Windows CE uses */

struct mt3351_bl {
	void __iomem *reg;
};

static int mt3351_bl_update(struct backlight_device *bd)
{
	struct mt3351_bl *bl = bl_get_data(bd);
	int b = backlight_get_brightness(bd);	/* 0 when blanked */

	writel(MT3351_BL_MIN + min(b, MT3351_BL_MAX - MT3351_BL_MIN), bl->reg);
	return 0;
}

static const struct backlight_ops mt3351_bl_ops = {
	.options = BL_CORE_SUSPENDRESUME,
	.update_status = mt3351_bl_update,
};

static int mt3351_bl_probe(struct platform_device *pdev)
{
	struct backlight_properties props = {
		.type = BACKLIGHT_PLATFORM,
		.max_brightness = MT3351_BL_MAX - MT3351_BL_MIN,
		.scale = BACKLIGHT_SCALE_NON_LINEAR,
	};
	struct backlight_device *bd;
	struct mt3351_bl *bl;
	u32 cur;

	bl = devm_kzalloc(&pdev->dev, sizeof(*bl), GFP_KERNEL);
	if (!bl)
		return -ENOMEM;
	bl->reg = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(bl->reg))
		return PTR_ERR(bl->reg);

	/* start from the level Windows CE left */
	cur = readl(bl->reg);
	props.brightness = clamp_t(u32, cur, MT3351_BL_MIN, MT3351_BL_MAX) - MT3351_BL_MIN;

	bd = devm_backlight_device_register(&pdev->dev, "mt3351-backlight", &pdev->dev,
					   bl, &mt3351_bl_ops, &props);
	if (IS_ERR(bd))
		return PTR_ERR(bd);
	dev_info(&pdev->dev, "level %u left by CE, brightness %d/%d\n",
		 cur, props.brightness, props.max_brightness);
	return 0;
}

static const struct of_device_id mt3351_bl_of_match[] = {
	{ .compatible = "foston,fs707dc-backlight" },
	{ }
};
MODULE_DEVICE_TABLE(of, mt3351_bl_of_match);

static struct platform_driver mt3351_bl_driver = {
	.probe = mt3351_bl_probe,
	.driver = {
		.name = "mt3351-backlight",
		.of_match_table = mt3351_bl_of_match,
	},
};
module_platform_driver(mt3351_bl_driver);

MODULE_DESCRIPTION("FOSTON FS-707DC backlight level (MT3351 PMU)");
MODULE_LICENSE("GPL");
