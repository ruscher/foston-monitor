/* gui.c — frontend "bigfoston" do NetSurf: o motor do app Internet (BigFoston Web).
 *
 * Processo sem interface própria. O shell do BigFoston OS (LVGL) desenha a barra,
 * o teclado e os menus; este processo só renderiza a página e responde a comandos:
 *
 *   tela    a página é desenhada numa superfície libnsfb "ram" RGB565 do tamanho
 *           da área de conteúdo e, depois de cada redesenho completo (sem passos
 *           intermediários visíveis), as linhas alteradas são copiadas para a
 *           memória compartilhada (--fd N ou --shm ARQ), que o shell mostra sem cópia.
 *   entrada comandos em linhas UTF-8 no stdin (ver protocol.md).
 *   saída   eventos em linhas no stdout: "R x0 y0 x1 y1" (região pronta), URL,
 *           TITLE, LOAD, NAV, EXTENT, SCROLL, CARET, DL, WARN, FEAT...
 *
 * Um travamento aqui não derruba o shell; fechar o app devolve toda a RAM.
 * Reaproveita do frontend framebuffer (no tarball conferido do NetSurf 3.11) os
 * plotters, bitmaps, fontes FreeType, fetch e o agendador; sem o toolkit fbtk.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include <libnsfb.h>
#include <libnsfb_plot.h>

#include "utils/utils.h"
#include "utils/nsoption.h"
#include "utils/filepath.h"
#include "utils/log.h"
#include "utils/messages.h"
#include "utils/nsurl.h"
#include "netsurf/browser_window.h"
#include "netsurf/keypress.h"
#include "netsurf/mouse.h"
#include "netsurf/misc.h"
#include "netsurf/netsurf.h"
#include "netsurf/plotters.h"
#include "netsurf/window.h"
#include "netsurf/content.h"
#include "netsurf/cookie_db.h"
#include "netsurf/url_db.h"
#include "content/fetch.h"
#include "content/hlcache.h"
#include "desktop/browser_history.h"

#include "framebuffer/fetch.h"
#include "framebuffer/framebuffer.h"
#include "framebuffer/schedule.h"
#include "framebuffer/findfile.h"
#include "framebuffer/font.h"
#include "framebuffer/bitmap.h"

#include "bigfoston/download.h"
#include "bigfoston/out.h"

#define CMD_MAX 8192

/* ------------------------------------------------------------- estado */

struct gui_window {
	struct browser_window *bw;
	int scrollx, scrolly;          /* posição da janela na página (px, já em escala) */
	int panx, pany;                /* rolagem pedida ainda não aplicada */
	struct rect dirty;             /* área a redesenhar (coordenadas da janela) */
	bool redraw, pan;
	bool caret;                    /* campo de texto com cursor */
	int caret_x, caret_y, caret_h; /* na página */
	struct gui_window *next, *prev;
};

static struct gui_window *windows;     /* a primeira é a ativa (a que aparece) */
static nsfb_t *nsfb;
static uint16_t *shm;                  /* RGB565, VW x VH, compartilhada com o shell */
static int vw = 800, vh = 360;
static bool done;
static const char *start_url;
static char data_dir[PATH_MAX] = ".";

static char opt_paths[2][PATH_MAX + 16];  /* Cookies, URLs (histórico do núcleo) */

static struct gui_window *active(void) { return windows; }

static int count_windows(void)
{
	int n = 0;
	for (struct gui_window *w = windows; w; w = w->next)
		n++;
	return n;
}

/* ------------------------------------------------------------- saída */

void out(const char *fmt, ...)
{
	va_list ap;
	char line[CMD_MAX];
	va_start(ap, fmt);
	int n = vsnprintf(line, sizeof line - 1, fmt, ap);
	va_end(ap);
	if (n < 0)
		return;
	if (n > (int)sizeof line - 2)
		n = sizeof line - 2;
	for (int i = 0; i < n; i++)          /* uma linha por evento: nada de \n no meio */
		if (line[i] == '\n' || line[i] == '\r')
			line[i] = ' ';
	line[n++] = '\n';
	for (int off = 0; off < n; ) {
		ssize_t w = write(1, line + off, n - off);
		if (w < 0 && errno == EINTR)
			continue;
		if (w <= 0) {                     /* o shell fechou: encerra */
			done = true;
			return;
		}
		off += w;
	}
}

/* ------------------------------------------------------------- redesenho */

static void dirty_reset(struct gui_window *g)
{
	g->dirty.x0 = g->dirty.y0 = INT_MAX;
	g->dirty.x1 = g->dirty.y1 = INT_MIN;
	g->redraw = false;
}

