// SPDX-License-Identifier: GPL-2.0-only
/*
 * MediaTek MT3351 MSDC SD host controller (FOSTON FS-707DC), polled PIO.
 *
 * Register map and command encoding from the Windows CE sdhc_drv.dll of the
 * device (docs/sd-controller.md); same generation as the MT6516 MSDC, not
 * the one of mtk-sd.c. Windows CE leaves the slot powered (VSDIO LDO, the
 * MSDC1 clock gate and the card power GPIO), so this driver only uses the
 * controller's own registers: it never touches PMU, clock gates or GPIO.
 *
 * Like the CE driver, command and data completion are polled; card detect
 * is polled too (the card-detect interrupt acknowledge is not known).
 * There is no block-count register: multi-block transfers end with CMD12.
 *
 * Writes are refused unless the "readonly" parameter is cleared: the card
 * also holds the Windows CE side of BigFoston and may be worn out.
 */
#include <linux/bitfield.h>
#include <linux/delay.h>
#include <linux/highmem.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/mmc/host.h>
#include <linux/mmc/mmc.h>
#include <linux/mmc/sd.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/scatterlist.h>
#include <linux/workqueue.h>

#define MSDC_CFG		0x00
#define  CFG_SCLKF		GENMASK(15, 8)
#define  CFG_DMA		BIT(17)
#define  CFG_FIFOTHD		GENMASK(31, 28)	/* 1 for PIO (CE) */
#define MSDC_STA		0x04
#define  STA_FIFO		BIT(2)		/* FIFO has data / room */
#define  STA_FIFOCLR		BIT(14)
#define MSDC_DAT		0x0c
#define MSDC_PS			0x10
#define  PS_CDN			BIT(3)		/* 0 = card present */
#define MSDC_IOCON		0x14
#define SDC_CFG			0x20
#define  SDC_BLKLEN		GENMASK(11, 0)
#define  SDC_SIEN		BIT(16)
#define  SDC_4BIT		BIT(17)
#define SDC_CMD			0x24
#define  CMD_RSP_SHIFT		7
#define  CMD_DTYPE_SINGLE	(1 << 11)
#define  CMD_DTYPE_MULTI	(2 << 11)
#define  CMD_WRITE		BIT(13)
#define  CMD_STOP		BIT(14)
#define SDC_ARG			0x28
#define SDC_STA			0x2c
#define  SDC_BUSY		BIT(0)
#define  SDC_CMDBUSY		BIT(1)
#define  SDC_CARDBUSY		BIT(4)
#define SDC_RESP(n)		(0x30 + 4 * (n))
#define SDC_CMDSTA		0x40
#define SDC_DATSTA		0x44
#define  ST_OK			BIT(0)
#define  ST_TIMEOUT		BIT(1)
#define  ST_CRC			BIT(2)

/* response types of the command word, bits [9:7] */
enum { RSP_NONE, RSP_R1, RSP_R2, RSP_R3, RSP_R6 = 6, RSP_R1B = 7 };

#define MSDC_SRC_HZ		48000000	/* UPLL 48 MHz, as left by CE */

#define PIO_TIMEOUT_MS	2000

/* longest wait for FIFO room/data within one segment (read only, diagnostics) */
static unsigned int max_stall_ms;
module_param(max_stall_ms, uint, 0444);
MODULE_PARM_DESC(max_stall_ms, "longest PIO stall seen, in ms");

static bool readonly = true;
/* writable at run time; it takes effect when the card is (re)initialised, so
 * /etc/init.d/S30sdcard unbinds and binds the host after clearing it */
module_param(readonly, bool, 0644);
MODULE_PARM_DESC(readonly, "refuse writes to the card (default: yes)");

static unsigned int max_hz = 24000000;
module_param(max_hz, uint, 0444);
MODULE_PARM_DESC(max_hz, "highest SD clock (Windows CE uses 24 MHz)");

struct msdc {
	void __iomem *base;
	struct mmc_host *mmc;
	struct mmc_request *mrq;
	struct work_struct work;
	unsigned int clock;
};

static void msdc_rmw(struct msdc *h, u32 reg, u32 clear, u32 set)
{
	writel((readl(h->base + reg) & ~clear) | set, h->base + reg);
}

static bool msdc_card_present(struct msdc *h)
{
	return !(readl(h->base + MSDC_PS) & PS_CDN);
}

static int msdc_wait_clear(struct msdc *h, u32 reg, u32 mask, unsigned int ms)
{
	u32 v;

	return readl_poll_timeout(h->base + reg, v, !(v & mask), 5, ms * 1000);
}

