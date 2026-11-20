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
static void redraw(void);

static int paused_set(const char *val, const struct kernel_param *kp)
{
	int ret = param_set_bool(val, kp);

	if (!ret && !paused && fb) {
		console_lock();
		redraw();
		console_unlock();
	}
	return ret;
}

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

	if (paused)
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
		if (!paused)
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

void __init mt3351_earlyfb_register(void)
{
	struct device_node *np;
	struct resource res;
	const char *format;
	unsigned int x, y;

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
	/* a thin BigLinux-blue band on top, then the console area */
	for (y = 0; y < fb_h; y++)
		for (x = 0; x < fb_w; x++)
			writew_relaxed(y < 6 ? ACCENT : BG, fb + y * fb_stride + x);
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
