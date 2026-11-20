// SPDX-License-Identifier: GPL-2.0-only
/*
 * Speaker playback of the FOSTON FS-707DC (MediaTek MT3351), plain ALSA.
 *
 * From the Windows CE wavedev_drv.dll of the device (docs/audio.md) and
 * tested on the device with devmem: an audio DMA block at 0x8004B000 reads
 * a ring buffer in RAM and feeds the internal DAC; the speaker amplifier is
 * enabled by GPIO 5. The on-chip DSP (speech) is not used or touched.
 *
 * Only these registers are written:
 *   0x8004B000  channel 0 (+0x00 start, +0x04 end incl., +0x10 fmt|fs,
 *               +0x14 gain, +0x18 period), +0x100 run, +0x104, +0x200,
 *               +0x204, +0x208 (CE start values)
 *   0x80061200..0x8006120C, 0x8006110C  analog (read-modify-write, the
 *               PMU shares the page)
 *   0x80001344 / 0x80001324  clock gate clear / set, bits of ids 37 and 43
 *   0x80002000/+0x100/+0x200/+0x300  GPIO 5 (dir, pull-up, pull-down, out)
 *
 * The period interrupt (IRQ 55) is not used yet: a timer reports periods
 * and the position comes from the DMA's read address (+0x08).
 */
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/hrtimer.h>
#include <linux/io.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <sound/control.h>
#include <sound/core.h>
#include <sound/initval.h>
#include <sound/pcm.h>
#include <sound/tlv.h>

/* audio DMA, channel 0 */
#define ADMA_START	0x00
#define ADMA_END	0x04
#define ADMA_POS	0x08
#define ADMA_FMT	0x10
#define  FMT_S16_STEREO	0x40
#define  FMT_S16_MONO	0x60
#define ADMA_GAIN	0x14
#define ADMA_PERIOD	0x18
#define ADMA_RUN	0x100	/* bit0 global enable, bit(4+n) channel n */
#define ADMA_ALT	0x104
#define ADMA_IRQSTA	0x108
#define ADMA_IRQACK	0x10c
#define ADMA_AFE	0x200
#define ADMA_CFG204	0x204
#define ADMA_CFG208	0x208

/* analog, offsets from 0x80061100 */
#define ANA_110C	0x00c
#define ANA_GAIN	0x100	/* [7:0] gain (two equal nibbles), [9:8] mute */
#define ANA_PATH	0x104
#define ANA_DRV		0x108	/* [4:0] driver ramp: 0x1c, 200 us, 0x1f */
#define ANA_20C		0x10c

#define PDN_SET		0x24	/* from 0x80001300: power down */
#define PDN_CLR		0x44	/* power up */
#define PDN_AUDIO	(BIT(19) | BIT(13))	/* ids 43, 37 */

#define GPIO_DIR	0x000
#define GPIO_PU		0x100
#define GPIO_PD		0x200
#define GPIO_OUT	0x300
#define AMP_PIN		BIT(5)

#define BUF_MAX		(64 * 1024)

static int volume = 5;
module_param(volume, int, 0444);
MODULE_PARM_DESC(volume, "initial speaker volume 0..15 (5 sounded right on the device)");

struct mt3351_audio {
	struct device *dev;
	void __iomem *dma, *ana, *pdn, *gpio;
	struct snd_card *card;
	struct snd_pcm_substream *substream;
	struct hrtimer timer;
	ktime_t period;
	bool powered;
	int volume;
};

static void rmw16(void __iomem *r, u16 clear, u16 set)
{
	writew((readw(r) & ~clear) | set, r);
}

static void rmw32(void __iomem *r, u32 clear, u32 set)
{
	writel((readl(r) & ~clear) | set, r);
}

static void mt3351_set_volume(struct mt3351_audio *a)
{
	rmw16(a->ana + ANA_GAIN, 0xff, a->volume | a->volume << 4);
}