static u32 msdc_cmd_word(struct mmc_command *cmd)
{
	struct mmc_data *data = cmd->data;
	u32 rsp, w = cmd->opcode & 0x3f;

	switch (mmc_resp_type(cmd)) {
	case MMC_RSP_NONE:
		rsp = RSP_NONE;
		break;
	case MMC_RSP_R2:
		rsp = RSP_R2;
		break;
	case MMC_RSP_R3:
		rsp = RSP_R3;
		break;
	case MMC_RSP_R1B:
		rsp = RSP_R1B;
		break;
	default:			/* R1, R5, R6, R7 */
		rsp = cmd->opcode == SD_SEND_RELATIVE_ADDR ? RSP_R6 : RSP_R1;
		break;
	}
	w |= rsp << CMD_RSP_SHIFT;
	if (cmd->opcode == MMC_STOP_TRANSMISSION)
		w |= CMD_STOP;
	if (data) {
		w |= data->blocks > 1 ? CMD_DTYPE_MULTI : CMD_DTYPE_SINGLE;
		if (data->flags & MMC_DATA_WRITE)
			w |= CMD_WRITE;
	}
	return w;
}

static int msdc_send_cmd(struct msdc *h, struct mmc_command *cmd)
{
	/*
	 * CMD12 and CMD13 are answered while the card is still programming
	 * (DAT0 low): only the command line must be free. Every other command
	 * waits for the controller, up to 1 s: after long writes SanDisk cards
	 * were seen busy for more than the 500 ms used before, the write
	 * failed and the core then declared the card removed.
	 */
	bool cmd_only = cmd->opcode == MMC_STOP_TRANSMISSION || cmd->opcode == MMC_SEND_STATUS;
	u32 busy = cmd_only ? SDC_CMDBUSY : SDC_BUSY;
	u32 st;
	int ret;

	if (msdc_wait_clear(h, SDC_STA, busy, cmd_only ? 500 : 1000))
		return -EBUSY;
	readl(h->base + SDC_CMDSTA);		/* read-to-clear a stale status */
	writel(cmd->arg, h->base + SDC_ARG);
	writel(msdc_cmd_word(cmd), h->base + SDC_CMD);

	ret = readl_poll_timeout(h->base + SDC_CMDSTA, st, st & 0xffff, 5, 100 * 1000);
	if (ret)
		return -ETIMEDOUT;
	if (st & ST_TIMEOUT)
		return -ETIMEDOUT;
	if ((st & ST_CRC) && (cmd->flags & MMC_RSP_CRC))
		return -EILSEQ;

	if (cmd->flags & MMC_RSP_136) {
		/* RESP3 holds the most significant word (CE: RESP0..3 little-endian) */
		cmd->resp[0] = readl(h->base + SDC_RESP(3));
		cmd->resp[1] = readl(h->base + SDC_RESP(2));
		cmd->resp[2] = readl(h->base + SDC_RESP(1));
		cmd->resp[3] = readl(h->base + SDC_RESP(0));
	} else if (cmd->flags & MMC_RSP_PRESENT) {
		cmd->resp[0] = readl(h->base + SDC_RESP(0));
	}
	if ((cmd->flags & MMC_RSP_BUSY) &&
	    msdc_wait_clear(h, SDC_STA, SDC_CARDBUSY, cmd->busy_timeout ?: 3000))
		return -ETIMEDOUT;
	return 0;
}

