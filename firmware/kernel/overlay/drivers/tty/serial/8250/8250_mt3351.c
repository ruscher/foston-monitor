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
 *   - sets DMA_EN (+0x4c) to 0,
 *   - stops the GDMA channel named by "mediatek,ce-rx-dma" (its START
 *     register, written 0).
 * Nothing else is written: no clock gates, no pin mux.
 *
 * Baud rate: programmed exactly as uart.dll does (function at VA 0x3d31ad0,
 * for every rate up to 921600): HIGHSPEED = 3, divisor = (clk / baud) / 256
 * + 1, SAMPLE_COUNT = round(clk / (divisor * baud)) - 1, SAMPLE_POINT =
 * (SAMPLE_COUNT - 1) / 2, RATE_FIX = 1. CE leaves RATE_FIX at 1 and the plain
 * 16x mode of the 8250 core was never proven with it (the GPS boot loader
 * did not answer at all). Checked against CE's own values at 921600:
 * divisor 1, SAMPLE_COUNT 31, SAMPLE_POINT 15.
 *
 * Batched receive on ports with RTS/CTS (Bluetooth, docs/bluetooth-audio-
 * performance/08-uart-hci.md): the 8250 core pushes the tty flip buffer on
 * every interrupt, and each push wakes a kworker that runs the whole HCI
 * receive path for ~14 bytes. With A2DP at ~42 KB/s that was ~3000 wakeups/s
 * and ~33 % of the CPU in kernel threads. Here the FIFO is still drained on
 * every interrupt, but the buffer is pushed only after MT3351_BATCH bytes, on
 * a receive timeout interrupt (line idle) or by a 2 ms timer.
 */
#include <linux/hrtimer.h>
#include <linux/io.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/serial_8250.h>
#include <linux/serial_core.h>
#include <linux/tty_flip.h>

#include "8250.h"

#define MT3351_UART_HIGHSPEED	0x24
#define MT3351_UART_SAMPLE_COUNT 0x28
#define MT3351_UART_SAMPLE_POINT 0x2c
#define MT3351_UART_RATE_FIX	0x34
#define MT3351_UART_DMA_EN	0x4c
#define MT3351_UART_EFR		0x08	/* with LCR = 0xBF */
#define MT3351_EFR_AUTO_RTS	0x40
#define MT3351_EFR_AUTO_CTS	0x80
#define MT3351_BATCH		256	/* bytes per tty push in batched mode */
#define MT3351_FLUSH_NS		(2 * NSEC_PER_MSEC)

struct mt3351_uart {
	int line;
	struct uart_port *port;
	struct hrtimer flush;
	bool batch;			/* RTS/CTS on: batched receive */
	unsigned int pending;		/* bytes read but not pushed yet */
};

/* port lock held */
static bool mt3351_take_pending(struct mt3351_uart *d)
{
	bool push = d->pending != 0;

	d->pending = 0;
	return push;
}

static enum hrtimer_restart mt3351_flush_timer(struct hrtimer *t)
{
	struct mt3351_uart *d = container_of(t, struct mt3351_uart, flush);
	struct uart_port *port = d->port;
	unsigned long flags;
	bool push;

	uart_port_lock_irqsave(port, &flags);
	push = mt3351_take_pending(d) && port->state;
	if (push)
		tty_flip_buffer_push(&port->state->port);
	uart_port_unlock_irqrestore(port, flags);
	return HRTIMER_NORESTART;
}

static int mt3351_handle_irq(struct uart_port *port)
{
	struct uart_8250_port *up = up_to_u8250p(port);
	struct mt3351_uart *d = port->private_data;
	unsigned int iir = serial_port_in(port, UART_IIR);
	unsigned long flags;
	u16 lsr;

	if (!d || !READ_ONCE(d->batch))
		return serial8250_handle_irq(port, iir);
	if (iir & UART_IIR_NO_INT)
		return 0;

	uart_port_lock_irqsave(port, &flags);
	lsr = serial_lsr_in(up);
	if (!(lsr & UART_LSR_DR) && (lsr & UART_LSR_FIFOE))
		serial8250_clear_and_reinit_fifos(up);
	/* throttled: leave the FIFO full so the automatic RTS stops the sender */
	if ((lsr & (UART_LSR_DR | UART_LSR_BI)) &&
	    ((port->read_status_mask & UART_LSR_DR) || (lsr & (UART_LSR_FIFOE | UART_LSR_BRK_ERROR_BITS)))) {
		int max = 256;

		do {
			serial8250_read_char(up, lsr);
			d->pending++;
			if (--max == 0)
				break;
			lsr = serial_in(up, UART_LSR);
		} while (lsr & (UART_LSR_DR | UART_LSR_BI));

		if (d->pending >= MT3351_BATCH || (iir & UART_IIR_RX_TIMEOUT) == UART_IIR_RX_TIMEOUT) {
			if (mt3351_take_pending(d))
				tty_flip_buffer_push(&port->state->port);
		} else if (!hrtimer_is_queued(&d->flush)) {
			hrtimer_start(&d->flush, ns_to_ktime(MT3351_FLUSH_NS), HRTIMER_MODE_REL);
		}
	}
	serial8250_modem_status(up);
	if ((lsr & UART_LSR_THRE) && (up->ier & UART_IER_THRI))
		serial8250_tx_chars(up);
	uart_unlock_and_check_sysrq_irqrestore(port, flags);
	return 1;
}

