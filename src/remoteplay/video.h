/* video.h — vídeo do Big Remote Play no framebuffer (video.c).
 * SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef BFRP_VIDEO_H
#define BFRP_VIDEO_H
#include <stdbool.h>
#include <Limelight.h>

int  video_open(int x, int y, int w, int h);    /* retângulo da tela onde o vídeo aparece */
int  video_setup(int w, int h);                 /* abre o decodificador H.264 */
int  video_submit(PDECODE_UNIT du);             /* DR_OK ou DR_NEED_IDR */
void video_show(int on);                        /* 0: não desenha (um diálogo por cima) */
void video_stats(unsigned long *frames, unsigned long *errors, unsigned long long *us);
void video_loop_filter(int on);                 /* antes do video_setup */
void video_smooth(int on);                      /* ampliação suave (padrão) ou em blocos */
void video_clip(int rows);                      /* não pinta as últimas linhas (barra por cima) */
void video_zoom(int pct);                       /* 100..400 % */
void video_pan(int dx, int dy, int aw, int ah); /* arrastar com zoom (pontos da área l x a) */
int  video_map(int ax, int ay, int aw, int ah, int *fx, int *fy, int *fw, int *fh);   /* toque -> quadro */
void video_close(void);
#endif