static void queue_redraw(struct gui_window *g, int x0, int y0, int x1, int y1)
{
	if (x0 < 0) x0 = 0;
	if (y0 < 0) y0 = 0;
	if (x1 > vw) x1 = vw;
	if (y1 > vh) y1 = vh;
	if (x1 <= x0 || y1 <= y0)
		return;
	g->dirty.x0 = min(g->dirty.x0, x0);
	g->dirty.y0 = min(g->dirty.y0, y0);
	g->dirty.x1 = max(g->dirty.x1, x1);
	g->dirty.y1 = max(g->dirty.y1, y1);
	g->redraw = true;
}

static void extent(struct gui_window *g, int *w, int *h)
{
	if (browser_window_get_extents(g->bw, true, w, h) != NSERROR_OK) {
		*w = vw;
		*h = vh;
	}
}

static void clamp_scroll(struct gui_window *g, int *sx, int *sy)
{
	int w, h;
	extent(g, &w, &h);
	if (*sx > w - vw) *sx = w - vw;
	if (*sy > h - vh) *sy = h - vh;
	if (*sx < 0) *sx = 0;
	if (*sy < 0) *sy = 0;
}

static void report_scroll(struct gui_window *g)
{
	int w, h;
	extent(g, &w, &h);
	out("SCROLL %d %d %d %d", g->scrollx, g->scrolly, w, h);
}

/* rolagem: move o que continua visível (nsfb_plot_copy) e devolve em *exp a
 * faixa nova a renderizar; false se nada mudou */
static bool apply_pan(struct gui_window *g, struct rect *exp)
{
	int nx = g->scrollx + g->panx, ny = g->scrolly + g->pany;
	clamp_scroll(g, &nx, &ny);
	int dx = nx - g->scrollx, dy = ny - g->scrolly;
	g->panx = g->pany = 0;
	g->pan = false;
	if (!dx && !dy)
		return false;
	g->scrollx = nx;
	g->scrolly = ny;
	*exp = (struct rect){ 0, 0, vw, vh };
	if (dx && dy)
		return true;                        /* diagonal: redesenha tudo */
	if (dy && abs(dy) < vh) {
		nsfb_bbox_t src = { 0, dy > 0 ? dy : 0, vw, dy > 0 ? vh : vh + dy };
		nsfb_bbox_t dst = { 0, dy > 0 ? 0 : -dy, vw, dy > 0 ? vh - dy : vh };
		nsfb_plot_copy(nsfb, &src, nsfb, &dst);
		*exp = dy > 0 ? (struct rect){ 0, vh - dy, vw, vh } : (struct rect){ 0, 0, vw, -dy };
	} else if (dx && abs(dx) < vw) {
		nsfb_bbox_t src = { dx > 0 ? dx : 0, 0, dx > 0 ? vw : vw + dx, vh };
		nsfb_bbox_t dst = { dx > 0 ? 0 : -dx, 0, dx > 0 ? vw - dx : vw, vh };
		nsfb_plot_copy(nsfb, &src, nsfb, &dst);
		*exp = dx > 0 ? (struct rect){ vw - dx, 0, vw, vh } : (struct rect){ 0, 0, -dx, vh };
	}
	return true;
}

static void copy_out(int x0, int y0, int x1, int y1)
{
	uint8_t *buf;
	int stride;
	if (!shm || nsfb_get_buffer(nsfb, &buf, &stride) != 0)
		return;
	for (int y = y0; y < y1; y++)
		memcpy(shm + (size_t)y * vw + x0, buf + (size_t)y * stride + x0 * 2, (size_t)(x1 - x0) * 2);
	out("R %d %d %d %d", x0, y0, x1, y1);
}

static void render(struct gui_window *g, int x0, int y0, int x1, int y1)
{
	struct redraw_context ctx = {
		.interactive = true,
		.background_images = nsoption_bool(background_images),
		.plot = &fb_plotters
	};
	struct rect clip = { x0, y0, x1, y1 };
	nsfb_bbox_t box = { x0, y0, x1, y1 };
	nsfb_claim(nsfb, &box);
	/* fundo branco: páginas sem cor de fundo não mostram lixo de antes */
	nsfb_plot_rectangle_fill(nsfb, &box, 0xFFFFFFFF);
	browser_window_redraw(g->bw, -g->scrollx, -g->scrolly, &clip, &ctx);
	if (g->caret) {
		nsfb_bbox_t line = { g->caret_x - g->scrollx, g->caret_y - g->scrolly,
				     g->caret_x - g->scrollx + 1, g->caret_y - g->scrolly + g->caret_h };
		nsfb_plot_pen_t pen = { .stroke_type = NFSB_PLOT_OPTYPE_SOLID, .stroke_width = 2,
					.stroke_colour = 0xFFE48435 };   /* azul BigFoston (ABGR) */
		if (line.x0 >= x0 - 2 && line.x0 < x1 + 2)
			nsfb_plot_line(nsfb, &line, &pen);
	}
	nsfb_update(nsfb, &box);
}

