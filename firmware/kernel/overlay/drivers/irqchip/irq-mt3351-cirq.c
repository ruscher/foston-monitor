// SPDX-License-Identifier: GPL-2.0-only
/*
 * MediaTek MT3351 CIRQ interrupt controller (FOSTON FS-707DC, BigFoston OS).
 *
 * 64 interrupt lines; the layout matches the MT62xx/MT6516 "CIRQ" block.
 * Every register below was taken from the Windows CE 5 OAL of the device
 * (OALInterruptInit, SetIRQMask, ClearIRQMask, OEMInterruptHandler) and
 * checked against the live device; see docs/01-hardware.md.
 *
 *   0x00..0x30  IRQ_SEL0..12   priority slot -> source, 5 x 6-bit fields per word
 *   0x38/0x3c   IRQ_MASK L/H   current mask (1 = masked)
 *   0x40/0x44   IRQ_MASK_CLR   write 1 to unmask
 *   0x48/0x4c   IRQ_MASK_SET   write 1 to mask
 *   0x60/0x64   IRQ_SENS L/H   1 = level, 0 = edge
 *   0x78        IRQ_STA2       code of the highest pending line, bit 8 = none
 *   0x7c        IRQ_EOI2       write the code back to end the interrupt
 *
 * IRQ_SEL is programmed as the identity (slot n = line n), as Windows CE
 * does, so the code read from IRQ_STA2 is the hardware line number.
 */
#include <linux/bitops.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/irq.h>
#include <linux/irqchip.h>
#include <linux/irqdomain.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <asm/exception.h>

#define CIRQ_SEL(n)		(0x00 + 4 * (n))
#define CIRQ_SEL_WORDS		13
#define CIRQ_MASK(h)		(0x38 + 4 * (h))
#define CIRQ_MASK_CLR(h)	(0x40 + 4 * (h))
#define CIRQ_MASK_SET(h)	(0x48 + 4 * (h))
#define CIRQ_SENS(h)		(0x60 + 4 * (h))
#define CIRQ_STA2		0x78
#define CIRQ_EOI2		0x7c
#define CIRQ_STA2_NONE		BIT(8)
#define CIRQ_LINES		64

static void __iomem *cirq_base;
static struct irq_domain *cirq_domain;

static inline u32 line_bit(irq_hw_number_t hw)
{
	return BIT(hw & 31);
}

static void cirq_mask(struct irq_data *d)
{
	writel_relaxed(line_bit(d->hwirq), cirq_base + CIRQ_MASK_SET(d->hwirq >> 5));
}

static void cirq_unmask(struct irq_data *d)
{
	writel_relaxed(line_bit(d->hwirq), cirq_base + CIRQ_MASK_CLR(d->hwirq >> 5));
}

static void cirq_eoi(struct irq_data *d)
{
	writel_relaxed(d->hwirq, cirq_base + CIRQ_EOI2);
}

static int cirq_set_type(struct irq_data *d, unsigned int type)
{
	void __iomem *reg = cirq_base + CIRQ_SENS(d->hwirq >> 5);
	u32 sens = readl_relaxed(reg);

	/* the block has no polarity control: only level versus edge */
	if (type & IRQ_TYPE_LEVEL_MASK) {
		sens |= line_bit(d->hwirq);
		irq_set_handler_locked(d, handle_fasteoi_irq);
	} else if (type & IRQ_TYPE_EDGE_BOTH) {
		sens &= ~line_bit(d->hwirq);
		irq_set_handler_locked(d, handle_fasteoi_irq);
	} else {
		return -EINVAL;
	}
	writel_relaxed(sens, reg);
	return IRQ_SET_MASK_OK;
}

static struct irq_chip cirq_chip = {
	.name		= "CIRQ",
	.irq_mask	= cirq_mask,
	.irq_unmask	= cirq_unmask,
	.irq_eoi	= cirq_eoi,
	.irq_set_type	= cirq_set_type,
	.flags		= IRQCHIP_SKIP_SET_WAKE,
};

static int cirq_map(struct irq_domain *d, unsigned int irq, irq_hw_number_t hw)
{
	irq_set_chip_and_handler(irq, &cirq_chip, handle_fasteoi_irq);
	irq_set_probe(irq);
	return 0;
}

static const struct irq_domain_ops cirq_domain_ops = {
	.map	= cirq_map,
	.xlate	= irq_domain_xlate_twocell,
};

static void __exception_irq_entry cirq_handle_irq(struct pt_regs *regs)
{
	u32 code;

	for (;;) {
		code = readl_relaxed(cirq_base + CIRQ_STA2);
		if (code & CIRQ_STA2_NONE)
			break;
		code &= 0x3f;
		if (generic_handle_domain_irq(cirq_domain, code)) {
			/* unexpected line: mask it so it cannot storm, then end it */
			writel_relaxed(BIT(code & 31), cirq_base + CIRQ_MASK_SET(code >> 5));
			writel_relaxed(code, cirq_base + CIRQ_EOI2);
		}
	}
}

/* Packs slots 5n..5n+4 -> lines 5n..5n+4 (identity), exactly the values Windows CE writes. */
static void cirq_select_identity(void)
{
	unsigned int word, field;

	for (word = 0; word < CIRQ_SEL_WORDS; word++) {
		u32 v = 0;

		for (field = 0; field < 5; field++) {
			unsigned int line = word * 5 + field;

			if (line < CIRQ_LINES)
				v |= line << (6 * field);
		}
		writel_relaxed(v, cirq_base + CIRQ_SEL(word));
	}
}

/* drivers/video/fbdev/mt3351-bootcon.c overrides this when CONFIG_MT3351_BOOTCON=y */
void __init mt3351_earlyfb_register(void);

void __init __weak mt3351_earlyfb_register(void)
{
}

static int __init mt3351_cirq_of_init(struct device_node *node, struct device_node *parent)
{
	unsigned int i;

	/* our framebuffer console comes first so that any hang below is visible */
	mt3351_earlyfb_register();

	cirq_base = of_iomap(node, 0);
	if (!cirq_base)
		return -ENOMEM;

	/* everything masked; Windows CE left its own sources enabled */
	writel_relaxed(~0u, cirq_base + CIRQ_MASK_SET(0));
	writel_relaxed(~0u, cirq_base + CIRQ_MASK_SET(1));
	cirq_select_identity();
	/* end whatever Windows CE left in service (bounded: a masked line may still show) */
	for (i = 0; i < CIRQ_LINES; i++) {
		u32 code = readl_relaxed(cirq_base + CIRQ_STA2);

		if (code & CIRQ_STA2_NONE)
			break;
		writel_relaxed(code & 0x3f, cirq_base + CIRQ_EOI2);
	}

	cirq_domain = irq_domain_add_linear(node, CIRQ_LINES, &cirq_domain_ops, NULL);
	if (!cirq_domain) {
		iounmap(cirq_base);
		return -ENOMEM;
	}
	set_handle_irq(cirq_handle_irq);
	pr_info("mt3351-cirq: %d lines at %pOF, sens %08x %08x\n", CIRQ_LINES, node,
		readl_relaxed(cirq_base + CIRQ_SENS(1)), readl_relaxed(cirq_base + CIRQ_SENS(0)));
	return 0;
}
IRQCHIP_DECLARE(mt3351_cirq, "mediatek,mt3351-cirq", mt3351_cirq_of_init);
