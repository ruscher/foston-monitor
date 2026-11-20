// SPDX-License-Identifier: GPL-2.0-only
/*
 * Text console drawn straight into the framebuffer that the Windows CE boot
 * of the FOSTON FS-707DC leaves running (BigFoston OS bring-up).
 *
 * There is no known serial port on this board, so this is how kernel
 * messages become visible. It is registered from the interrupt controller
 * initialisation (before the timer, the usual point of a silent hang) and
 * replays the log from the very first line (CON_PRINTBUFFER).
 *
 * The framebuffer is described by the "simple-framebuffer" node of the
 * device tree (r5g6b5); that memory is not part of the kernel's RAM.
 *
 * With "splash" on the command line the BigFoston opening screen is drawn
 * instead (mt3351-splash.h, made by tools/bigfoston/make_splash.py) and the
 * boot scripts move its progress bar through the "progress" parameter. The
 * log is still kept: a panic, or the graphical shell giving the screen back
 * (paused=0), replaces the splash with the text console.
 */
#include <linux/console.h>
#include <linux/font.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/string.h>
#include <linux/crc32.h>
#include <linux/mm.h>
#include <linux/moduleparam.h>
#include <linux/notifier.h>
#include <linux/panic_notifier.h>
#include "mt3351-splash.h"

#define COLS_MAX	128
#define ROWS_MAX	64
#define FG		0xdefb		/* light grey */
#define BG		0x1904		/* graphite */
#define ACCENT		0x343c		/* BigLinux blue */

static void initrd_check(const char *when);

static u16 __iomem *fb;
static unsigned int fb_w, fb_h, fb_stride;	/* stride in pixels */
static unsigned int cols, rows, cx, cy;
static const struct font_desc *font;
static char text[ROWS_MAX][COLS_MAX];

/*
 * The same memory is /dev/fb0 (simplefb). A graphical program writes 1 to
 * /sys/module/mt3351_bootcon/parameters/paused so kernel messages stop
 * drawing over it; the text is still kept and comes back on 0.
 */
static bool paused;
static bool splash, splash_on;
static int progress;
static void redraw(void);
static void show_text(void);

static int __init splash_param(char *arg)
{
	splash = true;
	return 0;
}
early_param("splash", splash_param);

static int paused_set(const char *val, const struct kernel_param *kp)
{
	int ret = param_set_bool(val, kp);

	if (!ret && !paused && fb) {
		console_lock();
		show_text();		/* the shell gave the screen back: show the log */
		console_unlock();
	}
	return ret;
}

static void draw_progress(void)
{
	unsigned int x, y, fill = SPLASH_BAR_W * progress / 100;

	if (!fb || !splash_on || paused)
		return;
	/* degradê do logotipo na parte cheia; as pontas arredondadas mantêm o fundo */
	for (y = 0; y < SPLASH_BAR_H; y++)
		for (x = splash_round[y]; x < SPLASH_BAR_W - splash_round[y]; x++)
			writew_relaxed(x < fill ? splash_bar[x] : SPLASH_TRACK,
				       fb + (SPLASH_BAR_Y + y) * fb_stride + SPLASH_BAR_X + x);
}

static int progress_set(const char *val, const struct kernel_param *kp)
{
	int ret = param_set_int(val, kp);

	progress = clamp(progress, 0, 100);
	if (!ret)
		draw_progress();
	return ret;
}

static const struct kernel_param_ops progress_ops = {
	.set = progress_set,
	.get = param_get_int,
};
module_param_cb(progress, &progress_ops, &progress, 0644);
MODULE_PARM_DESC(progress, "boot progress bar of the splash, 0..100");

static const struct kernel_param_ops paused_ops = {
	.set = paused_set,
	.get = param_get_bool,
};
module_param_cb(paused, &paused_ops, &paused, 0644);
MODULE_PARM_DESC(paused, "stop drawing kernel messages (the screen belongs to /dev/fb0)");