static void mt3351_set_termios(struct uart_port *port, struct ktermios *termios,
			       const struct ktermios *old)
{
	struct uart_8250_port *up = up_to_u8250p(port);
	struct mt3351_uart *d = port->private_data;
	unsigned int baud, div, cnt, q, r;
	unsigned long flags;

	bool hwfc = termios->c_cflag & CRTSCTS;
	u8 efr;

	/* receive trigger: 8 bytes with automatic flow control, else 1 (see top) */
	up->fcr = UART_FCR_ENABLE_FIFO | (hwfc ? UART_FCR_R_TRIG_10 : UART_FCR_R_TRIG_00);
	serial8250_do_set_termios(port, termios, old);
	baud = uart_get_baud_rate(port, termios, old, 300, 921600);
	/* uart.dll: divisor from the high byte of clk/baud, then the sample count */
	div = (port->uartclk / baud) / 256 + 1;
	q = port->uartclk / (div * baud);
	r = port->uartclk % (div * baud);
	if (r >= div * baud / 2)
		q++;
	cnt = q - 1;
	if (cnt > 255)
		cnt = 255;

	uart_port_lock_irqsave(port, &flags);
	uart_update_timeout(port, termios->c_cflag, baud);
	writeb(3, port->membase + MT3351_UART_HIGHSPEED);
	writeb(cnt, port->membase + MT3351_UART_SAMPLE_COUNT);
	writeb((cnt - 1) >> 1, port->membase + MT3351_UART_SAMPLE_POINT);
	writeb(1, port->membase + MT3351_UART_RATE_FIX);
	serial_port_out(port, UART_LCR, up->lcr | UART_LCR_DLAB);
	serial_dl_write(up, div);
	serial_port_out(port, UART_LCR, 0xbf);
	efr = readb(port->membase + MT3351_UART_EFR);
	if (hwfc)
		efr |= MT3351_EFR_AUTO_RTS | MT3351_EFR_AUTO_CTS;
	else
		efr &= ~(MT3351_EFR_AUTO_RTS | MT3351_EFR_AUTO_CTS);
	writeb(efr, port->membase + MT3351_UART_EFR);
	serial_port_out(port, UART_LCR, up->lcr);
	if (d) {
		if (!hwfc && mt3351_take_pending(d) && port->state)
			tty_flip_buffer_push(&port->state->port);
		WRITE_ONCE(d->batch, hwfc);
	}
	uart_port_unlock_irqrestore(port, flags);
	tty_termios_encode_baud_rate(termios, baud, baud);
}

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
	up.port.set_termios = mt3351_set_termios;
	up.port.handle_irq = mt3351_handle_irq;
	up.port.private_data = data;
	hrtimer_init(&data->flush, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
	data->flush.function = mt3351_flush_timer;

	data->line = serial8250_register_8250_port(&up);
	if (data->line < 0)
		return data->line;
	data->port = &serial8250_get_port(data->line)->port;
	/*
	 * Receive FIFO trigger at 1 byte (the core uses 8): the GPS sends its
	 * measurements in bursts at 921600 baud and with 8 left in the 16-byte
	 * FIFO (87 us) the interrupt came too late: 202 overruns in ~500 bytes.
	 * At 1 byte (162 us of margin): no overrun (21/11/2026). CE uses DMA.
	 */
	serial8250_get_port(data->line)->fcr = UART_FCR_ENABLE_FIFO | UART_FCR_R_TRIG_00;
	platform_set_drvdata(pdev, data);
	return 0;
}

static void mt3351_uart_remove(struct platform_device *pdev)
{
	struct mt3351_uart *data = platform_get_drvdata(pdev);

	WRITE_ONCE(data->batch, false);
	hrtimer_cancel(&data->flush);
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
