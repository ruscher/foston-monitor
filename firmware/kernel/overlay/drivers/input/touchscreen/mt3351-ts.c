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
 *
 * The same ADC block also measures the battery (CE Battdrvr.dll, "aux"
 * mode: +0x04 = 0 then 0x1ff starts, +0x0c bit0 busy, +0x28 battery,
 * mV = code * 5600 / 1023). This driver owns the block, so it also
 * registers the battery and charger power supplies, reading under the
 * same lock as the touch. Charger state comes from the PMU word 0x80061420
 * (bit3 charger present, bit14 charging), only read. Nothing of the
 * charger is ever written: Windows CE drives charging in software and
 * Linux does not (yet), see docs.
 */
#include <linux/bitops.h>
#include <linux/delay.h>
#include <linux/input.h>
#include <linux/input/touchscreen.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/platform_device.h>
#include <linux/power_supply.h>
#include <linux/reboot.h>
#include <linux/sort.h>
#include <linux/workqueue.h>

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

#define MT3351_AUX_START	0x04
#define MT3351_AUX_STA		0x0c
#define MT3351_AUX_BAT		0x28
#define MT3351_AUX_ISENSE	0x2c
#define MT3351_AUX_VCHG		0x30
#define MT3351_CHR_FAULT	BIT(1)
#define MT3351_CHR_NOBAT	BIT(2)
#define MT3351_CHR_PRESENT	BIT(3)
#define MT3351_CHR_CV		BIT(4)
#define MT3351_CHR_S1001	BIT(12)
#define MT3351_CHR_PRE		BIT(13)
#define MT3351_CHR_CHARGING	BIT(14)
#define MT3351_CHR_INVALID	BIT(15)

/* charger words, offsets from 0x80061378 (CE Battdrvr.dll, docs/battery-charger.md) */
#define CHR_CON		0x00	/* bit1 charge on, [8:5] current code; CE Init 0xA00 (CV 4.2 V) */
#define CHR_LED		0x8c	/* 0x80061404: bit0 charging, bit4 full, bit8 */
#define CHR_410		0x98	/* 0x80061410: CE sets bits 6, 7 every cycle */
#define CHR_WDT		0x9c	/* 0x80061414: 0xa250 then 0xa257 re-arms the charge watchdog */
#define CHR_KEY		0xa0	/* 0x80061418: 0xaf26 before each change of CHR_CON */
#define CHR_STA		0xa8	/* 0x80061420 */
#define CHR_FLT		0xb0	/* 0x80061428: [1:0] 2 = watchdog timeout, 3 = temperature; bit2 RTC */
#define CHR_WIN		0xb4
#define CHR_KEY_VAL	0xaf26
#define CHR_ON		BIT(1)
#define CHR_CUR		GENMASK(8, 5)
#define CHR_PERIOD_S	10

#define MT3351_TS_MAX		1023
#define MT3351_TS_SAMPLES	5

/* Charging is done by software, as Windows CE does; off until enabled. */
static bool charge;
module_param(charge, bool, 0644);
MODULE_PARM_DESC(charge, "charge the battery (Windows CE Battdrvr.dll algorithm)");

static unsigned int charge_current = 0x80;
module_param(charge_current, uint, 0644);
MODULE_PARM_DESC(charge_current, "CC current code written to [8:5] (0x80, 0xa0 or 0xe0 as CE; default the lowest)");

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
	struct mutex lock;		/* the ADC: touch conversions vs battery */
	void __iomem *chr;		/* PMU charger status word, read only */
	struct power_supply *bat, *ac;
	unsigned long bat_jiffies;
	int bat_mv;
	/* charger */
	struct device *dev;
	void __iomem *pmu;		/* 0x80061378..0x8006142b */
	void __iomem *c018;		/* 0x8002c018, read only: bit7 must be 1 to charge */
	struct delayed_work chg_work;
	bool chg_on, chg_full, chg_locked;
	int chg_state;			/* 0 idle, CE states 0x1000..0x1003 */
	unsigned int pre_s, cv_s, total_s;
	int vbat, visense, vchg, low_count;
	int last_log;
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

static void mt3351_ts_poll_locked(struct input_dev *input);