/* aplica a rolagem e o redesenho pendentes da janela ativa; true se fez algo */
static bool flush(void)
{
	struct gui_window *g = active();
	if (!g || !browser_window_redraw_ready(g->bw))
		return false;
	struct rect exp;
	if (g->pan && apply_pan(g, &exp)) {
		/* o que já estava sujo mudou de lugar: mais simples redesenhar junto */
		if (g->redraw)
			exp = (struct rect){ 0, 0, vw, vh };
		dirty_reset(g);
		render(g, exp.x0, exp.y0, exp.x1, exp.y1);
		copy_out(0, 0, vw, vh);              /* a parte movida também mudou na tela */
		report_scroll(g);
		return true;
	}
	if (!g->redraw)
		return false;
	struct rect d = g->dirty;
	dirty_reset(g);
	render(g, d.x0, d.y0, d.x1, d.y1);
	copy_out(d.x0, d.y0, d.x1, d.y1);
	return true;
}

/* ------------------------------------------------------------- tabela de janelas */

static struct gui_window *gw_create(struct browser_window *bw, struct gui_window *existing,
				    gui_window_create_flags flags)
{
	(void)existing;
	(void)flags;
	struct gui_window *g = calloc(1, sizeof *g);
	if (!g)
		return NULL;
	g->bw = bw;
	dirty_reset(g);
	g->next = windows;              /* a nova janela passa a ser a ativa */
	if (windows)
		windows->prev = g;
	windows = g;
	out("WINDOWS %d", count_windows());
	queue_redraw(g, 0, 0, vw, vh);
	return g;
}

static void gw_destroy(struct gui_window *g)
{
	if (g->prev) g->prev->next = g->next;
	else windows = g->next;
	if (g->next) g->next->prev = g->prev;
	free(g);
	if (windows) {
		queue_redraw(windows, 0, 0, vw, vh);
		out("WINDOWS %d", count_windows());
	}
}

static nserror gw_invalidate(struct gui_window *g, const struct rect *r)
{
	if (r)
		queue_redraw(g, r->x0 - g->scrollx, r->y0 - g->scrolly, r->x1 - g->scrollx, r->y1 - g->scrolly);
	else
		queue_redraw(g, 0, 0, vw, vh);
	return NSERROR_OK;
}

static bool gw_get_scroll(struct gui_window *g, int *sx, int *sy)
{
	*sx = g->scrollx + g->panx;
	*sy = g->scrolly + g->pany;
	return true;
}

static nserror gw_set_scroll(struct gui_window *g, const struct rect *r)
{
	g->panx = r->x0 - g->scrollx;
	g->pany = r->y0 - g->scrolly;
	g->pan = true;
	return NSERROR_OK;
}

static nserror gw_get_dimensions(struct gui_window *g, int *w, int *h)
{
	(void)g;
	*w = vw;
	*h = vh;
	return NSERROR_OK;
}

static void report_nav(struct gui_window *g)
{
	out("NAV %d %d", browser_window_back_available(g->bw), browser_window_forward_available(g->bw));
}

static void report_security(struct gui_window *g)
{
	browser_window_page_info_state s = browser_window_get_page_info_state(g->bw);
	out("SEC %d", (int)s);
}

static nserror gw_event(struct gui_window *g, enum gui_window_event ev)
{
	if (g != active())
		return NSERROR_OK;
	switch (ev) {
	case GW_EVENT_UPDATE_EXTENT: {
		int sx = g->scrollx, sy = g->scrolly;
		clamp_scroll(g, &sx, &sy);
		if (sx != g->scrollx || sy != g->scrolly) {
			g->scrollx = sx;
			g->scrolly = sy;
			queue_redraw(g, 0, 0, vw, vh);
		}
		report_scroll(g);
		break;
	}
	case GW_EVENT_REMOVE_CARET:
		if (g->caret) {
			g->caret = false;
			queue_redraw(g, g->caret_x - g->scrollx - 2, g->caret_y - g->scrolly,
				     g->caret_x - g->scrollx + 3, g->caret_y - g->scrolly + g->caret_h);
			out("CARET 0");
		}
		break;
	case GW_EVENT_START_THROBBER:
		out("LOAD 1");
		report_nav(g);
		break;
	case GW_EVENT_STOP_THROBBER: {
		/* página interna de erro (rede, tempo, certificado, senha): o shell diagnostica */
		struct hlcache_handle *c = browser_window_get_content(g->bw);
		nsurl *cu = c ? hlcache_handle_get_url(c) : NULL;
		const char *cs = cu ? nsurl_access(cu) : "";
		if (!strncmp(cs, "about:query/", 12))
			out("ERRPAGE %s", cs + 12);
		out("LOAD 0");
		report_nav(g);
		break;
	}
	case GW_EVENT_NEW_CONTENT:
		g->scrollx = g->scrolly = g->panx = g->pany = 0;
		g->pan = false;
		queue_redraw(g, 0, 0, vw, vh);
		report_scroll(g);
		break;
	case GW_EVENT_PAGE_INFO_CHANGE:
		report_security(g);
		break;
	default:
		break;
	}
	return NSERROR_OK;
}

