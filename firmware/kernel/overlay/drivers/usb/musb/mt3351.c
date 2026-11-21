// SPDX-License-Identifier: GPL-2.0+
/*
 * MediaTek MT3351 MUSB glue (FOSTON FS-707DC), BigFoston OS.
 *
 * The USB function block at 0x80100000 (IRQ 8) is a Mentor Graphics MUSBHDRC,
 * identified on the device by reading its configuration registers from
 * Windows CE: EPINFO 0x44 (4 TX + 4 RX endpoints), RAMINFO 0x4a (4 KB FIFO
 * RAM, 4 DMA channels), LINKINFO 0x5c and VPLEN 0x3c (MUSB reset defaults),
 * POWER 0x70 (high speed) and DEVCTL 0x91 (B-device, session) while CE was
 * enumerated by a PC.  docs/01-hardware.md.
 *
 * The PHY is left powered by Windows CE; it is modelled as a no-op
 * transceiver ("usb-nop-xceiv").  Peripheral only, PIO until the Inventra DMA
 * is validated on this board.  MediaTek MUSB traits seen on the device (as
 * handled by mediatek.c): interrupt status is write-1-to-clear and a bus
 * reset does not clear FADDR.
 */
#include <linux/dma-mapping.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/usb/usb_phy_generic.h>
#include <linux/workqueue.h>

#include "musb_core.h"

struct mt3351_glue {
	struct platform_device *musb;
};

/*
 * Bring-up trace, off unless booted with mt3351.trace=1: every interrupt
 * (first 64) and the controller state every 3 s on the console.
 */
static bool mt3351_trace;
module_param_named(trace, mt3351_trace, bool, 0444);
MODULE_PARM_DESC(trace, "print the USB bring-up trace on the console");

static unsigned int mt3351_irqs, mt3351_empty;
static struct musb *mt3351_musb;
static struct delayed_work mt3351_state_work;

/* compact event list repeated every 3 s, so a photo always catches it */
static char mt3351_ev[600];
static unsigned int mt3351_ev_len;

static void mt3351_event(const char *fmt, ...)
{
	va_list ap;

	if (mt3351_ev_len >= sizeof(mt3351_ev) - 40)
		return;
	va_start(ap, fmt);
	mt3351_ev_len += vscnprintf(mt3351_ev + mt3351_ev_len, sizeof(mt3351_ev) - mt3351_ev_len, fmt, ap);
	va_end(ap);
}

static void mt3351_state(struct work_struct *work)
{
	struct musb *musb = mt3351_musb;
	void __iomem *r = musb->mregs;
	unsigned long flags;
	u8 power, devctl, faddr, usbe, index, csr0l;
	u16 txe, rxe, frame;

	spin_lock_irqsave(&musb->lock, flags);
	power = musb_readb(r, MUSB_POWER);
	devctl = musb_readb(r, MUSB_DEVCTL);
	faddr = musb_readb(r, MUSB_FADDR);
	usbe = musb_readb(r, MUSB_INTRUSBE);
	txe = musb_readw(r, MUSB_INTRTXE);
	rxe = musb_readw(r, MUSB_INTRRXE);
	frame = musb_readw(r, MUSB_FRAME);
	index = musb_readb(r, MUSB_INDEX);
	musb_writeb(r, MUSB_INDEX, 0);
	csr0l = musb_readb(r, 0x12);
	musb_writeb(r, MUSB_INDEX, index);
	spin_unlock_irqrestore(&musb->lock, flags);
	pr_info("mt3351-usb: POWER %02x DEVCTL %02x FADDR %02x FRAME %u USBE %02x TXE %04x RXE %04x CSR0 %02x irqs %u (%u empty) %s\n",
		power, devctl, faddr, frame, usbe, txe, rxe, csr0l, mt3351_irqs, mt3351_empty,
		musb_otg_state_string(musb));
	for (unsigned int i = 0; i < mt3351_ev_len; i += 96)
		pr_info("mt3351-ev: %.96s\n", mt3351_ev + i);
	schedule_delayed_work(&mt3351_state_work, 3 * HZ);
}