/* PIO, as the CE PollingReceive/PollingTransmit: move words while the FIFO allows */
static int msdc_pio(struct msdc *h, struct mmc_data *data)
{
	bool write = data->flags & MMC_DATA_WRITE;
	struct sg_mapping_iter miter;
	unsigned long timeout, t0;
	unsigned int stall, moved;
	int ret = 0;
	u32 st;

	/*
	 * Not SG_MITER_ATOMIC: this runs in a workqueue and must be able to give
	 * the (single) CPU away while the card is busy, or the whole system
	 * froze for as long as the card stalled.
	 */
	sg_miter_start(&miter, data->sg, data->sg_len,
		       write ? SG_MITER_FROM_SG : SG_MITER_TO_SG);
	while (sg_miter_next(&miter)) {
		u32 *p = miter.addr;
		size_t words = miter.length / 4;

		/*
		 * A card may stop taking data between blocks while it programs
		 * its flash: 500 ms was exceeded in a sustained write test (CMD25
		 * data timeouts, files corrupted). Allow 2 s and record the
		 * longest stall seen (max_stall_ms).
		 */
		timeout = jiffies + msecs_to_jiffies(PIO_TIMEOUT_MS);
		t0 = jiffies;
		while (words) {
			st = readl(h->base + SDC_DATSTA);
			if (st & ST_TIMEOUT) {
				ret = -ETIMEDOUT;
				goto out;
			}
			if (st & ST_CRC) {
				ret = -EILSEQ;
				goto out;
			}
			moved = 0;
			while (words && (readl(h->base + MSDC_STA) & STA_FIFO)) {
				if (write)
					writel(*p++, h->base + MSDC_DAT);
				else
					*p++ = readl(h->base + MSDC_DAT);
				words--;
				moved++;
			}
			if (words && time_after(jiffies, timeout)) {
				ret = -ETIMEDOUT;
				goto out;
			}
			if (!moved)
				cond_resched();		/* card busy: let the UI run */
		}
		stall = jiffies_to_msecs(jiffies - t0);
		if (stall > max_stall_ms)
			max_stall_ms = stall;
		data->bytes_xfered += miter.length;
	}
	/* the last block: wait for its data status (CE SD_WaitDATRdyOrTo, 250 ms) */
	if (readl_poll_timeout(h->base + SDC_DATSTA, st, st & 0xffff, 10, 250 * 1000) == 0 &&
	    (st & (ST_TIMEOUT | ST_CRC)))
		ret = st & ST_CRC ? -EILSEQ : -ETIMEDOUT;
out:
	sg_miter_stop(&miter);
	/*
	 * On any error report nothing transferred: the FIFO may have taken words
	 * the card never programmed, and a partial count made the core rewrite
	 * only the "missing" part (files were corrupted). Rewriting the same
	 * blocks again is harmless.
	 */
	if (ret)
		data->bytes_xfered = 0;
	return ret;
}

static void msdc_work(struct work_struct *work)
{
	struct msdc *h = container_of(work, struct msdc, work);
	struct mmc_request *mrq = h->mrq;
	struct mmc_data *data = mrq->data;

	if (!msdc_card_present(h)) {
		mrq->cmd->error = -ENOMEDIUM;
		goto done;
	}
	if (data) {
		if ((data->flags & MMC_DATA_WRITE) && readonly) {
			mrq->cmd->error = -EROFS;
			goto done;
		}
		msdc_rmw(h, MSDC_STA, 0, STA_FIFOCLR);
		msdc_rmw(h, MSDC_CFG, CFG_FIFOTHD | CFG_DMA, FIELD_PREP(CFG_FIFOTHD, 1));
		msdc_rmw(h, SDC_CFG, SDC_BLKLEN, FIELD_PREP(SDC_BLKLEN, data->blksz));
	}
	mrq->cmd->error = msdc_send_cmd(h, mrq->cmd);
	if (mrq->cmd->error)
		goto done;
	if (data) {
		data->error = msdc_pio(h, data);
		/* no block counter: a multi-block transfer (or a failed one) ends with CMD12 */
		if (mrq->stop && (data->blocks > 1 || data->error))
			mrq->stop->error = msdc_send_cmd(h, mrq->stop);
	}
done:
	/* every failure in the log, with the state of the controller and of the slot */
	if (mrq->cmd->error || (data && data->error) || (mrq->stop && mrq->stop->error))
		dev_warn_ratelimited(mmc_dev(h->mmc),
				     "CMD%u arg %#x: cmd %d data %d stop %d (SDC_STA %#x CMDSTA %#x DATSTA %#x PS %#x)\n",
				     mrq->cmd->opcode, mrq->cmd->arg, mrq->cmd->error,
				     data ? data->error : 0, mrq->stop ? mrq->stop->error : 0,
				     readl(h->base + SDC_STA), readl(h->base + SDC_CMDSTA),
				     readl(h->base + SDC_DATSTA), readl(h->base + MSDC_PS));
	h->mrq = NULL;
	mmc_request_done(h->mmc, mrq);
}

static void msdc_request(struct mmc_host *mmc, struct mmc_request *mrq)
{
	struct msdc *h = mmc_priv(mmc);

	WARN_ON(h->mrq);
	if (mrq->sbc) {			/* no CMD23: there is no block-count register */
		mrq->sbc->error = -EINVAL;
		mmc_request_done(mmc, mrq);
		return;
	}
	h->mrq = mrq;
	queue_work(system_unbound_wq, &h->work);
}

