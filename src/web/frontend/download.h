/* download.h — repasse de downloads ao shell (download.c). SPDX-License-Identifier: GPL-2.0-only */
#ifndef BIGFOSTON_DOWNLOAD_H
#define BIGFOSTON_DOWNLOAD_H
extern struct gui_download_table *bf_download_table;
const char *bf_current_url(void);        /* gui.c: página da janela ativa ("" se nenhuma) */
#endif
