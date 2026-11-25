/* volpop.h — controle de volume flutuante: barra vertical, porcentagem e um
 * botão de mudo (o ícone troca para mudo). Abre pelo ícone de som da barra
 * superior (qualquer tela) e pelo Multimídia; fecha ao tocar fora ou após
 * 5 s sem uso. Mudo = volume 0; desmutar volta ao volume anterior. */
#ifndef BF_VOLPOP_H
#define BF_VOLPOP_H
#include "lvgl.h"
#include <stdbool.h>

/* x: centro do painel; y: topo (ajustados para caber na tela). Abrir de novo fecha. */
void volpop_open(int x, int y);
void volpop_close(void);
bool volpop_is_open(void);
void volpop_toggle_mute(void);
/* teclas de volume: abre o painel (se fechado) e muda delta pontos */
void volpop_step(int delta);
/* símbolo de volume para pct (mudo, médio, alto) */
const char *volpop_symbol(int pct);
#endif