static void gw_set_title(struct gui_window *g, const char *title)
{
	if (g == active())
		out("TITLE %s", title ? title : "");
}

static nserror gw_set_url(struct gui_window *g, nsurl *url)
{
	if (g == active())
		out("URL %s", nsurl_access(url));
	return NSERROR_OK;
}

static void gw_set_status(struct gui_window *g, const char *text)
{
	if (g == active())
		out("STATUS %s", text ? text : "");
}

static void gw_set_pointer(struct gui_window *g, gui_pointer_shape shape)
{
	(void)g;
	(void)shape;
}

static void gw_place_caret(struct gui_window *g, int x, int y, int height, const struct rect *clip)
{
	(void)clip;
	if (g->caret)
		queue_redraw(g, g->caret_x - g->scrollx - 2, g->caret_y - g->scrolly,
			     g->caret_x - g->scrollx + 3, g->caret_y - g->scrolly + g->caret_h);
	bool was = g->caret;
	g->caret = true;
	g->caret_x = x;
	g->caret_y = y;
	g->caret_h = height;
	queue_redraw(g, x - g->scrollx - 2, y - g->scrolly, x - g->scrollx + 3, y - g->scrolly + height);
	if (g == active())
		out("CARET 1 %d %d %d%s", x - g->scrollx, y - g->scrolly, height, was ? "" : " new");
}

static void gw_console_log(struct gui_window *g, browser_window_console_source src,
			   const char *msg, size_t msglen, browser_window_console_flags flags)
{
	(void)g;
	(void)src;
	(void)flags;
	fprintf(stderr, "js: %.*s\n", (int)msglen, msg);
}

static struct gui_window_table window_table = {
	.create = gw_create,
	.destroy = gw_destroy,
	.invalidate = gw_invalidate,
	.get_scroll = gw_get_scroll,
	.set_scroll = gw_set_scroll,
	.get_dimensions = gw_get_dimensions,
	.event = gw_event,
	.set_title = gw_set_title,
	.set_url = gw_set_url,
	.set_status = gw_set_status,
	.set_pointer = gw_set_pointer,
	.place_caret = gw_place_caret,
	.console_log = gw_console_log,
};

/* ------------------------------------------------------------- miscelânea */

static void misc_quit(void)
{
	urldb_save_cookies(nsoption_charp(cookie_jar));
	urldb_save(opt_paths[1]);
	framebuffer_finalise();
}

/* mailto:, tel: e outros esquemas: o shell decide (nunca executa nada daqui) */
static nserror misc_launch_url(struct nsurl *url)
{
	out("LAUNCH %s", nsurl_access(url));
	return NSERROR_OK;
}

static struct gui_misc_table misc_table = {
	.schedule = framebuffer_schedule,
	.quit = misc_quit,
	.launch_url = misc_launch_url,
};

/* ------------------------------------------------------------- comandos */

static struct gui_window *need(void)
{
	return active();
}

static void navigate(const char *text)
{
	struct gui_window *g = need();
	nsurl *url;
	if (!g || !*text)
		return;
	if (nsurl_create(text, &url) != NSERROR_OK) {
		out("WARN url %s", text);
		return;
	}
	browser_window_navigate(g->bw, url, NULL, BW_NAVIGATE_HISTORY, NULL, NULL, NULL);
	nsurl_unref(url);
}

