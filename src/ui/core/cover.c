/* cover.c — capa de álbum: JPEG -> RGB565 COVER_PX x COVER_PX.
 * Usa o TJpgDec que acompanha o LVGL (LV_USE_TJPGD) diretamente, para
 * reduzir a imagem enquanto decodifica (amostragem do pixel mais próximo):
 * nada de imagem grande na RAM nem transformação a cada quadro no ARM sem
 * FPU. Uma decodificação por faixa. */
#include "cover.h"
#include "../third_party/lvgl/src/libs/tjpgd/tjpgd.h"
#include <stdlib.h>
#include <string.h>

static uint16_t pixels[COVER_PX * COVER_PX];
static lv_image_dsc_t img;

typedef struct {
    const uint8_t *data;
    size_t len, pos;
    unsigned w, h;
} src_t;

static size_t in_func(JDEC *jd, uint8_t *buf, size_t n)
{
    src_t *s = jd->device;
    if(n > s->len - s->pos) n = s->len - s->pos;
    if(buf) memcpy(buf, s->data + s->pos, n);
    s->pos += n;
    return n;
}

/* bloco RGB888 decodificado -> pixels da miniatura que caem nele */
static int out_func(JDEC *jd, void *bitmap, JRECT *r)
{
    src_t *s = jd->device;
    const uint8_t *rgb = bitmap;
    unsigned bw = r->right - r->left + 1;
    unsigned y0 = (r->top * COVER_PX + s->h - 1) / s->h, y1 = ((r->bottom + 1) * COVER_PX - 1) / s->h;
    unsigned x0 = (r->left * COVER_PX + s->w - 1) / s->w, x1 = ((r->right + 1) * COVER_PX - 1) / s->w;
    for(unsigned ty = y0; ty <= y1 && ty < COVER_PX; ty++) {
        unsigned sy = ty * s->h / COVER_PX;
        if(sy < r->top || sy > r->bottom) continue;
        for(unsigned tx = x0; tx <= x1 && tx < COVER_PX; tx++) {
            unsigned sx = tx * s->w / COVER_PX;
            if(sx < r->left || sx > r->right) continue;
            const uint8_t *p = rgb + 3 * ((sy - r->top) * bw + (sx - r->left));
            pixels[ty * COVER_PX + tx] = (uint16_t)(((p[0] >> 3) << 11) | ((p[1] >> 2) << 5) | (p[2] >> 3));
        }
    }
    return 1;
}

const lv_image_dsc_t *cover_decode(const uint8_t *data, size_t len)
{
    if(!data || len < 4 || data[0] != 0xFF || data[1] != 0xD8) return NULL;   /* só JPEG */
    void *pool = malloc(16 * 1024);
    if(!pool) return NULL;
    JDEC jd;
    src_t s = { data, len, 0, 0, 0 };
    const lv_image_dsc_t *ret = NULL;
    if(jd_prepare(&jd, in_func, pool, 16 * 1024, &s) == JDR_OK && jd.width && jd.height) {
        s.w = jd.width;
        s.h = jd.height;
        memset(pixels, 0, sizeof pixels);
        if(jd_decomp(&jd, out_func, 0) == JDR_OK) {
            memset(&img, 0, sizeof img);
            img.header.magic = LV_IMAGE_HEADER_MAGIC;
            img.header.cf = LV_COLOR_FORMAT_RGB565;
            img.header.w = COVER_PX;
            img.header.h = COVER_PX;
            img.header.stride = COVER_PX * 2;
            img.data = (const uint8_t *)pixels;
            img.data_size = sizeof pixels;
            lv_image_cache_drop(&img);     /* mesma fonte, conteúdo novo */
            ret = &img;
        }
    }
    free(pool);
    return ret;
}