/*
 * Reconnecting the cable re-enumerated the gadget (ep0 fine) but the network
 * endpoints never moved data again. The FIFO configuration of each endpoint is
 * photographed at the first interrupt (right after musb_core_init set it up),
 * compared after every bus reset, logged if the controller changed it and
 * written back before the gadget handles the reset.
 */
#define MT3351_EPS 5
static struct { u8 txsz, rxsz; u16 txadd, rxadd; } mt3351_fifo[MT3351_EPS];
static bool mt3351_fifo_saved;
static unsigned int st_reset, st_disc, st_susp, st_resume, st_fifo_fix, st_tx[8], st_rx[8];

static void mt3351_fifo_check(struct musb *musb, bool save)
{
	void __iomem *r = musb->mregs;
	u8 index = musb_readb(r, MUSB_INDEX);

	for (int i = 1; i < MT3351_EPS; i++) {
		u8 txsz, rxsz;
		u16 txadd, rxadd;

		musb_writeb(r, MUSB_INDEX, i);
		txsz = musb_readb(r, MUSB_TXFIFOSZ);
		rxsz = musb_readb(r, MUSB_RXFIFOSZ);
		txadd = musb_readw(r, MUSB_TXFIFOADD);
		rxadd = musb_readw(r, MUSB_RXFIFOADD);
		if (save) {
			mt3351_fifo[i].txsz = txsz;
			mt3351_fifo[i].rxsz = rxsz;
			mt3351_fifo[i].txadd = txadd;
			mt3351_fifo[i].rxadd = rxadd;
		} else if (txsz != mt3351_fifo[i].txsz || rxsz != mt3351_fifo[i].rxsz ||
			   txadd != mt3351_fifo[i].txadd || rxadd != mt3351_fifo[i].rxadd) {
			st_fifo_fix++;
			pr_info_ratelimited("mt3351-usb: ep%d FIFO changed by the reset (tx %02x@%04x rx %02x@%04x, was tx %02x@%04x rx %02x@%04x): restored\n",
					    i, txsz, txadd, rxsz, rxadd, mt3351_fifo[i].txsz,
					    mt3351_fifo[i].txadd, mt3351_fifo[i].rxsz, mt3351_fifo[i].rxadd);
			musb_writeb(r, MUSB_TXFIFOSZ, mt3351_fifo[i].txsz);
			musb_writeb(r, MUSB_RXFIFOSZ, mt3351_fifo[i].rxsz);
			musb_writew(r, MUSB_TXFIFOADD, mt3351_fifo[i].txadd);
			musb_writew(r, MUSB_RXFIFOADD, mt3351_fifo[i].rxadd);
		}
	}
	musb_writeb(r, MUSB_INDEX, index);
}

static void mt3351_count(unsigned int *c, u16 bits)
{
	for (int i = 0; i < 8; i++)
		if (bits & BIT(i))
			c[i]++;
}

/* /sys/module/mt3351/parameters/stats: interrupt counters and the FIFO setup now */
static int mt3351_stats_get(char *buf, const struct kernel_param *kp)
{
	struct musb *musb = mt3351_musb;
	int n;

	n = sysfs_emit(buf, "irqs %u empty %u reset %u disconnect %u suspend %u resume %u fifo_restored %u\n",
		       mt3351_irqs, mt3351_empty, st_reset, st_disc, st_susp, st_resume, st_fifo_fix);
	n += sysfs_emit_at(buf, n, "tx ep0-4 %u %u %u %u %u  rx ep1-4 %u %u %u %u\n",
			   st_tx[0], st_tx[1], st_tx[2], st_tx[3], st_tx[4],
			   st_rx[1], st_rx[2], st_rx[3], st_rx[4]);
	if (musb) {
		unsigned long flags;
		u8 index;

		spin_lock_irqsave(&musb->lock, flags);
		index = musb_readb(musb->mregs, MUSB_INDEX);
		for (int i = 1; i < MT3351_EPS; i++) {
			musb_writeb(musb->mregs, MUSB_INDEX, i);
			n += sysfs_emit_at(buf, n, "ep%d fifo tx %02x@%04x rx %02x@%04x txcsr %04x rxcsr %04x\n", i,
					   musb_readb(musb->mregs, MUSB_TXFIFOSZ),
					   musb_readw(musb->mregs, MUSB_TXFIFOADD),
					   musb_readb(musb->mregs, MUSB_RXFIFOSZ),
					   musb_readw(musb->mregs, MUSB_RXFIFOADD),
					   musb_readw(musb->mregs, 0x10 + MUSB_TXCSR),
					   musb_readw(musb->mregs, 0x10 + MUSB_RXCSR));
		}
		musb_writeb(musb->mregs, MUSB_INDEX, index);
		spin_unlock_irqrestore(&musb->lock, flags);
	}
	return n;
}

