// SPDX-License-Identifier: GPL-2.0-only
/*
 * MediaTek MT3351 GPT (general purpose timer), FOSTON FS-707DC / BigFoston OS.
 *
 * Measured on the device (docs/01-hardware.md):
 *   0x00  IRQ_EN    bit n = channel n+1 interrupt enable (Windows CE: 0x5)
 *   0x04  IRQ_STA
 *   0x08  IRQ_ACK   write 1 to acknowledge
 *   0x10  GPT1_CTRL 0x21 = enable | KEEP_GO (counts on after a compare match)
 *   0x14  GPT1_CLK  0x22 as left by the boot loader: 14.625 MHz (468 MHz / 32)
 *   0x18  GPT1_CNT  free-running 32-bit up counter
 *   0x1c  GPT1_CMP  compare value, raises line 39 of the CIRQ
 *
 * The clock register encoding of this SoC is not known, so it is left as
 * the boot loader programmed it and the rate comes from the device tree.
 * One channel serves as clocksource, sched_clock and one-shot clockevent.
 */
#include <linux/clockchips.h>
#include <linux/clocksource.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/irq.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_irq.h>
#include <linux/sched_clock.h>

#define GPT_IRQ_EN		0x00
#define GPT_IRQ_STA		0x04
#define GPT_IRQ_ACK		0x08
#define GPT1_CTRL		0x10
#define GPT1_CNT		0x18
#define GPT1_CMP		0x1c
#define GPT_CTRL_ENABLE		BIT(0)
#define GPT_CTRL_KEEP_GO	(2 << 4)
#define GPT1_BIT		BIT(0)
#define GPT_MIN_DELTA		64

static void __iomem *gpt_base;

static u64 notrace gpt_sched_read(void)
{
	return readl_relaxed(gpt_base + GPT1_CNT);
}

static u64 gpt_cs_read(struct clocksource *cs)
{
	return readl_relaxed(gpt_base + GPT1_CNT);
}

static struct clocksource gpt_clocksource = {
	.name	= "mt3351-gpt",
	.rating	= 300,
	.read	= gpt_cs_read,
	.mask	= CLOCKSOURCE_MASK(32),
	.flags	= CLOCK_SOURCE_IS_CONTINUOUS,
};

static int gpt_set_next_event(unsigned long delta, struct clock_event_device *evt)
{
	u32 target = readl_relaxed(gpt_base + GPT1_CNT) + delta;

	writel_relaxed(target, gpt_base + GPT1_CMP);
	writel_relaxed(readl_relaxed(gpt_base + GPT_IRQ_EN) | GPT1_BIT, gpt_base + GPT_IRQ_EN);
	/* the counter must not have passed the target already */
	if ((s32)(readl_relaxed(gpt_base + GPT1_CNT) - target) >= 0)
		return -ETIME;
	return 0;
}

static int gpt_shutdown(struct clock_event_device *evt)
{
	writel_relaxed(readl_relaxed(gpt_base + GPT_IRQ_EN) & ~GPT1_BIT, gpt_base + GPT_IRQ_EN);
	writel_relaxed(GPT1_BIT, gpt_base + GPT_IRQ_ACK);
	return 0;
}

static struct clock_event_device gpt_clockevent = {
	.name			= "mt3351-gpt",
	.features		= CLOCK_EVT_FEAT_ONESHOT,
	.rating			= 300,
	.set_next_event		= gpt_set_next_event,
	.set_state_shutdown	= gpt_shutdown,
	.set_state_oneshot	= gpt_shutdown,
	.tick_resume		= gpt_shutdown,
};

static irqreturn_t gpt_interrupt(int irq, void *dev_id)
{
	struct clock_event_device *evt = dev_id;

	writel_relaxed(GPT1_BIT, gpt_base + GPT_IRQ_ACK);
	evt->event_handler(evt);
	return IRQ_HANDLED;
}

static int __init mt3351_gpt_init(struct device_node *node)
{
	u32 rate;
	int irq, ret;

	if (of_property_read_u32(node, "clock-frequency", &rate) || !rate) {
		pr_err("mt3351-gpt: clock-frequency missing\n");
		return -EINVAL;
	}
	gpt_base = of_iomap(node, 0);
	if (!gpt_base)
		return -ENOMEM;
	irq = irq_of_parse_and_map(node, 0);
	if (!irq) {
		iounmap(gpt_base);
		return -EINVAL;
	}

	/* every channel interrupt off (Windows CE also used channel 3), pending cleared */
	writel_relaxed(0, gpt_base + GPT_IRQ_EN);
	writel_relaxed(0xff, gpt_base + GPT_IRQ_ACK);
	/* keep counting through compare matches; the clock setting stays untouched */
	writel_relaxed(GPT_CTRL_ENABLE | GPT_CTRL_KEEP_GO, gpt_base + GPT1_CTRL);

	sched_clock_register(gpt_sched_read, 32, rate);
	ret = clocksource_register_hz(&gpt_clocksource, rate);
	if (ret)
		return ret;

	ret = request_irq(irq, gpt_interrupt, IRQF_TIMER | IRQF_IRQPOLL, "mt3351-gpt", &gpt_clockevent);
	if (ret)
		return ret;
	gpt_clockevent.cpumask = cpu_possible_mask;
	gpt_clockevent.irq = irq;
	clockevents_config_and_register(&gpt_clockevent, rate, GPT_MIN_DELTA, 0x7fffffff);
	pr_info("mt3351-gpt: %u Hz, irq %d, counter %08x\n", rate, irq, readl_relaxed(gpt_base + GPT1_CNT));
	return 0;
}
TIMER_OF_DECLARE(mt3351_gpt, "mediatek,mt3351-gpt", mt3351_gpt_init);