static void click(int x, int y, bool long_press)
{
	struct gui_window *g = need();
	if (!g)
		return;
	int px = x + g->scrollx, py = y + g->scrolly;
	if (long_press) {                   /* menu de contexto: o que há neste ponto */
		struct browser_window_features f;
		memset(&f, 0, sizeof f);
		browser_window_get_features(g->bw, px, py, &f);
		const char *link = f.link ? nsurl_access(f.link) : "";
		const char *obj = "";
		if (f.object) {
			nsurl *u = hlcache_handle_get_url(f.object);
			obj = u ? nsurl_access(u) : "";
		}
		/* separador \t: URLs não têm tab */
		out("FEAT %d %d\t%s\t%s", x, y, link, obj);
		return;
	}
	browser_window_mouse_track(g->bw, BROWSER_MOUSE_HOVER, px, py);
	browser_window_mouse_click(g->bw, BROWSER_MOUSE_PRESS_1, px, py);
	browser_window_mouse_click(g->bw, BROWSER_MOUSE_CLICK_1, px, py);
}

static uint32_t utf8_next(const char **s)
{
	const unsigned char *p = (const unsigned char *)*s;
	uint32_t c = *p++;
	int n = c < 0x80 ? 0 : (c & 0xe0) == 0xc0 ? 1 : (c & 0xf0) == 0xe0 ? 2 : (c & 0xf8) == 0xf0 ? 3 : -1;
	if (n < 0) { *s = (const char *)p; return 0xfffd; }
	if (n) c &= 0x3f >> n;
	while (n-- > 0) {
		if ((*p & 0xc0) != 0x80) { *s = (const char *)p; return 0xfffd; }
		c = c << 6 | (*p++ & 0x3f);
	}
	*s = (const char *)p;
	return c;
}

static const struct { const char *name; uint32_t key; } keys[] = {
	{ "enter", NS_KEY_NL }, { "backspace", NS_KEY_DELETE_LEFT }, { "delete", NS_KEY_DELETE_RIGHT },
	{ "left", NS_KEY_LEFT }, { "right", NS_KEY_RIGHT }, { "up", NS_KEY_UP }, { "down", NS_KEY_DOWN },
	{ "tab", NS_KEY_TAB }, { "shifttab", NS_KEY_SHIFT_TAB }, { "escape", NS_KEY_ESCAPE },
	{ "home", NS_KEY_LINE_START }, { "end", NS_KEY_LINE_END },
	{ "pageup", NS_KEY_PAGE_UP }, { "pagedown", NS_KEY_PAGE_DOWN },
	{ "selectall", NS_KEY_SELECT_ALL }, { "copy", NS_KEY_COPY_SELECTION }, { "paste", NS_KEY_PASTE },
};

static void key(const char *name)
{
	struct gui_window *g = need();
	if (!g)
		return;
	for (size_t i = 0; i < sizeof keys / sizeof keys[0]; i++)
		if (!strcmp(name, keys[i].name)) {
			if (!browser_window_key_press(g->bw, keys[i].key)) {
				/* a página não usou a tecla: rola */
				int dy = keys[i].key == NS_KEY_PAGE_DOWN ? vh - 40 : keys[i].key == NS_KEY_PAGE_UP ? -(vh - 40)
				       : keys[i].key == NS_KEY_DOWN ? 40 : keys[i].key == NS_KEY_UP ? -40 : 0;
				if (dy) {
					g->pany += dy;
					g->pan = true;
				}
			}
			return;
		}
}

static void text(const char *s)
{
	struct gui_window *g = need();
	if (!g)
		return;
	while (*s) {
		uint32_t c = utf8_next(&s);
		if (c >= 0x20)
			browser_window_key_press(g->bw, c);
	}
}

static void zoom(int percent)
{
	struct gui_window *g = need();
	if (!g || percent < 30 || percent > 400)
		return;
	browser_window_set_scale(g->bw, percent / 100.0f, true);
	out("ZOOM %d", (int)(browser_window_get_scale(g->bw) * 100 + 0.5f));
}

const char *bf_current_url(void)
{
	struct gui_window *g = active();
	nsurl *u = g ? browser_window_access_url(g->bw) : NULL;
	return u ? nsurl_access(u) : "";
}

/* menu de toque longo: "baixar link/imagem" (vem como download, com cookies e origem) */
static void download_url(const char *text)
{
	struct gui_window *g = active();
	nsurl *url, *ref = NULL;
	if (!g || nsurl_create(text, &url) != NSERROR_OK)
		return;
	if (browser_window_access_url(g->bw))
		ref = nsurl_ref(browser_window_access_url(g->bw));
	browser_window_navigate(g->bw, url, ref, BW_NAVIGATE_DOWNLOAD, NULL, NULL, NULL);
	if (ref)
		nsurl_unref(ref);
	nsurl_unref(url);
}

