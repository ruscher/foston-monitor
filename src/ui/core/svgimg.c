/* svgimg.c — SVG com nanosvg (análise + rasterizador anti-serrilhado).
 * O nanosvg usa float: no ARM sem VFP é emulado, então a rasterização é
 * feita só quando a escala muda, nunca por quadro. */
#include "svgimg.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NANOSVG_IMPLEMENTATION
#define NANOSVGRAST_IMPLEMENTATION
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wmisleading-indentation"
#pragma GCC diagnostic ignored "-Wimplicit-fallthrough"
#endif
#include "nanosvg.h"
#include "nanosvgrast.h"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

struct svgdoc { NSVGimage *img; };

svgdoc_t *svg_open(const char *path)
{
    NSVGimage *im = nsvgParseFromFile(path, "px", 96.0f);
    if(!im) return NULL;
    if(im->width <= 0 || im->height <= 0 || !im->shapes) { nsvgDelete(im); return NULL; }
    svgdoc_t *d = malloc(sizeof *d);
    if(!d) { nsvgDelete(im); return NULL; }
    d->img = im;
    return d;
}

void svg_close(svgdoc_t *d)
{
    if(!d) return;
    nsvgDelete(d->img);
    free(d);
}

void svg_size(const svgdoc_t *d, int *w, int *h)
{
    *w = (int)(d->img->width + 0.5f);
    *h = (int)(d->img->height + 0.5f);
}

void svg_image_free(lv_image_dsc_t *img)
{
    if(img->data) {
        lv_image_cache_drop(img);
        free((void *)img->data);
    }
    memset(img, 0, sizeof *img);
}

bool svg_render(svgdoc_t *d, float scale, lv_color_t bg, int max_px, lv_image_dsc_t *img)
{
    svg_image_free(img);
    int w = (int)(d->img->width * scale + 0.5f), h = (int)(d->img->height * scale + 0.5f);
    if(w > max_px || h > max_px) {             /* limita a memória (RGBA + RGB565) */
        float k = (float)max_px / (float)(w > h ? w : h);
        scale *= k;
        w = (int)(d->img->width * scale + 0.5f);
        h = (int)(d->img->height * scale + 0.5f);
    }
    if(w < 1) w = 1;
    if(h < 1) h = 1;
    unsigned char *rgba = malloc((size_t)w * h * 4);
    uint16_t *px = malloc((size_t)w * h * 2);
    NSVGrasterizer *r = nsvgCreateRasterizer();
    if(!rgba || !px || !r) { free(rgba); free(px); if(r) nsvgDeleteRasterizer(r); return false; }
    nsvgRasterize(r, d->img, 0, 0, scale, rgba, w, h, w * 4);
    nsvgDeleteRasterizer(r);
    /* compõe sobre o fundo (RGBA não pré-multiplicado) e converte para RGB565 */
    uint32_t br = bg.red, bgc = bg.green, bb = bg.blue;
    for(size_t i = 0; i < (size_t)w * h; i++) {
        const unsigned char *s = rgba + 4 * i;
        uint32_t a = s[3];
        uint32_t rr = (s[0] * a + br * (255 - a)) / 255;
        uint32_t gg = (s[1] * a + bgc * (255 - a)) / 255;
        uint32_t bl = (s[2] * a + bb * (255 - a)) / 255;
        px[i] = (uint16_t)(((rr >> 3) << 11) | ((gg >> 2) << 5) | (bl >> 3));
    }
    free(rgba);
    memset(img, 0, sizeof *img);
    img->header.magic = LV_IMAGE_HEADER_MAGIC;
    img->header.cf = LV_COLOR_FORMAT_RGB565;
    img->header.w = (uint32_t)w;
    img->header.h = (uint32_t)h;
    img->header.stride = (uint32_t)w * 2;
    img->data = (const uint8_t *)px;
    img->data_size = (uint32_t)(w * h * 2);
    return true;
}

bool svg_render_mask(const char *path, int box, lv_image_dsc_t *img)
{
    svg_image_free(img);
    svgdoc_t *d = svg_open(path);
    if(!d) return false;
    float iw = d->img->width, ih = d->img->height;
    float scale = (float)box * 0.86f / (iw > ih ? iw : ih);
    unsigned char *rgba = malloc((size_t)box * box * 4);
    uint8_t *a8 = malloc((size_t)box * box);
    NSVGrasterizer *r = nsvgCreateRasterizer();
    if(!rgba || !a8 || !r) {
        free(rgba); free(a8);
        if(r) nsvgDeleteRasterizer(r);
        svg_close(d);
        return false;
    }
    memset(rgba, 0, (size_t)box * box * 4);
    float tx = ((float)box - iw * scale) / 2, ty = ((float)box - ih * scale) / 2;
    nsvgRasterize(r, d->img, tx, ty, scale, rgba, box, box, box * 4);
    nsvgDeleteRasterizer(r);
    svg_close(d);
    for(size_t i = 0; i < (size_t)box * box; i++) a8[i] = rgba[4 * i + 3];
    free(rgba);
    memset(img, 0, sizeof *img);
    img->header.magic = LV_IMAGE_HEADER_MAGIC;
    img->header.cf = LV_COLOR_FORMAT_A8;
    img->header.w = (uint32_t)box;
    img->header.h = (uint32_t)box;
    img->header.stride = (uint32_t)box;
    img->data = a8;
    img->data_size = (uint32_t)(box * box);
    return true;
}