static void mt3351_ts_poll(struct input_dev *input)
{
	struct mt3351_ts *ts = input_get_drvdata(input);

	mutex_lock(&ts->lock);
	mt3351_ts_poll_locked(input);
	mutex_unlock(&ts->lock);
}

static void mt3351_ts_poll_locked(struct input_dev *input)
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

/* ---- battery (aux mode of the same ADC) ---- */

static int mt3351_bat_mv(struct mt3351_ts *ts)
{
	unsigned int s[MT3351_TS_SAMPLES];
	u32 sta;
	int i;

	mutex_lock(&ts->lock);
	if (ts->bat_mv && time_before(jiffies, ts->bat_jiffies + 5 * HZ))
		goto out;
	for (i = 0; i < MT3351_TS_SAMPLES; i++) {
		writel(0, ts->base + MT3351_AUX_START);
		writel(0x1ff, ts->base + MT3351_AUX_START);
		if (readl_poll_timeout(ts->base + MT3351_AUX_STA, sta, !(sta & 1), 10, 10000))
			break;
		s[i] = readl(ts->base + MT3351_AUX_BAT) & MT3351_TS_MAX;
	}
	if (i == MT3351_TS_SAMPLES) {
		sort(s, MT3351_TS_SAMPLES, sizeof(s[0]), mt3351_ts_cmp, NULL);
		ts->bat_mv = (s[1] + s[2] + s[3]) * 5600 / (3 * 1023);
		ts->bat_jiffies = jiffies;
	}
out:
	mutex_unlock(&ts->lock);
	return ts->bat_mv;
}

/* the step table of the CE driver (on the voltage it reads while not charging) */
static int mt3351_bat_capacity(int mv)
{
	static const struct { int mv, pct; } t[] = {
		{ 3990, 100 }, { 3865, 95 }, { 3750, 80 }, { 3677, 60 },
		{ 3670, 40 }, { 3553, 20 },
	};
	int i;

	for (i = 0; i < ARRAY_SIZE(t); i++)
		if (mv >= t[i].mv)
			return t[i].pct;
	return 10;
}

/* ---- charger ---- */

/* VBAT, VISENSE (both x 5600/1023) and VCHG (x 14000/1023), averaged over the
 * samples taken while the charger status is valid (CE 0x3bb3264) */
static bool mt3351_chg_adc(struct mt3351_ts *ts)
{
	unsigned int n = 0, b = 0, i = 0, c = 0, k;
	u32 sta;

	mutex_lock(&ts->lock);
	for (k = 0; k < MT3351_TS_SAMPLES; k++) {
		writel(0, ts->base + MT3351_AUX_START);
		writel(0x1ff, ts->base + MT3351_AUX_START);
		if (readl_poll_timeout(ts->base + MT3351_AUX_STA, sta, !(sta & 1), 10, 10000))
			break;
		if (readl(ts->chr) & MT3351_CHR_INVALID)
			continue;
		b += readl(ts->base + MT3351_AUX_BAT) & MT3351_TS_MAX;
		i += readl(ts->base + MT3351_AUX_ISENSE) & MT3351_TS_MAX;
		c += readl(ts->base + MT3351_AUX_VCHG) & MT3351_TS_MAX;
		n++;
	}
	mutex_unlock(&ts->lock);
	if (!n)
		return false;
	ts->vbat = b * 5600 / (1023 * n);
	ts->visense = i * 5600 / (1023 * n);
	ts->vchg = c * 14000 / (1023 * n);
	return true;
}

static void mt3351_chg_con(struct mt3351_ts *ts, u32 clear, u32 set)
{
	u32 v = readl(ts->pmu + CHR_CON);

	writel(CHR_KEY_VAL, ts->pmu + CHR_KEY);
	writel((v & ~clear) | set, ts->pmu + CHR_CON);
}

static void mt3351_chg_led(struct mt3351_ts *ts, u32 set)
{
	u32 v = readl(ts->pmu + CHR_LED);

	writel((v & ~(BIT(0) | BIT(4) | BIT(8))) | set, ts->pmu + CHR_LED);
}

/* stop charging: CE 0x3bb4070 / 0x3bb4104 */
static void mt3351_chg_stop(struct mt3351_ts *ts, bool full)
{
	mt3351_chg_con(ts, CHR_ON, 0);
	writel(0xa250, ts->pmu + CHR_WDT);
	mt3351_chg_led(ts, full ? BIT(4) : 0);
	ts->chg_on = false;
	ts->chg_state = 0;
}