static const struct kernel_param_ops mt3351_stats_ops = { .get = mt3351_stats_get };
module_param_cb(stats, &mt3351_stats_ops, NULL, 0444);
MODULE_PARM_DESC(stats, "USB interrupt counters and endpoint FIFO setup (read only)");

static irqreturn_t mt3351_musb_interrupt(int irq, void *__hci)
{
	struct musb *musb = __hci;
	irqreturn_t ret = IRQ_NONE;
	unsigned long flags;

	spin_lock_irqsave(&musb->lock, flags);
	/* MediaTek MUSB: interrupt status is write-1-to-clear (as in mediatek.c) */
	musb->int_usb = musb_clearb(musb->mregs, MUSB_INTRUSB);
	musb->int_rx = musb_clearw(musb->mregs, MUSB_INTRRX);
	musb->int_tx = musb_clearw(musb->mregs, MUSB_INTRTX);
	if (!mt3351_fifo_saved) {		/* FIFOs as musb_core_init configured them */
		mt3351_fifo_check(musb, true);
		mt3351_fifo_saved = true;
	}
	if (musb->int_usb & MUSB_INTR_DISCONNECT)
		st_disc++;
	if (musb->int_usb & MUSB_INTR_SUSPEND)
		st_susp++;
	if (musb->int_usb & MUSB_INTR_RESUME)
		st_resume++;
	mt3351_count(st_tx, musb->int_tx);
	mt3351_count(st_rx, musb->int_rx);
	/* ...and FADDR is not cleared by a bus reset (FADDR stayed at CE's 0x04) */
	if ((musb->int_usb & MUSB_INTR_RESET) && !is_host_active(musb)) {
		st_reset++;
		mt3351_fifo_check(musb, false);
		musb_ep_select(musb->mregs, 0);
		musb_writeb(musb->mregs, MUSB_FADDR, 0);
	}
	if (mt3351_trace && mt3351_irqs < 40)
		mt3351_event("I%x/%x/%x ", musb->int_usb, musb->int_tx, musb->int_rx);
	if (++mt3351_irqs <= 64 && mt3351_trace)
		pr_info("mt3351-usb: irq %u usb %02x tx %04x rx %04x power %02x devctl %02x faddr %02x\n",
			mt3351_irqs, musb->int_usb, musb->int_tx, musb->int_rx,
			musb_readb(musb->mregs, MUSB_POWER), musb_readb(musb->mregs, MUSB_DEVCTL),
			musb_readb(musb->mregs, MUSB_FADDR));
	if (musb->int_usb || musb->int_tx || musb->int_rx)
		ret = musb_interrupt(musb);
	else
		mt3351_empty++;
	spin_unlock_irqrestore(&musb->lock, flags);
	return ret;
}

/*
 * FIFO access one byte at a time (works on every MUSB bus width) with a
 * trace of the first ep0 packets: the PC got a STALL on GET_DESCRIPTOR,
 * so show what the controller actually read from the SETUP packet.
 */
static unsigned int mt3351_ep0_logged;