static void draw_char(unsigned int col, unsigned int row, unsigned char c)
{
	const u8 *glyph = (const u8 *)font->data + c * font->height;
	u16 __iomem *p = fb + (row * font->height + 16) * fb_stride + col * font->width;
	unsigned int y, x;

	for (y = 0; y < font->height; y++, p += fb_stride)
		for (x = 0; x < 8; x++)
			writew_relaxed((glyph[y] & (0x80 >> x)) ? FG : BG, p + x);
}

static void redraw(void)
{
	unsigned int r, c;

	if (paused || splash_on)
		return;

	for (r = 0; r < rows; r++)
		for (c = 0; c < cols; c++)
			draw_char(c, r, text[r][c]);
}

static void newline(void)
{
	cx = 0;
	if (++cy < rows)
		return;
	memmove(text[0], text[1], sizeof(text[0]) * (rows - 1));
	memset(text[rows - 1], ' ', sizeof(text[0]));
	cy = rows - 1;
	redraw();
}

static void bootcon_write(struct console *con, const char *s, unsigned int n)
{
	while (n--) {
		unsigned char c = *s++;

		if (c == '\n') {
			newline();
			continue;
		}
		if (c == '\r') {
			cx = 0;
			continue;
		}
		if (c < 32 || c > 126)
			c = '?';
		if (cx >= cols)
			newline();
		text[cy][cx] = c;
		if (!paused && !splash_on)
			draw_char(cx, cy, c);
		cx++;
	}
}

static struct console bootcon = {
	.name	= "bfcon",
	.write	= bootcon_write,
	.flags	= CON_PRINTBUFFER,
	.index	= -1,
};

/* blue band on top, graphite background, then the text */
static void clear_screen(void)
{
	unsigned int x, y;

	for (y = 0; y < fb_h; y++)
		for (x = 0; x < fb_w; x++)
			writew_relaxed(y < 6 ? ACCENT : BG, fb + y * fb_stride + x);
}

static void show_text(void)
{
	if (splash_on) {
		splash_on = false;
		clear_screen();
	}
	redraw();
}

static int panic_show_log(struct notifier_block *nb, unsigned long ev, void *p)
{
	paused = false;
	show_text();
	return NOTIFY_DONE;
}

static struct notifier_block panic_nb = {
	.notifier_call = panic_show_log,
	.priority = INT_MAX,
};

/* canal 8 bits -> n bits com pontilhado ordenado (limiar 0..15 de 16) */
static inline unsigned int __init dither(unsigned int c88, unsigned int bits, unsigned int thr)
{
	unsigned int max = (1u << bits) - 1;
	unsigned int v = (c88 * max / 255 + thr * 16) >> 8;

	return min(v, max);
}

/* Fundo: degradê radial do ícone (t = d² * K >> 24), em RGB565 com Bayer 4x4 */
static void __init draw_splash_bg(void)
{
	static const u8 bayer[4][4] __initconst = {
		{ 0, 8, 2, 10 }, { 12, 4, 14, 6 }, { 3, 11, 1, 9 }, { 15, 7, 13, 5 } };
	unsigned int x, y;

	for (y = 0; y < SPLASH_H; y++) {
		int dy = (int)y - SPLASH_CY;

		for (x = 0; x < SPLASH_W; x++) {
			int dx = (int)x - SPLASH_CX;
			u32 t = min_t(u32, ((u32)(dx * dx + dy * dy) * SPLASH_K) >> 24, 256);
			unsigned int thr = bayer[y & 3][x & 3];
			unsigned int r = SPLASH_CR * 256 + (SPLASH_ER - SPLASH_CR) * (int)t;
			unsigned int g = SPLASH_CG * 256 + (SPLASH_EG - SPLASH_CG) * (int)t;
			unsigned int b = SPLASH_CB * 256 + (SPLASH_EB - SPLASH_CB) * (int)t;

			writew_relaxed(dither(r, 5, thr) << 11 | dither(g, 6, thr) << 5 | dither(b, 5, thr),
				       fb + y * fb_stride + x);
		}
	}
}