/* CE start sequence (wavedev_drv.dll 0x3c0ea8c, 0x3c0db24), minus the DMA run bit */
static void mt3351_power_on(struct mt3351_audio *a)
{
	if (a->powered)
		return;
	writel(PDN_AUDIO, a->pdn + PDN_CLR);
	rmw16(a->ana + ANA_20C, 0, 0x2);
	rmw16(a->ana + ANA_110C, 0x8004, 0x8000);
	rmw32(a->ana + ANA_PATH, 0, 0x1000);
	rmw16(a->ana + ANA_PATH, 0xffff, 0x1000);	/* output path v = 0: speaker */
	rmw16(a->ana + ANA_GAIN, 0x300, 0x300);		/* mute while ramping */
	mt3351_set_volume(a);
	rmw16(a->ana + ANA_DRV, 0x1f, 0x1c);
	usleep_range(200, 300);
	rmw16(a->ana + ANA_DRV, 0x1f, 0x1f);
	rmw16(a->ana + ANA_GAIN, 0x300, 0);		/* unmute */
	writel(0x5b, a->dma + ADMA_CFG208);
	writel(0x11f0, a->dma + ADMA_CFG204);
	writel(1, a->dma + ADMA_AFE);
	writel(0, a->dma + ADMA_ALT);
	rmw32(a->gpio + GPIO_OUT, 0, AMP_PIN);		/* speaker amplifier on */
	a->powered = true;
}

static void mt3351_power_off(struct mt3351_audio *a)
{
	if (!a->powered)
		return;
	rmw32(a->gpio + GPIO_OUT, AMP_PIN, 0);
	rmw16(a->ana + ANA_GAIN, 0x300, 0x300);
	writel(0, a->dma + ADMA_RUN);
	writel(0, a->dma + ADMA_AFE);
	rmw16(a->ana + ANA_DRV, 0x1f, 0xc);
	rmw16(a->ana + ANA_DRV, 0x1f, 0);
	writel(PDN_AUDIO, a->pdn + PDN_SET);
	a->powered = false;
}

static enum hrtimer_restart mt3351_timer(struct hrtimer *t)
{
	struct mt3351_audio *a = container_of(t, struct mt3351_audio, timer);

	writel(readl(a->dma + ADMA_IRQSTA), a->dma + ADMA_IRQACK);
	snd_pcm_period_elapsed(a->substream);
	hrtimer_forward_now(t, a->period);
	return HRTIMER_RESTART;
}

static const struct snd_pcm_hardware mt3351_hw = {
	.info = SNDRV_PCM_INFO_MMAP | SNDRV_PCM_INFO_MMAP_VALID |
		SNDRV_PCM_INFO_INTERLEAVED | SNDRV_PCM_INFO_BLOCK_TRANSFER,
	.formats = SNDRV_PCM_FMTBIT_S16_LE,
	/* 48 kHz left out: its rate code (0) is not confirmed */
	.rates = SNDRV_PCM_RATE_8000 | SNDRV_PCM_RATE_11025 | SNDRV_PCM_RATE_16000 |
		 SNDRV_PCM_RATE_22050 | SNDRV_PCM_RATE_32000 | SNDRV_PCM_RATE_44100 |
		 SNDRV_PCM_RATE_KNOT,
	.rate_min = 8000,
	.rate_max = 44100,
	.channels_min = 1,
	.channels_max = 2,
	.buffer_bytes_max = BUF_MAX,
	.period_bytes_min = 256,
	.period_bytes_max = BUF_MAX / 2,
	.periods_min = 2,
	.periods_max = 64,
};

static const unsigned int mt3351_rates[] = { 8000, 11025, 12000, 16000, 22050, 24000, 32000, 44100 };
static const struct snd_pcm_hw_constraint_list mt3351_rate_list = {
	.count = ARRAY_SIZE(mt3351_rates),
	.list = mt3351_rates,
};

static int mt3351_rate_code(unsigned int rate)
{
	switch (rate) {
	case 44100: return 1;
	case 32000: return 2;
	case 24000: return 3;
	case 22050: return 4;
	case 16000: return 5;
	case 12000: return 6;
	case 11025: return 7;
	case 8000:  return 8;
	}
	return -EINVAL;
}

static int mt3351_open(struct snd_pcm_substream *ss)
{
	struct mt3351_audio *a = snd_pcm_substream_chip(ss);

	a->substream = ss;
	ss->runtime->hw = mt3351_hw;
	return snd_pcm_hw_constraint_list(ss->runtime, 0, SNDRV_PCM_HW_PARAM_RATE,
					  &mt3351_rate_list);
}

