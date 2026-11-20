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
 * is validated on this board.
 */
#include <linux/dma-mapping.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/usb/usb_phy_generic.h>

#include "musb_core.h"

struct mt3351_glue {
	struct platform_device *musb;
};

static irqreturn_t mt3351_musb_interrupt(int irq, void *__hci)
{
	struct musb *musb = __hci;
	irqreturn_t ret = IRQ_NONE;
	unsigned long flags;

	spin_lock_irqsave(&musb->lock, flags);
	musb->int_usb = musb_readb(musb->mregs, MUSB_INTRUSB);
	musb->int_tx = musb_readw(musb->mregs, MUSB_INTRTX);
	musb->int_rx = musb_readw(musb->mregs, MUSB_INTRRX);
	if (musb->int_usb || musb->int_tx || musb->int_rx)
		ret = musb_interrupt(musb);
	spin_unlock_irqrestore(&musb->lock, flags);
	return ret;
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
	musb_readb(musb->mregs, MUSB_INTRUSB);
	musb_readw(musb->mregs, MUSB_INTRTX);
	musb_readw(musb->mregs, MUSB_INTRRX);

	musb->dyn_fifo = true;
	musb->isr = mt3351_musb_interrupt;
	return 0;
}

static const struct musb_platform_ops mt3351_musb_ops = {
	.quirks		= MUSB_INDEXED_EP,
	.fifo_mode	= 2,
	.init		= mt3351_musb_init,
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