static void mt3351_trace_fifo(const char *dir, struct musb_hw_ep *hw_ep, u16 len, const u8 *buf)
{
	if (!mt3351_trace || hw_ep->epnum || mt3351_ep0_logged >= 24)
		return;
	mt3351_ep0_logged++;
	mt3351_event("%s%u:%*phN ", dir, len, min_t(int, len, 8), buf);
	pr_info("mt3351-usb: ep0 %s %u: %*ph\n", dir, len, min_t(int, len, 18), buf);
}

static void mt3351_read_fifo(struct musb_hw_ep *hw_ep, u16 len, u8 *dst)
{
	void __iomem *fifo = hw_ep->fifo;
	u16 i;

	for (i = 0; i < len; i++)
		dst[i] = readb(fifo);
	mt3351_trace_fifo("rx", hw_ep, len, dst);
}

static void mt3351_write_fifo(struct musb_hw_ep *hw_ep, u16 len, const u8 *src)
{
	void __iomem *fifo = hw_ep->fifo;
	u16 i;

	for (i = 0; i < len; i++)
		writeb(src[i], fifo);
	mt3351_trace_fifo("tx", hw_ep, len, src);
}

/* 4 KB of FIFO RAM (RAMINFO 0x4a: 2^10 words); ep0 uses the first 64 bytes */
static struct musb_fifo_cfg mt3351_fifo_cfg[] = {
	{ .hw_ep_num = 1, .style = FIFO_TX, .maxpacket = 512, },
	{ .hw_ep_num = 1, .style = FIFO_RX, .maxpacket = 512, },
	{ .hw_ep_num = 2, .style = FIFO_TX, .maxpacket = 512, },
	{ .hw_ep_num = 2, .style = FIFO_RX, .maxpacket = 512, },
	{ .hw_ep_num = 3, .style = FIFO_TX, .maxpacket = 512, },
	{ .hw_ep_num = 3, .style = FIFO_RX, .maxpacket = 512, },
	{ .hw_ep_num = 4, .style = FIFO_TX, .maxpacket = 64, },
	{ .hw_ep_num = 4, .style = FIFO_RX, .maxpacket = 64, },
};

static const struct musb_hdrc_config mt3351_musb_config = {
	.multipoint	= 0,
	.num_eps	= 5,	/* ep0 + 4 (EPINFO 0x44) */
	.ram_bits	= 10,
	/*
	 * High speed chirps fine but every transaction then fails on the PC
	 * (EPROTO, "device descriptor read/64, error -71"): the PHY is not
	 * tuned for HS after the hand-over from CE.  Full speed for now.
	 */
	.maximum_speed	= USB_SPEED_FULL,
	.fifo_cfg	= mt3351_fifo_cfg,
	.fifo_cfg_size	= ARRAY_SIZE(mt3351_fifo_cfg),
};

static int mt3351_musb_init(struct musb *musb)
{
	struct device *dev = musb->controller->parent;

	musb->xceiv = devm_usb_get_phy_by_phandle(dev, "phys", 0);
	if (IS_ERR(musb->xceiv))
		return dev_err_probe(dev, PTR_ERR(musb->xceiv), "no transceiver\n");

	dev_info(dev, "MUSB at %p: POWER %02x DEVCTL %02x EPINFO %02x RAMINFO %02x\n",
		 musb->mregs, musb_readb(musb->mregs, MUSB_POWER),
		 musb_readb(musb->mregs, MUSB_DEVCTL),
		 musb_readb(musb->mregs, MUSB_EPINFO),
		 musb_readb(musb->mregs, MUSB_RAMINFO));

	/* Windows CE left the controller connected and enumerated: drop off the bus first */
	musb_writeb(musb->mregs, MUSB_POWER,
		    musb_readb(musb->mregs, MUSB_POWER) & ~MUSB_POWER_SOFTCONN);
	musb_writew(musb->mregs, MUSB_INTRTXE, 0);
	musb_writew(musb->mregs, MUSB_INTRRXE, 0);
	musb_writeb(musb->mregs, MUSB_INTRUSBE, 0);
	/* stop the Inventra DMA channels CE may have left running (CNTL at 0x204 + 0x10 * ch) */
	for (int ch = 0; ch < 4; ch++)
		musb_writew(musb->mregs, 0x204 + 0x10 * ch, 0);
	musb_writeb(musb->mregs, MUSB_INTRUSB, 0xff);	/* write-1-to-clear */
	musb_writew(musb->mregs, MUSB_INTRTX, 0xffff);
	musb_writew(musb->mregs, MUSB_INTRRX, 0xffff);
	musb_writeb(musb->mregs, MUSB_FADDR, 0);

	musb->dyn_fifo = true;
	musb->isr = mt3351_musb_interrupt;

	mt3351_musb = musb;
	if (mt3351_trace) {
		INIT_DELAYED_WORK(&mt3351_state_work, mt3351_state);
		schedule_delayed_work(&mt3351_state_work, 3 * HZ);
	}
	return 0;
}