static int mt3351_close(struct snd_pcm_substream *ss)
{
	struct mt3351_audio *a = snd_pcm_substream_chip(ss);

	hrtimer_cancel(&a->timer);
	mt3351_power_off(a);
	a->substream = NULL;
	return 0;
}

static int mt3351_prepare(struct snd_pcm_substream *ss)
{
	struct mt3351_audio *a = snd_pcm_substream_chip(ss);
	struct snd_pcm_runtime *rt = ss->runtime;
	int code = mt3351_rate_code(rt->rate);
	u32 bytes = snd_pcm_lib_buffer_bytes(ss);

	if (code < 0)
		return code;
	writel(0, a->dma + ADMA_RUN);
	writel(rt->dma_addr, a->dma + ADMA_START);
	writel(rt->dma_addr + bytes - 1, a->dma + ADMA_END);
	writel((rt->channels == 1 ? FMT_S16_MONO : FMT_S16_STEREO) | code, a->dma + ADMA_FMT);
	rmw16(a->dma + ADMA_GAIN, 0xffff, 0x1000);
	writew(rt->rate / 40, a->dma + ADMA_PERIOD);
	a->period = ns_to_ktime(div_u64((u64)rt->period_size * NSEC_PER_SEC, rt->rate));
	mt3351_power_on(a);
	return 0;
}

static int mt3351_trigger(struct snd_pcm_substream *ss, int cmd)
{
	struct mt3351_audio *a = snd_pcm_substream_chip(ss);

	switch (cmd) {
	case SNDRV_PCM_TRIGGER_START:
	case SNDRV_PCM_TRIGGER_RESUME:
	case SNDRV_PCM_TRIGGER_PAUSE_RELEASE:
		writel(BIT(4) | 1, a->dma + ADMA_RUN);
		hrtimer_start(&a->timer, a->period, HRTIMER_MODE_REL);
		return 0;
	case SNDRV_PCM_TRIGGER_STOP:
	case SNDRV_PCM_TRIGGER_SUSPEND:
	case SNDRV_PCM_TRIGGER_PAUSE_PUSH:
		writel(0, a->dma + ADMA_RUN);
		hrtimer_try_to_cancel(&a->timer);
		return 0;
	}
	return -EINVAL;
}

static snd_pcm_uframes_t mt3351_pointer(struct snd_pcm_substream *ss)
{
	struct mt3351_audio *a = snd_pcm_substream_chip(ss);
	struct snd_pcm_runtime *rt = ss->runtime;
	u32 pos = readl(a->dma + ADMA_POS) - rt->dma_addr;

	if (pos >= snd_pcm_lib_buffer_bytes(ss))
		pos = 0;
	return bytes_to_frames(rt, pos);
}

static const struct snd_pcm_ops mt3351_pcm_ops = {
	.open = mt3351_open,
	.close = mt3351_close,
	.prepare = mt3351_prepare,
	.trigger = mt3351_trigger,
	.pointer = mt3351_pointer,
};

/* 0x80061200[7:0] index 0..15 maps to -22..+23 (CE table, units assumed dB) */
static const DECLARE_TLV_DB_SCALE(mt3351_vol_tlv, -2200, 300, 0);

static int mt3351_vol_info(struct snd_kcontrol *kc, struct snd_ctl_elem_info *ui)
{
	ui->type = SNDRV_CTL_ELEM_TYPE_INTEGER;
	ui->count = 1;
	ui->value.integer.min = 0;
	ui->value.integer.max = 15;
	return 0;
}

static int mt3351_vol_get(struct snd_kcontrol *kc, struct snd_ctl_elem_value *uv)
{
	struct mt3351_audio *a = snd_kcontrol_chip(kc);

	uv->value.integer.value[0] = a->volume;
	return 0;
}

static int mt3351_vol_put(struct snd_kcontrol *kc, struct snd_ctl_elem_value *uv)
{
	struct mt3351_audio *a = snd_kcontrol_chip(kc);
	int v = clamp_t(long, uv->value.integer.value[0], 0, 15);

	if (v == a->volume)
		return 0;
	a->volume = v;
	if (a->powered)
		mt3351_set_volume(a);
	return 1;
}

static const struct snd_kcontrol_new mt3351_vol_ctl = {
	.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
	.name = "Master Playback Volume",
	.access = SNDRV_CTL_ELEM_ACCESS_READWRITE | SNDRV_CTL_ELEM_ACCESS_TLV_READ,
	.info = mt3351_vol_info,
	.get = mt3351_vol_get,
	.put = mt3351_vol_put,
	.tlv.p = mt3351_vol_tlv,
};

