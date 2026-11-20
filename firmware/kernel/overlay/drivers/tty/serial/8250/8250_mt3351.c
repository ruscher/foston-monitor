// SPDX-License-Identifier: GPL-2.0-only
/*
 * UARTs of the MediaTek MT3351 (FOSTON FS-707DC) on the generic 8250 core.
 *
 * From the Windows CE uart.dll of the device (docs/uart.md): a 16550 with
 * 4-byte register stride and byte access, clocked at 29.25 MHz, plus
 * MediaTek extras the 8250 core does not know. CE leaves UART1 (GPS) and
 * UART2 (Bluetooth) in its "highspeed" mode with receive DMA on, and their
 * GDMA channels running into its old buffers (0x007e8000, 0x007db000:
 * that is the memory corruption seen during the bring-up). Before handing
 * a port to the 8250 core this driver:
 *   - sets HIGHSPEED (+0x24) and DMA_EN (+0x4c) to 0 (plain 16x divisor),
 *   - stops the GDMA channel named by "mediatek,ce-rx-dma" (its START
 *     register, written 0).
 * Nothing else is written: no clock gates, no pin mux.
 */
#include <linux/io.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/serial_8250.h>
#include <linux/serial_core.h>

#define MT3351_UART_HIGHSPEED	0x24
#define MT3351_UART_DMA_EN	0x4c

struct mt3351_uart {
	int line;
};

static int mt3351_uart_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct uart_8250_port up = { };
	struct mt3351_uart *data;
	struct resource *res;
	void __iomem *base;
	u32 clk, dma;
	int irq;

	data = devm_kzalloc(dev, sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;
	base = devm_platform_get_and_ioremap_resource(pdev, 0, &res);
	if (IS_ERR(base))
		return PTR_ERR(base);
	irq = platform_get_irq(pdev, 0);
	if (irq < 0)
		return irq;
	if (of_property_read_u32(dev->of_node, "clock-frequency", &clk))
		return dev_err_probe(dev, -EINVAL, "clock-frequency missing\n");

	/* stop the receive DMA Windows CE left running into its own buffer */
	if (!of_property_read_u32(dev->of_node, "mediatek,ce-rx-dma", &dma)) {
		void __iomem *start = devm_ioremap(dev, dma, 4);

		if (start) {
			dev_info(dev, "CE receive DMA at %#x was %#x, stopped\n", dma, readl(start));
			writel(0, start);
		}
	}
	writeb(0, base + MT3351_UART_DMA_EN);
	writeb(0, base + MT3351_UART_HIGHSPEED);

	spin_lock_init(&up.port.lock);
	up.port.dev = dev;
	up.port.mapbase = res->start;
	up.port.membase = base;
	up.port.irq = irq;
	up.port.uartclk = clk;
	up.port.iotype = UPIO_MEM;
	up.port.regshift = 2;
	up.port.type = PORT_16550A;
	up.port.flags = UPF_FIXED_PORT | UPF_FIXED_TYPE | UPF_SKIP_TEST;
	up.port.fifosize = 16;

	data->line = serial8250_register_8250_port(&up);
	if (data->line < 0)
		return data->line;
	platform_set_drvdata(pdev, data);
	return 0;
}

static void mt3351_uart_remove(struct platform_device *pdev)
{
	struct mt3351_uart *data = platform_get_drvdata(pdev);

	serial8250_unregister_port(data->line);
}

static const struct of_device_id mt3351_uart_of_match[] = {
	{ .compatible = "mediatek,mt3351-uart" },
	{ }
};
MODULE_DEVICE_TABLE(of, mt3351_uart_of_match);

static struct platform_driver mt3351_uart_driver = {
	.probe = mt3351_uart_probe,
	.remove = mt3351_uart_remove,
	.driver = {
		.name = "mt3351-uart",
		.of_match_table = mt3351_uart_of_match,
	},
};
module_platform_driver(mt3351_uart_driver);

MODULE_DESCRIPTION("MediaTek MT3351 UART (8250 with CE hand-over fixups)");
MODULE_LICENSE("GPL");
