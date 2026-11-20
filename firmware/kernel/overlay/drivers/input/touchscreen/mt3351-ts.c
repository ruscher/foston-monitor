// SPDX-License-Identifier: GPL-2.0-only
/*
 * MediaTek MT3351 resistive touchscreen ADC (FOSTON FS-707DC).
 *
 * Registers from the Windows CE touch.dll of the device, confirmed with
 * devmem on Linux (docs/touchscreen-driver.md):
 *   +0x3c  channel select: 0x10 = horizontal plate, 0x50 = vertical plate,
 *          0x30 / 0x40 = Z1 / Z2 (contact resistance)
 *   +0x40  write 1 to start a conversion; bit 0 busy, bit 1 pen down
 *   +0x44  10-bit result
 * The block is left configured by Windows CE (debounce, enable bits); this
 * driver only selects channels and starts conversions, it never touches
 * clocks, pin muxing or GPIO.
 *
 * No pen-down interrupt is known yet, so the device is polled while open.
 * Each axis is sampled several times and rejected when the samples spread
 * too far apart. A light touch reads too close to the middle of the panel,
 * so, as in Windows CE (TouchResistorThreshold 2500), a contact whose
 * resistance Y * (Z2 - Z1) / Z1 exceeds the threshold counts as no touch:
 * measured 1255-2072 for a firm press anywhere, 4700 and up while the
 * finger lands or lifts. Once a touch is down a 1.5x lighter contact is
 * still accepted, since the pressure drops while dragging, and the touch is
 * only released after release_ms without a good reading, so a drag does not
 * break into many short touches. A position is also reported one poll late,
 * so a reading taken while the finger lifts off is dropped, not reported.
 */
#include <linux/bitops.h>
#include <linux/input.h>
#include <linux/input/touchscreen.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/sort.h>

#define MT3351_TS_CHAN		0x3c
#define MT3351_TS_CTRL		0x40
#define MT3351_TS_DATA		0x44

#define MT3351_TS_START		BIT(0)
#define MT3351_TS_BUSY		BIT(0)
#define MT3351_TS_PEN		BIT(1)

#define MT3351_TS_CH_X		0x10	/* ~880 at the left edge, ~140 at the right */
#define MT3351_TS_CH_Y		0x50	/* ~775 at the top edge, ~190 at the bottom */
#define MT3351_TS_X_IDLE	1016	/* readings with no contact: X 1023, Y 0 */
#define MT3351_TS_Y_IDLE	7
#define MT3351_TS_CH_Z1		0x30
#define MT3351_TS_CH_Z2		0x40

#define MT3351_TS_MAX		1023
#define MT3351_TS_SAMPLES	5

static unsigned int poll_ms = 10;
module_param(poll_ms, uint, 0444);
MODULE_PARM_DESC(poll_ms, "polling interval in ms while the device is open");

static unsigned int max_spread = 24;
module_param(max_spread, uint, 0644);
MODULE_PARM_DESC(max_spread, "largest spread of the middle samples of one axis (ADC units)");

static unsigned int max_resistance = 2500;
module_param(max_resistance, uint, 0644);
MODULE_PARM_DESC(max_resistance, "lightest touch accepted, as Y * (Z2 - Z1) / Z1 (Windows CE: 2500)");

static unsigned int release_ms = 100;
module_param(release_ms, uint, 0644);
MODULE_PARM_DESC(release_ms, "time without a good reading before the touch is released");

struct mt3351_ts {
	void __iomem *base;
	struct input_dev *input;
	struct touchscreen_properties prop;
	bool pending, down;
	unsigned int px, py;
	unsigned int misses;
};

static bool mt3351_ts_pen(struct mt3351_ts *ts)
{
	return readl(ts->base + MT3351_TS_CTRL) & MT3351_TS_PEN;
}

static int mt3351_ts_convert(struct mt3351_ts *ts, u32 chan, unsigned int *val)
{
	u32 ctrl;
	int ret;

	writel(chan, ts->base + MT3351_TS_CHAN);
	writel(MT3351_TS_START, ts->base + MT3351_TS_CTRL);
	ret = readl_poll_timeout_atomic(ts->base + MT3351_TS_CTRL, ctrl,
					!(ctrl & MT3351_TS_BUSY), 1, 500);
	if (ret)
		return ret;
	*val = readl(ts->base + MT3351_TS_DATA) & MT3351_TS_MAX;
	return 0;
}

static int mt3351_ts_cmp(const void *a, const void *b)
{
	return *(const unsigned int *)a - *(const unsigned int *)b;
}