static int mt3351_audio_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct mt3351_audio *a;
	struct snd_pcm *pcm;
	struct resource *r;
	int ret;

	a = devm_kzalloc(dev, sizeof(*a), GFP_KERNEL);
	if (!a)
		return -ENOMEM;
	a->dev = dev;
	a->volume = clamp(volume, 0, 15);
	a->dma = devm_platform_ioremap_resource_byname(pdev, "dma");
	if (IS_ERR(a->dma))
		return PTR_ERR(a->dma);
	/* shared pages (PMU, clock gates, GPIO): mapped without claiming them */
	r = platform_get_resource_byname(pdev, IORESOURCE_MEM, "analog");
	a->ana = r ? devm_ioremap(dev, r->start, resource_size(r)) : NULL;
	r = platform_get_resource_byname(pdev, IORESOURCE_MEM, "pdn");
	a->pdn = r ? devm_ioremap(dev, r->start, resource_size(r)) : NULL;
	r = platform_get_resource_byname(pdev, IORESOURCE_MEM, "gpio");
	a->gpio = r ? devm_ioremap(dev, r->start, resource_size(r)) : NULL;
	if (!a->ana || !a->pdn || !a->gpio)
		return -ENOMEM;

	/* the amplifier pin as CE sets it: output, no pulls, off */
	rmw32(a->gpio + GPIO_OUT, AMP_PIN, 0);
	rmw32(a->gpio + GPIO_PU, AMP_PIN, 0);
	rmw32(a->gpio + GPIO_PD, AMP_PIN, 0);
	rmw32(a->gpio + GPIO_DIR, 0, AMP_PIN);

	hrtimer_init(&a->timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
	a->timer.function = mt3351_timer;

	ret = dma_set_mask_and_coherent(dev, DMA_BIT_MASK(32));
	if (ret)
		return ret;
	ret = snd_devm_card_new(dev, SNDRV_DEFAULT_IDX1, "FS707DC", THIS_MODULE, 0, &a->card);
	if (ret)
		return ret;
	strscpy(a->card->driver, "mt3351", sizeof(a->card->driver));
	strscpy(a->card->shortname, "FOSTON FS-707DC", sizeof(a->card->shortname));
	strscpy(a->card->longname, "FOSTON FS-707DC speaker (MT3351 audio DMA)",
		sizeof(a->card->longname));

	ret = snd_pcm_new(a->card, "Speaker", 0, 1, 0, &pcm);
	if (ret)
		return ret;
	pcm->private_data = a;
	strscpy(pcm->name, "Speaker", sizeof(pcm->name));
	snd_pcm_set_ops(pcm, SNDRV_PCM_STREAM_PLAYBACK, &mt3351_pcm_ops);
	snd_pcm_set_managed_buffer_all(pcm, SNDRV_DMA_TYPE_DEV, dev, BUF_MAX, BUF_MAX);

	ret = snd_ctl_add(a->card, snd_ctl_new1(&mt3351_vol_ctl, a));
	if (ret)
		return ret;
	ret = snd_card_register(a->card);
	if (ret)
		return ret;
	platform_set_drvdata(pdev, a);
	dev_info(dev, "speaker ready, volume %d/15\n", a->volume);
	return 0;
}

static void mt3351_audio_remove(struct platform_device *pdev)
{
	struct mt3351_audio *a = platform_get_drvdata(pdev);

	hrtimer_cancel(&a->timer);
	mt3351_power_off(a);
}

static const struct of_device_id mt3351_audio_of_match[] = {
	{ .compatible = "foston,fs707dc-audio" },
	{ }
};
MODULE_DEVICE_TABLE(of, mt3351_audio_of_match);

static struct platform_driver mt3351_audio_driver = {
	.probe = mt3351_audio_probe,
	.remove = mt3351_audio_remove,
	.driver = {
		.name = "mt3351-audio",
		.of_match_table = mt3351_audio_of_match,
	},
};
module_platform_driver(mt3351_audio_driver);

MODULE_DESCRIPTION("FOSTON FS-707DC speaker playback (MT3351 audio DMA)");
MODULE_LICENSE("GPL");
