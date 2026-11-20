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
#include <linux/kthread.h>
#include <linux/sched/signal.h>
#include <linux/sched/task.h>
#include <linux/sched/cputime.h>
#include <linux/sched/task_stack.h>
#include <linux/mm.h>
#include <linux/math64.h>
#include <linux/gfp.h>
#include <linux/highmem.h>
#include <linux/delay.h>
#include <linux/jiffies.h>
#include <linux/init.h>

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
static unsigned int gpt_ticks;		/* interrupts handled, for the bring-up self-check */

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
	gpt_ticks++;
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

/*
 * Bring-up self-check, printed on the framebuffer console: are timer
 * interrupts arriving (jiffies and our counter move during mdelay), does
 * sleeping work, and what did the kernel leave in the CP15 control register
 * (U = bit 22 unaligned access, A = bit 1 alignment trap).
 */
static int __init mt3351_gpt_selfcheck(void)
{
	static u8 buf[8] __aligned(4) = {1, 2, 3, 4, 5, 6, 7, 8};
	volatile u32 *odd = (volatile u32 *)(buf + 1);
	unsigned long j0 = jiffies;
	unsigned int t0 = gpt_ticks;
	u32 sctlr;

	if (!gpt_base)
		return 0;
	asm volatile("mrc p15, 0, %0, c1, c0, 0" : "=r"(sctlr));
	pr_info("selfcheck: HZ=%d jiffies=%lu ticks=%u sctlr=%08x (U=%d A=%d)\n", HZ, j0, t0, sctlr,
		!!(sctlr & BIT(22)), !!(sctlr & BIT(1)));
	pr_info("selfcheck: unaligned u32 at +1 = %08x (expected 05040302)\n", *odd);
	mdelay(200);
	pr_info("selfcheck: after mdelay(200): jiffies +%lu ticks +%u\n", jiffies - j0, gpt_ticks - t0);
	msleep(200);
	pr_info("selfcheck: after msleep(200): jiffies +%lu ticks +%u\n", jiffies - j0, gpt_ticks - t0);
	return 0;
}
late_initcall(mt3351_gpt_selfcheck);

/*
 * Bring-up watcher: every 5 s, where is PID 1 and what is it doing? Shows
 * a user process that is stuck (fault loop, sleep, spin) without a serial
 * console or input. Printed on the framebuffer console.
 */
static int bfwatch_thread(void *unused)
{
	while (!kthread_should_stop()) {
		struct task_struct *init;
		struct pt_regs *regs;
		u64 ut = 0, st = 0;

		ssleep(5);
		rcu_read_lock();
		init = find_task_by_vpid(1);
		if (init)
			get_task_struct(init);
		rcu_read_unlock();
		if (!init)
			continue;
		task_cputime(init, &ut, &st);
		regs = task_pt_regs(init);
		pr_info("bfwatch: %s state=%c pc=%08lx lr=%08lx sp=%08lx user=%llums sys=%llums minflt=%lu majflt=%lu csw=%lu/%lu free=%luK\n",
			init->comm, task_state_to_char(init), regs->ARM_pc, regs->ARM_lr, regs->ARM_sp,
			div_u64(ut, NSEC_PER_MSEC), div_u64(st, NSEC_PER_MSEC), init->min_flt, init->maj_flt,
			init->nvcsw, init->nivcsw, global_zone_page_state(NR_FREE_PAGES) << (PAGE_SHIFT - 10));
		put_task_struct(init);
	}
	return 0;
}

static int __init bfwatch_start(void)
{
	kthread_run(bfwatch_thread, NULL, "bfwatch");
	return 0;
}
late_initcall_sync(bfwatch_start);

/*
 * Bring-up memory watcher: who writes into RAM behind Linux's back? Fills
 * all free memory but 16 MB (1 MB blocks) with a pattern derived from each
 * word's physical address and reports, every 5 s, the words that changed
 * (physical address and value). A bus master left running by Windows CE
 * (DMA into one of its old buffers) shows up as a fixed address range.
 */
#define MEMWATCH_BLOCKS 128			/* up to 128 MB, stops leaving MEMWATCH_KEEP free */
#define MEMWATCH_KEEP_PAGES (16u << (20 - PAGE_SHIFT))
#define MEMWATCH_ORDER  8			/* 256 pages = 1 MB */

static inline u32 memwatch_pattern(phys_addr_t pa)
{
	return (u32)pa ^ 0xA5C3B5E1u;
}

