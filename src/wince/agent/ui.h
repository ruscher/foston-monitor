/* ui.h — a tela "Dual Boot" do agente (Windows CE): escolher entre o BigFoston (Linux) e o
 * Windows CE, a contagem do início automático com barra de progresso e o registro do que o
 * agente está fazendo, em várias linhas.
 *
 * Desenho próprio num buffer RGB565 (o formato da tela), com bordas suaves, degradês com
 * pontilhado (sem faixas no 16 bits) e ícones com alfa; o GDI só escreve o texto (Tahoma) e
 * copia o quadro pronto para a tela de uma vez (sem piscar). Só usa funções que o coredll do
 * FOSTON exporta e que o agente já usava: CreateDIBSection, CreateCompatibleDC, BitBlt,
 * DrawTextW, CreateFontIndirectW. Sem ponto flutuante (o ARM926 não tem FPU).
 *
 * O mesmo arquivo compila com o winegcc no PC (make preview) para ver a tela sem o aparelho.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef BF_AGENT_UI_H
#define BF_AGENT_UI_H
#include <windows.h>

enum { UI_HIT_NONE, UI_HIT_LINUX, UI_HIT_CE, UI_HIT_RESTORE, UI_HIT_OV_OK, UI_HIT_OV_CANCEL };
/* janela de confirmação desenhada por cima (só ela recebe toques) */
enum { UI_OV_NONE, UI_OV_INSTALL, UI_OV_RESTORE, UI_OV_DONE };
/* aberto pelo cartão sem o Dual Boot instalado */
enum { UI_SETUP_NO, UI_SETUP_READY, UI_SETUP_NOFILES };
/* tipo da linha do registro: o ícone e a cor */
enum { UI_L_INFO, UI_L_OK, UI_L_WARN, UI_L_ERR, UI_L_PC };

#define UI_LINE_CHARS 200
#define UI_LINES      8

typedef struct {
    int kind;
    WCHAR text[UI_LINE_CHARS];
} ui_line_t;

typedef struct {
    int autoboot;               /* 0 nada, 1 contando, 2 cancelado, 3 iniciando */
    int linux_ok;               /* Linux instalado na memória interna */
    DWORD cd_total_ms;          /* contagem: duração e quanto já passou */
    DWORD cd_elapsed_ms;
    DWORD requests;             /* pedidos do PC atendidos */
    int pressed;                /* UI_HIT_* com o dedo em cima agora */
    const WCHAR *version;       /* "bfagent 0.7.0" */
    const WCHAR *port;          /* porta USB serial ("" sem cabo) */
    int nlines;                 /* registro, a mais antiga primeiro */
    const ui_line_t *lines;
    int setup;                  /* UI_SETUP_*: o cartão BigFoston vira "Instalar o Dual Boot" */
    int can_restore;            /* "Restaurar o original" na barra superior */
    int busy;                   /* instalando ou restaurando: os cartões não respondem */
    int overlay;                /* UI_OV_* */
    const WCHAR *ov_title, *ov_body;   /* textos de UI_OV_DONE */
    const WCHAR *status;        /* a faixa do meio quando busy/setup (NULL: pela contagem) */
} ui_state_t;

/* cria os buffers e as fontes (uma vez); 0 = falhou (a tela fica sem desenho próprio) */
int ui_init(HDC screen, int width, int height);
void ui_free(void);
/* desenha o estado e copia para dst a área clip (NULL = tudo) */
void ui_paint(HDC dst, const ui_state_t *s, const RECT *clip);
int ui_hit(int x, int y);
/* com a confirmação aberta, só os botões dela */
int ui_hit_overlay(int x, int y);
/* a faixa da contagem (para redesenhar só ela durante a animação) */
void ui_progress_rect(RECT *r);
#endif