static u8 mt3351_clearb(void __iomem *addr, unsigned int offset)
{
	u8 data = musb_readb(addr, offset);

	musb_writeb(addr, offset, data);
	return data;
}

static u16 mt3351_clearw(void __iomem *addr, unsigned int offset)
{
	u16 data = musb_readw(addr, offset);

	musb_writew(addr, offset, data);
	return data;
}

static const struct musb_platform_ops mt3351_musb_ops = {
	.quirks		= MUSB_INDEXED_EP,
	.fifo_mode	= 2,
	.init		= mt3351_musb_init,
	.clearb		= mt3351_clearb,
	.clearw		= mt3351_clearw,
	.read_fifo	= mt3351_read_fifo,
	.write_fifo	= mt3351_write_fifo,
};

static const struct musb_hdrc_platform_data mt3351_musb_pdata = {
	.mode		= MUSB_PERIPHERAL,
	.config		= &mt3351_musb_config,
	.platform_ops	= &mt3351_musb_ops,
};

static int mt3351_musb_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct mt3351_glue *glue;
	struct platform_device *musb;
	int ret;

	glue = devm_kzalloc(dev, sizeof(*glue), GFP_KERNEL);
	if (!glue)
		return -ENOMEM;

	musb = platform_device_alloc("musb-hdrc", PLATFORM_DEVID_AUTO);
	if (!musb)
		return -ENOMEM;

	musb->dev.parent = dev;
	musb->dev.dma_mask = &musb->dev.coherent_dma_mask;
	musb->dev.coherent_dma_mask = DMA_BIT_MASK(32);
	device_set_of_node_from_dev(&musb->dev, dev);
	glue->musb = musb;
	platform_set_drvdata(pdev, glue);

	ret = platform_device_add_resources(musb, pdev->resource, pdev->num_resources);
	if (!ret)
		ret = platform_device_add_data(musb, &mt3351_musb_pdata, sizeof(mt3351_musb_pdata));
	if (!ret)
		ret = platform_device_add(musb);
	if (ret) {
		dev_err(dev, "failed to register musb device: %d\n", ret);
		platform_device_put(musb);
	}
	return ret;
}

static void mt3351_musb_remove(struct platform_device *pdev)
{
	struct mt3351_glue *glue = platform_get_drvdata(pdev);

	platform_device_unregister(glue->musb);
}

static const struct of_device_id mt3351_musb_of_match[] = {
	{ .compatible = "mediatek,mt3351-musb" },
	{ /* sentinel */ },
};
MODULE_DEVICE_TABLE(of, mt3351_musb_of_match);

static struct platform_driver mt3351_musb_driver = {
	.probe		= mt3351_musb_probe,
	.remove_new	= mt3351_musb_remove,
	.driver		= {
		.name	= "musb-mt3351",
		.of_match_table = mt3351_musb_of_match,
	},
};
module_platform_driver(mt3351_musb_driver);

MODULE_DESCRIPTION("MediaTek MT3351 MUSB glue");
MODULE_LICENSE("GPL");
