/* imgload.h — imagens de arquivo para a tela: JPEG (baseline), PNG, BMP,
 * GIF (primeiro quadro) e SVG, reduzidas para caber numa caixa e convertidas
 * para RGB565 (transparência misturada com a cor de fundo).
 *
 * Limites (128 MB de RAM, ARM sem FPU): PNG até 6 Mpx (decodifica inteiro),
 * GIF até 1600x1600 (quadro inteiro em ARGB), BMP 1/4/8/24/32 bits sem
 * compressão. JPEG progressivo não é suportado pelo TJpgDec. */
#ifndef BF_IMGLOAD_H
#define BF_IMGLOAD_H
#include "lvgl.h"
#include <stdbool.h>

typedef enum { IMG_UNKNOWN, IMG_JPEG, IMG_PNG, IMG_BMP, IMG_GIF, IMG_SVG } img_kind_t;

/* pela extensão (para listar arquivos) */
img_kind_t img_kind_of(const char *path);
bool       img_is_image(const char *path);
/* tamanho natural sem decodificar (SVG: o tamanho do documento) */
bool       img_probe(const char *path, int *w, int *h);
/* decodifica cabendo em max_w x max_h (nunca amplia, exceto SVG até a caixa).
 * Pixels alocados: liberar com img_free. err (opcional) recebe o motivo. */
bool       img_load_fit(const char *path, int max_w, int max_h, lv_color_t bg,
                        lv_image_dsc_t *out, char *err, size_t errn);
void       img_free(lv_image_dsc_t *img);
#endif
