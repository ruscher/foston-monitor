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
    unsigned w, h;          /* imagem de origem */
    unsigned dw, dh;        /* destino */
    uint16_t *out;
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
    unsigned y0 = (r->top * s->dh + s->h - 1) / s->h, y1 = ((r->bottom + 1) * s->dh - 1) / s->h;
    unsigned x0 = (r->left * s->dw + s->w - 1) / s->w, x1 = ((r->right + 1) * s->dw - 1) / s->w;
    for(unsigned ty = y0; ty <= y1 && ty < s->dh; ty++) {
        unsigned sy = ty * s->h / s->dh;
        if(sy < r->top || sy > r->bottom) continue;
        for(unsigned tx = x0; tx <= x1 && tx < s->dw; tx++) {
            unsigned sx = tx * s->w / s->dw;
            if(sx < r->left || sx > r->right) continue;
            const uint8_t *p = rgb + 3 * ((sy - r->top) * bw + (sx - r->left));
            s->out[ty * s->dw + tx] = (uint16_t)(((p[0] >> 3) << 11) | ((p[1] >> 2) << 5) | (p[2] >> 3));
        }
    }
    return 1;
}

/* decodifica para dw x dh (dw/dh = 0: cabe em max_w x max_h mantendo a proporção) */
static bool jpeg_scaled(const uint8_t *data, size_t len, unsigned max_w, unsigned max_h,
                        bool square, uint16_t **out, unsigned *ow, unsigned *oh, uint16_t *fixed)
{
    if(!data || len < 4 || data[0] != 0xFF || data[1] != 0xD8) return false;   /* só JPEG */
    void *pool = malloc(16 * 1024);
    if(!pool) return false;
    JDEC jd;
    src_t s = { data, len, 0, 0, 0, 0, 0, NULL };
    bool ok = false;
    if(jd_prepare(&jd, in_func, pool, 16 * 1024, &s) == JDR_OK && jd.width && jd.height) {
        s.w = jd.width;
        s.h = jd.height;
        if(square) { s.dw = max_w; s.dh = max_h; }
        else if((unsigned long)s.w * max_h > (unsigned long)s.h * max_w) {
            s.dw = s.w < max_w ? s.w : max_w;
            s.dh = s.h * s.dw / s.w;
        }
        else {
            s.dh = s.h < max_h ? s.h : max_h;
            s.dw = s.w * s.dh / s.h;
        }
        if(!s.dw) s.dw = 1;
        if(!s.dh) s.dh = 1;
        /* o TJpgDec reduz 1/2, 1/4 ou 1/8 enquanto decodifica (JD_USE_SCALE):
         * a maior redução que ainda cobre o destino, muito mais rápido em fotos grandes */
        uint8_t sc = 0;            /* até 1/4: o 1/8 (só DC) sai corrompido nesta configuração */
        while(sc < 2 && (s.w >> (sc + 1)) >= s.dw && (s.h >> (sc + 1)) >= s.dh) sc++;
        s.w >>= sc;
        s.h >>= sc;
        s.out = fixed ? fixed : malloc((size_t)s.dw * s.dh * 2);
        if(s.out) {
            memset(s.out, 0, (size_t)s.dw * s.dh * 2);
            if(jd_decomp(&jd, out_func, sc) == JDR_OK) {
                ok = true;
                *out = s.out;
                *ow = s.dw;
                *oh = s.dh;
            }
            else if(!fixed) free(s.out);
        }
    }
    free(pool);
    return ok;
}

bool jpeg_size(const uint8_t *data, size_t len, unsigned *w, unsigned *h)
{
    if(!data || len < 4 || data[0] != 0xFF || data[1] != 0xD8) return false;
    void *pool = malloc(16 * 1024);
    if(!pool) return false;
    JDEC jd;
    src_t s = { data, len, 0, 0, 0, 0, 0, NULL };
    bool ok = jd_prepare(&jd, in_func, pool, 16 * 1024, &s) == JDR_OK && jd.width && jd.height;
    if(ok) { *w = jd.width; *h = jd.height; }
    free(pool);
    return ok;
}

bool jpeg_fit(const uint8_t *data, size_t len, unsigned max_w, unsigned max_h, lv_image_dsc_t *img)
{
    uint16_t *px;
    unsigned w, h;
    if(!jpeg_scaled(data, len, max_w, max_h, false, &px, &w, &h, NULL)) return false;
    memset(img, 0, sizeof *img);
    img->header.magic = LV_IMAGE_HEADER_MAGIC;
    img->header.cf = LV_COLOR_FORMAT_RGB565;
    img->header.w = w;
    img->header.h = h;
    img->header.stride = w * 2;
    img->data = (const uint8_t *)px;
    img->data_size = w * h * 2;
    return true;
}

void jpeg_free(lv_image_dsc_t *img)
{
    if(img->data) {
        lv_image_cache_drop(img);
        free((void *)img->data);
        img->data = NULL;
    }
}

const lv_image_dsc_t *cover_decode(const uint8_t *data, size_t len)
{
    uint16_t *px;
    unsigned w, h;
    if(!jpeg_scaled(data, len, COVER_PX, COVER_PX, true, &px, &w, &h, pixels)) return NULL;
    memset(&img, 0, sizeof img);
    img.header.magic = LV_IMAGE_HEADER_MAGIC;
    img.header.cf = LV_COLOR_FORMAT_RGB565;
    img.header.w = COVER_PX;
    img.header.h = COVER_PX;
    img.header.stride = COVER_PX * 2;
    img.data = (const uint8_t *)pixels;
    img.data_size = sizeof pixels;
    lv_image_cache_drop(&img);     /* mesma fonte, conteúdo novo */
    return &img;
}