/* median-of-five style: drop the lowest and highest, average the rest */
static int mt3351_ts_sample(struct mt3351_ts *ts, u32 chan, unsigned int *val, unsigned int spread)
{
	unsigned int s[MT3351_TS_SAMPLES];
	int i, ret;

	for (i = 0; i < MT3351_TS_SAMPLES; i++) {
		ret = mt3351_ts_convert(ts, chan, &s[i]);
		if (ret)
			return ret;
	}
	sort(s, MT3351_TS_SAMPLES, sizeof(s[0]), mt3351_ts_cmp, NULL);
	if (s[MT3351_TS_SAMPLES - 2] - s[1] > spread)
		return -EAGAIN;
	*val = (s[1] + s[2] + s[3]) / 3;
	return 0;
}

static void mt3351_ts_release(struct mt3351_ts *ts)
{
	ts->pending = false;
	ts->misses = 0;
	if (!ts->down)
		return;
	ts->down = false;
	input_report_key(ts->input, BTN_TOUCH, 0);
	input_sync(ts->input);
}

static void mt3351_ts_poll(struct input_dev *input)
{
	struct mt3351_ts *ts = input_get_drvdata(input);
	unsigned int x, y, z1, z2, limit;

	limit = ts->down ? max_resistance * 3 / 2 : max_resistance;
	if (!mt3351_ts_pen(ts) ||
	    mt3351_ts_sample(ts, MT3351_TS_CH_X, &x, max_spread) ||
	    mt3351_ts_sample(ts, MT3351_TS_CH_Y, &y, max_spread) ||
	    mt3351_ts_sample(ts, MT3351_TS_CH_Z1, &z1, MT3351_TS_MAX) ||
	    mt3351_ts_sample(ts, MT3351_TS_CH_Z2, &z2, MT3351_TS_MAX) ||
	    x >= MT3351_TS_X_IDLE || y <= MT3351_TS_Y_IDLE ||	/* plates not in contact */
	    !z1 || z2 <= z1 || y * (z2 - z1) / z1 > limit ||	/* too light to be reliable */
	    !mt3351_ts_pen(ts)) {				/* lifted while sampling */
		ts->pending = false;
		if (++ts->misses * max(poll_ms, 1U) >= release_ms)
			mt3351_ts_release(ts);
		return;
	}
	ts->misses = 0;

	if (ts->pending) {
		touchscreen_report_pos(input, &ts->prop, ts->px, ts->py, false);
		input_report_key(input, BTN_TOUCH, 1);
		input_sync(input);
		ts->down = true;
	}
	ts->px = x;
	ts->py = y;
	ts->pending = true;
}

static void mt3351_ts_close(struct input_dev *input)
{
	mt3351_ts_release(input_get_drvdata(input));
}

static int mt3351_ts_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct mt3351_ts *ts;
	struct input_dev *input;
	int ret;

	ts = devm_kzalloc(dev, sizeof(*ts), GFP_KERNEL);
	if (!ts)
		return -ENOMEM;

	ts->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(ts->base))
		return PTR_ERR(ts->base);

	input = devm_input_allocate_device(dev);
	if (!input)
		return -ENOMEM;
	ts->input = input;
	input->name = "MT3351 touchscreen";
	input->phys = "mt3351-ts/input0";
	input->id.bustype = BUS_HOST;
	input->close = mt3351_ts_close;
	input_set_drvdata(input, ts);

	input_set_capability(input, EV_KEY, BTN_TOUCH);
	input_set_abs_params(input, ABS_X, 0, MT3351_TS_MAX, 0, 0);
	input_set_abs_params(input, ABS_Y, 0, MT3351_TS_MAX, 0, 0);
	touchscreen_parse_properties(input, false, &ts->prop);

	ret = input_setup_polling(input, mt3351_ts_poll);
	if (ret)
		return ret;
	input_set_poll_interval(input, poll_ms);

	ret = input_register_device(input);
	if (ret)
		return ret;

	dev_info(dev, "pen %s, polling every %u ms\n",
		 mt3351_ts_pen(ts) ? "down" : "up", poll_ms);
	return 0;
}

static const struct of_device_id mt3351_ts_of_match[] = {
	{ .compatible = "mediatek,mt3351-ts" },
	{ }
};
MODULE_DEVICE_TABLE(of, mt3351_ts_of_match);

static struct platform_driver mt3351_ts_driver = {
	.probe = mt3351_ts_probe,
	.driver = {
		.name = "mt3351-ts",
		.of_match_table = mt3351_ts_of_match,
	},
};
module_platform_driver(mt3351_ts_driver);

MODULE_DESCRIPTION("MediaTek MT3351 resistive touchscreen");
MODULE_LICENSE("GPL");