static void mt3351_chg_kick(struct mt3351_ts *ts)
{
	writel(0xa250, ts->pmu + CHR_WDT);
	udelay(100);
	writel(0xa257, ts->pmu + CHR_WDT);
}

/* CE 0x3bb3910: switch the charger on for a moment and read the phase */
static int mt3351_chg_probe_state(struct mt3351_ts *ts)
{
	u32 sta;

	mt3351_chg_con(ts, 0, CHR_ON);
	udelay(100);
	sta = readl(ts->chr);
	mt3351_chg_con(ts, CHR_ON, 0);
	if (sta & MT3351_CHR_PRE)
		return 0x1000;
	if (sta & MT3351_CHR_S1001)
		return 0x1001;
	return sta & MT3351_CHR_CV ? 0x1003 : 0x1002;
}

static void mt3351_chg_log(struct mt3351_ts *ts, int what, const char *msg)
{
	if (what == ts->last_log)
		return;
	ts->last_log = what;
	dev_info(ts->dev, "charger: %s (VBAT %d mV, I %d, VCHG %d mV)\n", msg, ts->vbat,
		 (ts->visense - ts->vbat) * 5, ts->vchg);
}

static void mt3351_chg_work(struct work_struct *w)
{
	struct mt3351_ts *ts = container_of(to_delayed_work(w), struct mt3351_ts, chg_work);
	u32 sta = readl(ts->chr), flt = readl(ts->pmu + CHR_FLT);
	unsigned int cur = charge_current;
	bool was_on = ts->chg_on;
	int st;

	if (!mt3351_chg_adc(ts))
		goto again;

	/* on battery: orderly power-off below 3450 mV (+100 as CE), 3 readings in a row */
	if (!(sta & MT3351_CHR_PRESENT) && ts->vbat > 2000 && ts->vbat + 100 < 3450) {
		if (++ts->low_count >= 3) {
			dev_crit(ts->dev, "battery low (%d mV): powering off\n", ts->vbat);
			orderly_poweroff(true);
		}
	} else {
		ts->low_count = 0;
	}

	if (!(sta & MT3351_CHR_PRESENT)) {
		if (ts->chg_on || ts->chg_locked || ts->chg_full)
			mt3351_chg_stop(ts, false);
		ts->chg_full = ts->chg_locked = false;
		ts->pre_s = ts->cv_s = ts->total_s = 0;
		mt3351_chg_log(ts, 1, "no charger");
		goto changed;
	}
	if (!charge) {
		if (ts->chg_on)
			mt3351_chg_stop(ts, false);
		mt3351_chg_log(ts, 2, "charging disabled");
		goto changed;
	}
	if (ts->chg_locked)
		goto again;
	if ((sta & (MT3351_CHR_FAULT | MT3351_CHR_NOBAT | MT3351_CHR_INVALID)) ||
	    (flt & 3) >= 2 || (flt & BIT(2)) || !(readl(ts->c018) & BIT(7)) ||
	    ts->vchg <= 4350 || ts->vchg >= 6000) {
		if (ts->chg_on)
			mt3351_chg_stop(ts, false);
		dev_warn(ts->dev, "charger: not charging (status %#x, fault %#x, %#x, VCHG %d mV)\n",
			 sta, flt, readl(ts->c018), ts->vchg);
		ts->last_log = 3;
		goto changed;
	}
	if (ts->chg_full) {
		if (ts->vbat >= 4000)
			goto again;
		ts->chg_full = false;			/* recharge */
		ts->pre_s = ts->cv_s = ts->total_s = 0;
	}

	writel(readl(ts->pmu + CHR_410) | 0xc0, ts->pmu + CHR_410);
	st = mt3351_chg_probe_state(ts);
	if (cur != 0x80 && cur != 0xa0 && cur != 0xe0)
		cur = 0x80;
	switch (st) {
	case 0x1000:				/* pre-charge */
		ts->pre_s += CHR_PERIOD_S;
		if (ts->pre_s > 1800) {
			mt3351_chg_stop(ts, false);
			ts->chg_locked = true;		/* until the charger is removed */
			dev_err(ts->dev, "charger: pre-charge longer than 30 min, stopped\n");
			goto changed;
		}
		mt3351_chg_con(ts, 0, CHR_ON);
		break;
	case 0x1002:				/* constant current */
		mt3351_chg_kick(ts);
		mt3351_chg_con(ts, CHR_ON, 0);
		writel(0, ts->pmu + CHR_KEY);
		mt3351_chg_con(ts, CHR_CUR, cur);
		mt3351_chg_con(ts, 0, CHR_ON);
		break;
	default:				/* 0x1001 and constant voltage */
		mt3351_chg_kick(ts);
		mt3351_chg_con(ts, 0, CHR_ON);
		break;
	}
	ts->chg_on = true;
	ts->chg_state = st;
	mt3351_chg_led(ts, BIT(0));
	ts->total_s += CHR_PERIOD_S;
	if (st == 0x1003) {
		ts->cv_s += CHR_PERIOD_S;
		/* end of charge: current down to 50 in CV, or 2 h in CV */
		if ((was_on && (ts->visense - ts->vbat) * 5 <= 50) || ts->cv_s >= 7200) {
			mt3351_chg_stop(ts, true);
			ts->chg_full = true;
			mt3351_chg_log(ts, 4, "full");
			goto changed;
		}
	}
	if (ts->total_s >= 28800) {			/* 8 h */
		mt3351_chg_stop(ts, true);
		ts->chg_full = true;
		mt3351_chg_log(ts, 5, "8 h limit reached");
		goto changed;
	}
	mt3351_chg_log(ts, st, st == 0x1000 ? "pre-charge" : st == 0x1002 ? "constant current" :
		       st == 0x1003 ? "constant voltage" : "charging (state 0x1001)");
changed:
	if (ts->bat)
		power_supply_changed(ts->bat);
again:
	schedule_delayed_work(&ts->chg_work, CHR_PERIOD_S * HZ);
}

