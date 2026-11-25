/* download.c — downloads do BigFoston Web: o motor só repassa o pedido.
 *
 * O bfweb roda sem privilégios e não grava no cartão. Quando uma resposta é um
 * download, ele manda ao shell
 *
 *   DLREQ <url>\t<nome sugerido>\t<tamanho ou 0>\t<tipo MIME>\t<página de origem>\t<cookie>
 *
 * e cancela a própria transferência (create devolve NULL: o núcleo aborta e libera
 * o contexto). O shell entrega ao bfdl, que grava em BigFoston/Downloads com .part,
 * pausa e continuação (HTTP Range) e funciona mesmo com o navegador fechado.
 * Limitação: um download que é a resposta de um POST não pode ser repetido por URL.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "utils/nsurl.h"
#include "netsurf/download.h"
#include "netsurf/browser_window.h"
#include "desktop/download.h"
#include "content/urldb.h"

#include "bigfoston/download.h"
#include "bigfoston/out.h"

/* sem tab, quebra de linha nem controles: o campo vai numa linha do protocolo */
static void field(char *s)
{
	for (; *s; s++)
		if ((unsigned char)*s < 0x20)
			*s = ' ';
}

static struct gui_download_window *dl_create(download_context *ctx, struct gui_window *parent)
{
	(void)parent;
	nsurl *u = download_context_get_url(ctx);
	if (!u)
		return NULL;
	char name[256], mime[96];
	snprintf(name, sizeof name, "%s", download_context_get_filename(ctx));
	snprintf(mime, sizeof mime, "%s", download_context_get_mime_type(ctx));
	field(name);
	field(mime);
	char *cookie = urldb_get_cookie(u, true);
	if (cookie)
		field(cookie);
	out("DLREQ %s\t%s\t%llu\t%s\t%s\t%s", nsurl_access(u), name,
	    download_context_get_total_length(ctx), mime, bf_current_url(), cookie ? cookie : "");
	free(cookie);
	return NULL;                       /* o núcleo aborta e libera o contexto */
}

static nserror dl_data(struct gui_download_window *dw, const char *data, unsigned int size)
{
	(void)dw;
	(void)data;
	(void)size;
	return NSERROR_SAVE_FAILED;
}

static void dl_error(struct gui_download_window *dw, const char *msg)
{
	(void)dw;
	(void)msg;
}

static void dl_done(struct gui_download_window *dw)
{
	(void)dw;
}

static struct gui_download_table table = {
	.create = dl_create,
	.data = dl_data,
	.error = dl_error,
	.done = dl_done,
};

struct gui_download_table *bf_download_table = &table;