static void command(char *line)
{
	char *arg = strchr(line, ' ');
	if (arg)
		*arg++ = 0;
	else
		arg = line + strlen(line);
	struct gui_window *g = active();
	int a = 0, b = 0;

	if (!strcmp(line, "GO")) navigate(arg);
	else if (!strcmp(line, "BACK")) { if (g && browser_window_back_available(g->bw)) browser_window_history_back(g->bw, false); }
	else if (!strcmp(line, "FWD")) { if (g && browser_window_forward_available(g->bw)) browser_window_history_forward(g->bw, false); }
	else if (!strcmp(line, "RELOAD")) { if (g) browser_window_reload(g->bw, true); }
	else if (!strcmp(line, "STOP")) { if (g) browser_window_stop(g->bw); }
	else if (!strcmp(line, "CLICK") && sscanf(arg, "%d %d", &a, &b) == 2) click(a, b, false);
	else if (!strcmp(line, "LONG") && sscanf(arg, "%d %d", &a, &b) == 2) click(a, b, true);
	else if (!strcmp(line, "SCROLL") && sscanf(arg, "%d %d", &a, &b) == 2) {
		if (g) {
			/* a página pode rolar sozinha um quadro interno (iframe, div com overflow) */
			if (!browser_window_scroll_at_point(g->bw, g->scrollx + vw / 2, g->scrolly + vh / 2, a, b)) {
				g->panx += a;
				g->pany += b;
				g->pan = true;
			}
		}
	}
	else if (!strcmp(line, "SCROLLTO") && sscanf(arg, "%d %d", &a, &b) == 2) {
		if (g) { g->panx = a - g->scrollx; g->pany = b - g->scrolly; g->pan = true; }
	}
	else if (!strcmp(line, "KEY")) key(arg);
	else if (!strcmp(line, "TEXT")) text(arg);
	else if (!strcmp(line, "ZOOM") && sscanf(arg, "%d", &a) == 1) zoom(a);
	else if (!strcmp(line, "REDRAW")) { if (g) queue_redraw(g, 0, 0, vw, vh); }
	else if (!strcmp(line, "CLOSEWIN")) {          /* volta da janela aberta por um link */
		if (g && g->next) browser_window_destroy(g->bw);
	}
	else if (!strcmp(line, "DOWNLOAD")) download_url(arg);
	else if (!strcmp(line, "QUIT")) done = true;
	else if (*line) out("WARN comando %s", line);
}

static char cmdbuf[CMD_MAX];
static size_t cmdlen;

static void read_commands(void)
{
	ssize_t n = read(0, cmdbuf + cmdlen, sizeof cmdbuf - 1 - cmdlen);
	if (n <= 0) {
		if (n == 0 || (errno != EINTR && errno != EAGAIN))
			done = true;              /* o shell fechou o pipe */
		return;
	}
	cmdlen += n;
	char *start = cmdbuf, *nl;
	while ((nl = memchr(start, '\n', cmdbuf + cmdlen - start))) {
		*nl = 0;
		if (nl > start && nl[-1] == '\r')
			nl[-1] = 0;
		command(start);
		start = nl + 1;
	}
	cmdlen -= start - cmdbuf;
	memmove(cmdbuf, start, cmdlen);
	if (cmdlen == sizeof cmdbuf - 1)          /* linha grande demais: descarta */
		cmdlen = 0;
}

/* ------------------------------------------------------------- laço */

static void run(void)
{
	while (!done) {
		int timeout = schedule_run();
		/* coalesce: comandos já chegados entram antes de desenhar */
		fd_set rd, wr, ex;
		int maxfd = -1;
		FD_ZERO(&rd);
		FD_ZERO(&wr);
		FD_ZERO(&ex);
		fetch_fdset(&rd, &wr, &ex, &maxfd);
		FD_SET(0, &rd);
		if (maxfd < 0)
			maxfd = 0;
		struct gui_window *g = active();
		bool pending = g && (g->pan || g->redraw) && browser_window_redraw_ready(g->bw);
		struct timeval tv, *tvp = NULL;
		if (pending)
			timeout = 0;
		if (timeout >= 0) {
			tv.tv_sec = timeout / 1000;
			tv.tv_usec = (timeout % 1000) * 1000;
			tvp = &tv;
		}
		int r = select(maxfd + 1, &rd, &wr, &ex, tvp);
		if (r < 0 && errno != EINTR) {
			NSLOG(netsurf, CRITICAL, "select: %s", strerror(errno));
			break;
		}
		if (r > 0 && FD_ISSET(0, &rd))
			read_commands();
		flush();
	}
}

/* ------------------------------------------------------------- perfil */

/* BFWEB_PROF=arquivo: amostra o PC a cada 2 ms de CPU e grava no fim, com o
 * mapa de memória, para tools/bigfoston/web-prof.py atribuir às funções */
