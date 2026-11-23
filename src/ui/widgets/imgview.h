/* imgview.h — visualizador de imagem no layout do leitor de livros: a barra
 * superior do sistema continua visível, a imagem ocupa a área de conteúdo e
 * um toque mostra/esconde os controles flutuantes:
 *   em cima:  ◀ voltar · nome · zoom − · % · zoom + · Ajustar · girar ⟲ ⟳
 *   embaixo:  ◀ barra entre as imagens N / M ▶ (Galeria) · resolução, tamanho, tipo
 * JPEG, PNG, BMP, GIF e SVG (core/imgload): reduzidos para caber; o zoom vai
 * até a resolução original (SVG re-rasterizado em cada nível, até 2048 px).
 * Girar é em passos de 90° (os pixels são girados, sem custo por quadro).
 * GIF de até 1200 px e 2 MB toca animado (sem zoom nem giro).
 * Com imgview_set_nav, ◀ ▶, a barra e deslizar trocam de imagem (Galeria).
 * O Voltar do sistema (nav_back) fecha a imagem antes de sair da tela. */
#ifndef BF_IMGVIEW_H
#define BF_IMGVIEW_H
#include <stdbool.h>

/* pede para abrir a imagem `index` (0..count-1). Chamado dentro de eventos do
 * visualizador: quem implementa deve reabrir com lv_async_call. */
typedef void (*imgview_nav_cb_t)(int index, void *ud);

/* false só quando o arquivo não é imagem; um arquivo ruim abre com o aviso */
bool imgview_open(const char *path);
void imgview_close(void);                  /* também desliga a navegação */
bool imgview_is_open(void);
/* chamar antes de imgview_open; vale até imgview_close */
void imgview_set_nav(imgview_nav_cb_t cb, void *ud, int index, int count);
void imgview_show_controls(bool on);
void imgview_zoom(int dir);                /* -1 menos, +1 mais (x1,5), 0 ajustar à tela */
void imgview_rotate(int dir);              /* -1 anti-horário, +1 horário */
#endif