/* Logotipo e legenda por cima: pula / repete / literais (ver mt3351-splash.h) */
static void __init draw_splash(void)
{
	unsigned int i = 0, n = 0, total = SPLASH_W * SPLASH_H;

	draw_splash_bg();
	while (i < ARRAY_SIZE(splash_rle) && n < total) {
		unsigned int w = splash_rle[i++], k, cnt = w & 0x3fff;

		if (!(w & 0xc000)) {
			n += cnt;
		} else if (w & 0x8000) {
			for (k = 0; k < (w & 0x7fff) && i < ARRAY_SIZE(splash_rle) && n < total; k++, n++)
				writew_relaxed(splash_rle[i++], fb + (n / SPLASH_W) * fb_stride + n % SPLASH_W);
		} else if (i < ARRAY_SIZE(splash_rle)) {
			u16 c = splash_rle[i++];

			for (k = 0; k < cnt && n < total; k++, n++)
				writew_relaxed(c, fb + (n / SPLASH_W) * fb_stride + n % SPLASH_W);
		}
	}
}

/* called by the CIRQ driver (irq-mt3351-cirq.c), which has a weak default */
void __init mt3351_earlyfb_register(void);

void __init mt3351_earlyfb_register(void)
{
	struct device_node *np;
	struct resource res;
	const char *format;

	np = of_find_compatible_node(NULL, NULL, "simple-framebuffer");
	if (!np)
		return;
	if (of_address_to_resource(np, 0, &res) ||
	    of_property_read_u32(np, "width", &fb_w) ||
	    of_property_read_u32(np, "height", &fb_h) ||
	    of_property_read_u32(np, "stride", &fb_stride) ||
	    of_property_read_string(np, "format", &format) || strcmp(format, "r5g6b5")) {
		of_node_put(np);
		return;
	}
	of_node_put(np);
	font = find_font("VGA8x8");	/* 58 rows: a whole panic fits in one photo */
	if (!font)
		return;
	fb = ioremap_wc(res.start, resource_size(&res));
	if (!fb)
		return;
	fb_stride /= 2;
	cols = min_t(unsigned int, fb_w / font->width, COLS_MAX);
	rows = min_t(unsigned int, (fb_h - 16) / font->height, ROWS_MAX);
	memset(text, ' ', sizeof(text));
	if (splash && fb_w == SPLASH_W && fb_h == SPLASH_H) {
		splash_on = true;
		draw_splash();
		progress = 5;
		draw_progress();
	}
	else
		clear_screen();		/* a thin BigLinux-blue band on top, then the console area */
	atomic_notifier_chain_register(&panic_notifier_list, &panic_nb);
	register_console(&bootcon);
	initrd_check("inicio");
}

/*
 * Bring-up integrity check of the initrd the BigFoston loader placed in RAM:
 * CRC32 (zlib) of /chosen linux,initrd-start..end against the value
 * tools/bigfoston/mkboot.py stored in /chosen bigfoston,initrd-crc32.
 * Run at interrupt-controller setup and again just before the initramfs is
 * unpacked, to tell "corrupted by the hand-over" from "corrupted during boot".
 */
static void initrd_check(const char *when)
{
	struct device_node *chosen = of_find_node_by_path("/chosen");
	u32 start, end, want = 0;
	u32 crc;

	if (!chosen)
		return;
	if (of_property_read_u32(chosen, "linux,initrd-start", &start) ||
	    of_property_read_u32(chosen, "linux,initrd-end", &end) || end <= start) {
		of_node_put(chosen);
		return;
	}
	of_property_read_u32(chosen, "bigfoston,initrd-crc32", &want);
	of_node_put(chosen);
	crc = crc32_le(~0u, phys_to_virt(start), end - start) ^ ~0u;
	pr_info("bfcheck %s: initrd 0x%08x+%u crc32 %08x expected %08x -> %s\n", when, start, end - start, crc,
		want, crc == want ? "OK" : "CORROMPIDO");
}

static int __init initrd_check_late(void)
{
	initrd_check("antes-do-unpack");
	return 0;
}
fs_initcall_sync(initrd_check_late);