#include <sys/time.h>
#include <ucontext.h>
#define PROF_MAX 200000
static unsigned long *prof_pc;
static volatile int prof_n;
static const char *prof_file;

static void prof_sig(int sig, siginfo_t *si, void *uc)
{
	(void)sig;
	(void)si;
	unsigned long pc = 0;
#if defined(__arm__)
	pc = ((ucontext_t *)uc)->uc_mcontext.arm_pc;
#elif defined(__x86_64__)
	pc = (unsigned long)((ucontext_t *)uc)->uc_mcontext.gregs[REG_RIP];
#else
	(void)uc;
#endif
	if (prof_n < PROF_MAX)
		prof_pc[prof_n++] = pc;
}

static void prof_start(void)
{
	prof_file = getenv("BFWEB_PROF");
	if (!prof_file || !(prof_pc = malloc(sizeof *prof_pc * PROF_MAX)))
		return;
	struct sigaction sa = { .sa_sigaction = prof_sig, .sa_flags = SA_SIGINFO | SA_RESTART };
	sigaction(SIGPROF, &sa, NULL);
	struct itimerval it = { { 0, 2000 }, { 0, 2000 } };
	setitimer(ITIMER_PROF, &it, NULL);
}

static void prof_stop(void)
{
	if (!prof_pc)
		return;
	struct itimerval it = { { 0, 0 }, { 0, 0 } };
	setitimer(ITIMER_PROF, &it, NULL);
	FILE *f = fopen(prof_file, "w");
	if (!f)
		return;
	FILE *m = fopen("/proc/self/maps", "r");
	char line[512];
	while (m && fgets(line, sizeof line, m))
		if (strstr(line, " r-xp ") || strstr(line, " r-xs "))
			fprintf(f, "MAP %s", line);
	if (m)
		fclose(m);
	for (int i = 0; i < prof_n; i++)
		fprintf(f, "%lx\n", prof_pc[i]);
	fclose(f);
}

/* ------------------------------------------------------------- opções */

static nserror set_defaults(struct nsoption_s *defaults)
{
	(void)defaults;
	snprintf(opt_paths[0], sizeof opt_paths[0], "%s/Cookies", data_dir);
	snprintf(opt_paths[1], sizeof opt_paths[1], "%s/URLs", data_dir);
	nsoption_setnull_charp(cookie_file, strdup(opt_paths[0]));
	nsoption_setnull_charp(cookie_jar, strdup(opt_paths[0]));
	if (!nsoption_charp(cookie_file) || !nsoption_charp(cookie_jar))
		return NSERROR_BAD_PARAMETER;
	/* 128 MB de RAM: caches pequenos, poucas conexões, nada de janelas novas */
	nsoption_set_int(memory_cache_size, 3 * 1024 * 1024);
	/* sem cache em disco: não gasta o cartão (cache só na RAM) */
	nsoption_set_int(max_fetchers, 6);
	nsoption_set_int(max_fetchers_per_host, 2);
	nsoption_set_int(max_cached_fetch_handles, 2);
	nsoption_set_bool(target_blank, false);
	nsoption_set_int(font_size, 120);
	nsoption_set_int(font_min_size, 90);
	nsoption_set_charp(accept_language, strdup("pt-BR, pt, en"));
	nsoption_set_int(fb_font_cachesize, 1024);
	return NSERROR_OK;
}

static bool log_configure(FILE *f)
{
	setbuf(f, NULL);
	return true;
}

static void usage(const char *a0)
{
	fprintf(stderr, "uso: %s [--fd N | --shm ARQ] [-w L] [-h A] [--data DIR] [--res DIR]\n"
			"          [--js] [--no-images] [url]\n", a0);
	exit(2);
}