static struct page *memwatch_blocks[MEMWATCH_BLOCKS];
static unsigned int memwatch_n;

static int memwatch_thread(void *unused)
{
	unsigned int b, reports = 0;
	size_t words = (PAGE_SIZE << MEMWATCH_ORDER) / 4, i;

	while (!kthread_should_stop()) {
		unsigned int bad = 0;

		ssleep(5);
		for (b = 0; b < memwatch_n; b++) {
			u32 *w = page_address(memwatch_blocks[b]);
			phys_addr_t pa = page_to_phys(memwatch_blocks[b]);

			for (i = 0; i < words; i++) {
				u32 want = memwatch_pattern(pa + i * 4);

				if (w[i] == want)
					continue;
				if (bad < 6 && reports < 60)
					pr_warn("memwatch: CHANGED pa=0x%08llx now=%08x was=%08x\n",
						(u64)(pa + i * 4), w[i], want);
				bad++;
				w[i] = want;			/* re-arm to see whether it keeps changing */
			}
		}
		if (bad) {
			reports++;
			pr_warn("memwatch: %u words changed in the last 5 s\n", bad);
		} else if (reports < 60) {
			pr_info("memwatch: no change (%u MB)\n", memwatch_n);
		}
	}
	return 0;
}

/*
 * Fill at the earliest initcall level, before the initramfs is unpacked
 * and before any user process exists: everything userland later gets comes
 * from the 16 MB left, and a stray bus master writing anywhere else shows up.
 */
static int __init memwatch_fill(void)
{
	size_t words = (PAGE_SIZE << MEMWATCH_ORDER) / 4, i;
	unsigned int b;

	for (b = 0; b < MEMWATCH_BLOCKS; b++) {
		u32 *w;
		phys_addr_t pa;

		if (global_zone_page_state(NR_FREE_PAGES) < MEMWATCH_KEEP_PAGES + (1u << MEMWATCH_ORDER))
			break;
		memwatch_blocks[b] = alloc_pages(GFP_KERNEL | __GFP_NOWARN, MEMWATCH_ORDER);
		if (!memwatch_blocks[b])
			break;
		w = page_address(memwatch_blocks[b]);
		pa = page_to_phys(memwatch_blocks[b]);
		for (i = 0; i < words; i++)
			w[i] = memwatch_pattern(pa + i * 4);
		memwatch_n++;
	}
	pr_info("memwatch: %u MB filled before the initramfs\n", memwatch_n);
	return 0;
}
early_initcall(memwatch_fill);

static int __init memwatch_start(void)
{
	kthread_run(memwatch_thread, NULL, "memwatch");
	return 0;
}
late_initcall_sync(memwatch_start);

/*
 * Alignment trap, on by default.  Measured on the FS-707DC (bftest=bytes):
 * a halfword store to an odd address right after a copy-on-write fault
 * loses the byte store made just before it to the same word (400 of 400
 * with musl's memset pattern "strb b0; strh b1-b2").  With SCTLR.A set every
 * unaligned access faults and arch/arm/mm/alignment.c redoes it with byte
 * accesses, which removes the corruption (0 of 400).  Applied at
 * fs_initcall_sync, right after alignment_init() cleared the bit and before
 * the initramfs is unpacked.  Run-time switch:
 * /sys/module/timer_mt3351_gpt/parameters/align_trap
 */
#include <linux/moduleparam.h>
#include <asm/cp15.h>

static bool align_trap = true;

static void align_trap_apply(void)
{
	unsigned long cr = align_trap ? (cr_alignment | CR_A) : (cr_alignment & ~CR_A);

	cr_alignment = cr;	/* reloaded into SCTLR on every entry from user space */
	set_cr(cr);
	isb();
	pr_info("mt3351: alignment trap %s (SCTLR %08lx)\n", align_trap ? "ON" : "OFF", get_cr());
}

static int align_trap_set(const char *val, const struct kernel_param *kp)
{
	int ret = param_set_bool(val, kp);

	if (!ret && system_state == SYSTEM_RUNNING)
		align_trap_apply();
	return ret;
}

static const struct kernel_param_ops align_trap_ops = {
	.set = align_trap_set,
	.get = param_get_bool,
};
module_param_cb(align_trap, &align_trap_ops, &align_trap, 0644);

static int __init align_trap_init(void)
{
	align_trap_apply();
	return 0;
}
fs_initcall_sync(align_trap_init);