static const enum power_supply_property mt3351_bat_props[] = {
	POWER_SUPPLY_PROP_STATUS,
	POWER_SUPPLY_PROP_PRESENT,
	POWER_SUPPLY_PROP_VOLTAGE_NOW,
	POWER_SUPPLY_PROP_CAPACITY,
	POWER_SUPPLY_PROP_TECHNOLOGY,
};

static int mt3351_bat_get(struct power_supply *psy, enum power_supply_property psp,
			  union power_supply_propval *val)
{
	struct mt3351_ts *ts = power_supply_get_drvdata(psy);
	u32 chr = readl(ts->chr);
	int mv;

	switch (psp) {
	case POWER_SUPPLY_PROP_STATUS:
		if (chr & MT3351_CHR_CHARGING)
			val->intval = POWER_SUPPLY_STATUS_CHARGING;
		else if (!(chr & MT3351_CHR_PRESENT))
			val->intval = POWER_SUPPLY_STATUS_DISCHARGING;
		else if (ts->chg_full || mt3351_bat_capacity(mt3351_bat_mv(ts)) >= 100)
			val->intval = POWER_SUPPLY_STATUS_FULL;
		else
			val->intval = POWER_SUPPLY_STATUS_NOT_CHARGING;
		return 0;
	case POWER_SUPPLY_PROP_PRESENT:
		val->intval = 1;
		return 0;
	case POWER_SUPPLY_PROP_VOLTAGE_NOW:
		mv = mt3351_bat_mv(ts);
		if (!mv)
			return -EIO;
		val->intval = mv * 1000;
		return 0;
	case POWER_SUPPLY_PROP_CAPACITY:
		mv = mt3351_bat_mv(ts);
		if (!mv)
			return -EIO;
		val->intval = mt3351_bat_capacity(mv);
		return 0;
	case POWER_SUPPLY_PROP_TECHNOLOGY:
		val->intval = POWER_SUPPLY_TECHNOLOGY_LION;
		return 0;
	default:
		return -EINVAL;
	}
}

static const enum power_supply_property mt3351_ac_props[] = {
	POWER_SUPPLY_PROP_ONLINE,
};

static int mt3351_ac_get(struct power_supply *psy, enum power_supply_property psp,
			 union power_supply_propval *val)
{
	struct mt3351_ts *ts = power_supply_get_drvdata(psy);