static int map_shared(int fd, const char *path)
{
	if (path) {
		fd = open(path, O_RDWR | O_CLOEXEC);
		if (fd < 0)
			return -1;
	}
	size_t n = (size_t)vw * vh * 2;
	struct stat st;
	if (fstat(fd, &st) || (size_t)st.st_size < n)
		return -1;
	void *m = mmap(NULL, n, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (m == MAP_FAILED)
		return -1;
	shm = m;
	return 0;
}

int main(int argc, char **argv)
{
	int shm_fd = -1;
	const char *shm_path = NULL, *res = NULL;
	bool js = false, no_images = false;
	signal(SIGPIPE, SIG_IGN);

	for (int i = 1; i < argc; i++) {
		const char *a = argv[i];
		const char *v = i + 1 < argc ? argv[i + 1] : NULL;
		if (!strcmp(a, "--fd") && v) { shm_fd = atoi(v); i++; }
		else if (!strcmp(a, "--shm") && v) { shm_path = v; i++; }
		else if (!strcmp(a, "-w") && v) { vw = atoi(v); i++; }
		else if (!strcmp(a, "-h") && v) { vh = atoi(v); i++; }
		else if (!strcmp(a, "--data") && v) { snprintf(data_dir, sizeof data_dir, "%s", v); i++; }
		else if (!strcmp(a, "--res") && v) { res = v; i++; }
		else if (!strcmp(a, "--js")) js = true;
		else if (!strcmp(a, "--no-images")) no_images = true;
		else if (a[0] == '-' && a[1] == '-') usage(argv[0]);
		else start_url = a;
	}
	if (vw < 64 || vh < 64 || vw > 2048 || vh > 2048)
		usage(argv[0]);
	if ((shm_fd >= 0 || shm_path) && map_shared(shm_fd, shm_path)) {
		fprintf(stderr, "bfweb: memória compartilhada inválida: %s\n", strerror(errno));
		return 1;
	}
	mkdir(data_dir, 0700);

	struct netsurf_table table = {
		.misc = &misc_table,
		.window = &window_table,
		.download = bf_download_table,
		.fetch = framebuffer_fetch_table,
		.utf8 = framebuffer_utf8_table,
		.bitmap = framebuffer_bitmap_table,
		.layout = framebuffer_layout_table,
	};
	if (netsurf_register(&table) != NSERROR_OK) {
		fprintf(stderr, "bfweb: registro das tabelas falhou\n");
		return 1;
	}

	char rp[PATH_MAX * 2];
	snprintf(rp, sizeof rp, "%s:%s/fonts", res ? res : "./res", res ? res : "./res");
	respaths = fb_init_resource_path(rp);

	nslog_init(log_configure, &argc, argv);
	if (nsoption_init(set_defaults, &nsoptions, &nsoptions_default) != NSERROR_OK) {
		fprintf(stderr, "bfweb: opções\n");
		return 1;
	}
	char *choices = filepath_find(respaths, "Choices");
	nsoption_read(choices, nsoptions);
	free(choices);
	nsoption_set_bool(enable_javascript, js);
	/* certificados que vão junto do app (o rootfs do firmware não tem /etc/ssl);
	 * a verificação de certificado nunca é desligada */
	char *ca = filepath_find(respaths, "ca-bundle.crt");
	if (ca) {
		nsoption_set_charp(ca_bundle, ca);
		nsoption_set_charp(ca_path, strdup(""));
	} else
		fprintf(stderr, "bfweb: sem ca-bundle.crt: HTTPS vai recusar os certificados\n");
	if (no_images) {
		nsoption_set_bool(foreground_images, false);
		nsoption_set_bool(background_images, false);
	}

	char *messages = filepath_find(respaths, "Messages");
	if (messages_add_from_file(messages) != NSERROR_OK)
		fprintf(stderr, "bfweb: sem Messages\n");
	free(messages);
	/* português por cima do inglês (a última definição de cada chave vale) */
	messages = filepath_find(respaths, "Messages.pt");
	if (messages && messages_add_from_file(messages) != NSERROR_OK)
		fprintf(stderr, "bfweb: Messages.pt inválido\n");
	free(messages);

	if (netsurf_init(NULL) != NSERROR_OK) {
		fprintf(stderr, "bfweb: netsurf_init falhou\n");
		return 1;
	}
	nsoption_set_bool(core_select_menu, true);   /* <select> desenhado pela própria página */

	nsfb = framebuffer_initialise("ram", vw, vh, 16);
	if (!nsfb) {
		fprintf(stderr, "bfweb: superfície ram\n");
		return 1;
	}
	if (!fb_font_init()) {
		fprintf(stderr, "bfweb: fontes\n");
		return 1;
	}
	urldb_load(opt_paths[1]);
	urldb_load_cookies(nsoption_charp(cookie_file));

	nsurl *url;
	struct browser_window *bw = NULL;
	if (nsurl_create(start_url ? start_url : "about:blank", &url) == NSERROR_OK) {
		browser_window_create(BW_CREATE_HISTORY, url, NULL, NULL, &bw);
		nsurl_unref(url);
	}
	if (!bw) {
		fprintf(stderr, "bfweb: janela\n");
		return 1;
	}
	out("READY %d %d", vw, vh);
	prof_start();
	run();
	prof_stop();

	while (windows)
		browser_window_destroy(windows->bw);
	netsurf_exit();
	fb_font_finalise();
	nsoption_finalise(nsoptions, nsoptions_default);
	nslog_finalise();
	return 0;
}