static void msdc_set_clock(struct msdc *h, unsigned int hz)
{
	unsigned int div, sclkf;

	if (!hz || hz == h->clock)
		return;
	div = DIV_ROUND_UP(MSDC_SRC_HZ, hz);
	sclkf = div <= 2 ? 0 : min(DIV_ROUND_UP(div, 4), 255U);
	/* CE SetClockRateBase: stop the serial interface, set SCLKF, restart */
	msdc_rmw(h, SDC_CFG, SDC_SIEN, 0);
	msdc_rmw(h, MSDC_CFG, CFG_SCLKF, FIELD_PREP(CFG_SCLKF, sclkf));
	msdc_rmw(h, SDC_CFG, 0, SDC_SIEN);
	h->clock = hz;
	h->mmc->actual_clock = MSDC_SRC_HZ / (sclkf ? 4 * sclkf : 2);
	dev_dbg(mmc_dev(h->mmc), "clock %u Hz -> SCLKF %u (%u Hz)\n",
		hz, sclkf, h->mmc->actual_clock);
}

static void msdc_set_ios(struct mmc_host *mmc, struct mmc_ios *ios)
{
	struct msdc *h = mmc_priv(mmc);

	/* power stays as Windows CE left it (VSDIO, clock gate, GPIO35) */
	msdc_set_clock(h, ios->clock);
	msdc_rmw(h, SDC_CFG, SDC_4BIT, ios->bus_width == MMC_BUS_WIDTH_4 ? SDC_4BIT : 0);
}

static int msdc_get_cd(struct mmc_host *mmc)
{
	return msdc_card_present(mmc_priv(mmc));
}

static int msdc_get_ro(struct mmc_host *mmc)
{
	return readonly;		/* the slot has no write-protect switch (CE: always 0) */
}

static const struct mmc_host_ops msdc_ops = {
	.request = msdc_request,
	.set_ios = msdc_set_ios,
	.get_cd = msdc_get_cd,
	.get_ro = msdc_get_ro,
};

static int msdc_probe(struct platform_device *pdev)
{
	struct mmc_host *mmc;
	struct msdc *h;
	int ret;

	mmc = devm_mmc_alloc_host(&pdev->dev, sizeof(*h));
	if (!mmc)
		return -ENOMEM;
	h = mmc_priv(mmc);
	h->mmc = mmc;
	h->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(h->base))
		return PTR_ERR(h->base);
	INIT_WORK(&h->work, msdc_work);

	ret = mmc_of_parse(mmc);
	if (ret)
		return ret;
	mmc->ops = &msdc_ops;
	mmc->f_min = MSDC_SRC_HZ / (4 * 255);
	mmc->f_max = min(max_hz, MSDC_SRC_HZ / 2);
	mmc->ocr_avail = MMC_VDD_32_33 | MMC_VDD_33_34;
	mmc->caps |= MMC_CAP_4_BIT_DATA | MMC_CAP_NEEDS_POLL;
	mmc->caps2 |= MMC_CAP2_NO_SDIO | MMC_CAP2_NO_MMC;
	mmc->max_blk_size = 2048;
	mmc->max_blk_count = 256;
	mmc->max_req_size = 256 * 512;
	mmc->max_segs = 64;
	mmc->max_seg_size = mmc->max_req_size;

	dev_info(&pdev->dev, "CFG %08x PS %08x SDC_CFG %08x: card %s, %s\n",
		 readl(h->base + MSDC_CFG), readl(h->base + MSDC_PS),
		 readl(h->base + SDC_CFG), msdc_card_present(h) ? "present" : "absent",
		 readonly ? "read-only" : "read-write");
	platform_set_drvdata(pdev, h);
	return mmc_add_host(mmc);
}

static void msdc_remove(struct platform_device *pdev)
{
	struct msdc *h = platform_get_drvdata(pdev);

	mmc_remove_host(h->mmc);
	cancel_work_sync(&h->work);
}

static const struct of_device_id msdc_of_match[] = {
	{ .compatible = "mediatek,mt3351-msdc" },
	{ }
};
MODULE_DEVICE_TABLE(of, msdc_of_match);

static struct platform_driver msdc_driver = {
	.probe = msdc_probe,
	.remove = msdc_remove,
	.driver = {
		.name = "mt3351-msdc",
		.of_match_table = msdc_of_match,
		.probe_type = PROBE_PREFER_ASYNCHRONOUS,
	},
};
module_platform_driver(msdc_driver);

MODULE_DESCRIPTION("MediaTek MT3351 MSDC SD host (polled PIO)");
MODULE_LICENSE("GPL");