	if (psp != POWER_SUPPLY_PROP_ONLINE)
		return -EINVAL;
	val->intval = !!(readl(ts->chr) & MT3351_CHR_PRESENT);
	return 0;
}

static const struct power_supply_desc mt3351_bat_desc = {
	.name = "battery",
	.type = POWER_SUPPLY_TYPE_BATTERY,
	.properties = mt3351_bat_props,
	.num_properties = ARRAY_SIZE(mt3351_bat_props),
	.get_property = mt3351_bat_get,
};

static const struct power_supply_desc mt3351_ac_desc = {
	.name = "charger",
	.type = POWER_SUPPLY_TYPE_MAINS,
	.properties = mt3351_ac_props,
	.num_properties = ARRAY_SIZE(mt3351_ac_props),
	.get_property = mt3351_ac_get,
};

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
	mutex_init(&ts->lock);

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

	/* battery and charger: optional, the charger word is in the shared PMU page */
	{
		struct resource *r = platform_get_resource(pdev, IORESOURCE_MEM, 1);
		struct power_supply_config cfg = { .drv_data = ts, .of_node = dev->of_node };

		ts->chr = r ? devm_ioremap(dev, r->start, 4) : NULL;
		if (ts->chr) {
			ts->bat = devm_power_supply_register(dev, &mt3351_bat_desc, &cfg);
			ts->ac = devm_power_supply_register(dev, &mt3351_ac_desc, &cfg);
			if (IS_ERR(ts->bat) || IS_ERR(ts->ac)) {
				dev_warn(dev, "battery/charger not registered\n");
				ts->bat = NULL;
			} else {
				dev_info(dev, "battery %d mV, charger status %#x\n",
					 mt3351_bat_mv(ts), readl(ts->chr));
			}
		}
		/* charger control: optional, both windows are shared pages (not claimed) */
		r = platform_get_resource(pdev, IORESOURCE_MEM, 2);
		ts->pmu = r && resource_size(r) >= CHR_WIN ? devm_ioremap(dev, r->start, CHR_WIN) : NULL;
		r = platform_get_resource(pdev, IORESOURCE_MEM, 3);
		ts->c018 = r ? devm_ioremap(dev, r->start, 4) : NULL;
		if (ts->chr && ts->pmu && ts->c018) {
			ts->dev = dev;
			INIT_DELAYED_WORK(&ts->chg_work, mt3351_chg_work);
			platform_set_drvdata(pdev, ts);
			schedule_delayed_work(&ts->chg_work, 2 * HZ);
			dev_info(dev, "charger control %s (CON %#x, status %#x, fault %#x)\n",
				 charge ? "on" : "off", readl(ts->pmu + CHR_CON), readl(ts->chr),
				 readl(ts->pmu + CHR_FLT));
		}
	}

	dev_info(dev, "pen %s, polling every %u ms\n",
		 mt3351_ts_pen(ts) ? "down" : "up", poll_ms);
	return 0;
}

static const struct of_device_id mt3351_ts_of_match[] = {
	{ .compatible = "mediatek,mt3351-ts" },
	{ }
};
MODULE_DEVICE_TABLE(of, mt3351_ts_of_match);

/* power-off, reboot, unbind: never leave the charger switched on without the
 * software that re-arms its watchdog */
static void mt3351_ts_shutdown(struct platform_device *pdev)
{
	struct mt3351_ts *ts = platform_get_drvdata(pdev);

	if (!ts || !ts->pmu)
		return;
	cancel_delayed_work_sync(&ts->chg_work);
	mt3351_chg_stop(ts, false);
}

static void mt3351_ts_remove(struct platform_device *pdev)
{
	mt3351_ts_shutdown(pdev);
}

static struct platform_driver mt3351_ts_driver = {
	.probe = mt3351_ts_probe,
	.remove = mt3351_ts_remove,
	.shutdown = mt3351_ts_shutdown,
	.driver = {
		.name = "mt3351-ts",
		.of_match_table = mt3351_ts_of_match,
	},
};
module_platform_driver(mt3351_ts_driver);

MODULE_DESCRIPTION("MediaTek MT3351 resistive touchscreen");
MODULE_LICENSE("GPL");
