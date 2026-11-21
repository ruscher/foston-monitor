/* imgload.c — ver imgload.h. */
#include "imgload.h"
#include "cover.h"
#include "svgimg.h"
#include "lodepng.h"
#include "../third_party/lvgl/src/libs/gif/gifdec.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define FILE_MAX   (32u * 1024 * 1024)
#define PNG_MAXPX  (6u * 1000 * 1000)
#define GIF_MAXDIM 1200

static void seterr(char *err, size_t n, const char *msg)
{
    if(err && n) snprintf(err, n, "%s", msg);
}

img_kind_t img_kind_of(const char *path)
{
    const char *d = strrchr(path, '.');
    if(!d) return IMG_UNKNOWN;
    d++;
    if(!strcasecmp(d, "jpg") || !strcasecmp(d, "jpeg") || !strcasecmp(d, "jpe") || !strcasecmp(d, "jfif"))
        return IMG_JPEG;
    if(!strcasecmp(d, "png")) return IMG_PNG;
    if(!strcasecmp(d, "bmp") || !strcasecmp(d, "dib")) return IMG_BMP;
    if(!strcasecmp(d, "gif")) return IMG_GIF;
    if(!strcasecmp(d, "svg")) return IMG_SVG;
    return IMG_UNKNOWN;
}

bool img_is_image(const char *path) { return img_kind_of(path) != IMG_UNKNOWN; }

static uint8_t *read_all(const char *path, size_t max, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if(!f) return NULL;
    uint8_t *buf = NULL;
    if(fseek(f, 0, SEEK_END) == 0) {
        long n = ftell(f);
        if(n > 0 && (size_t)n <= FILE_MAX) {
            size_t want = (size_t)n < max ? (size_t)n : max;
            fseek(f, 0, SEEK_SET);
            buf = malloc(want ? want : 1);
            if(buf && fread(buf, 1, want, f) != want) { free(buf); buf = NULL; }
            else if(buf) *len = want;
        }
    }
    fclose(f);
    return buf;
}

static uint32_t le32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }

/* caixa de destino: cabe em max mantendo a proporção, nunca amplia */
static void fit(int w, int h, int max_w, int max_h, int *dw, int *dh)
{
    if((long)w * max_h > (long)h * max_w) { *dw = w < max_w ? w : max_w; *dh = (int)((long)h * *dw / w); }
    else { *dh = h < max_h ? h : max_h; *dw = (int)((long)w * *dh / h); }
    if(*dw < 1) *dw = 1;
    if(*dh < 1) *dh = 1;
}

static bool alloc565(lv_image_dsc_t *img, int w, int h)
{
    memset(img, 0, sizeof *img);
    uint16_t *px = malloc((size_t)w * h * 2);
    if(!px) return false;
    img->header.magic = LV_IMAGE_HEADER_MAGIC;
    img->header.cf = LV_COLOR_FORMAT_RGB565;
    img->header.w = (uint32_t)w;
    img->header.h = (uint32_t)h;
    img->header.stride = (uint32_t)w * 2;
    img->data = (const uint8_t *)px;
    img->data_size = (uint32_t)w * h * 2;
    return true;
}

/* 32 bits por pixel (ordem ri/gi/bi/ai) -> RGB565 dw x dh; reduções de 2x ou
 * mais fazem a média de 4 amostras; alfa misturado com bg */
static void resample32(const uint8_t *src, int sw, int sh, int ri, int gi, int bi, int ai,
                       lv_color_t bg, uint16_t *dst, int dw, int dh)
{
    bool avg = sw >= 2 * dw && sh >= 2 * dh;
    for(int y = 0; y < dh; y++) {
        for(int x = 0; x < dw; x++) {
            int r = 0, g = 0, b = 0;
            int ns = avg ? 4 : 1;
            for(int k = 0; k < ns; k++) {
                long fx = avg ? ((long)x * 4 + 1 + (k & 1) * 2) * sw / (4L * dw) : (long)x * sw / dw;
                long fy = avg ? ((long)y * 4 + 1 + (k >> 1) * 2) * sh / (4L * dh) : (long)y * sh / dh;
                const uint8_t *p = src + ((size_t)fy * sw + (size_t)fx) * 4;
                int a = ai >= 0 ? p[ai] : 255;
                r += (p[ri] * a + bg.red * (255 - a)) / 255;
                g += (p[gi] * a + bg.green * (255 - a)) / 255;
                b += (p[bi] * a + bg.blue * (255 - a)) / 255;
            }
            r /= ns; g /= ns; b /= ns;
            dst[(size_t)y * dw + x] = (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
        }
    }
}

/* ---------------------------------------------------------------- PNG */

static bool load_png(const uint8_t *d, size_t n, int max_w, int max_h, lv_color_t bg, lv_image_dsc_t *out,
                     char *err, size_t errn)
{
    if(n < 24 || be32(d + 16) == 0 || be32(d + 20) == 0) { seterr(err, errn, "PNG inválido"); return false; }
    if((uint64_t)be32(d + 16) * be32(d + 20) > PNG_MAXPX) { seterr(err, errn, "PNG grande demais (máx. 6 Mpx)"); return false; }
    unsigned char *rgba = NULL;
    unsigned w, h;
    if(lodepng_decode32(&rgba, &w, &h, d, n)) { seterr(err, errn, "PNG inválido"); return false; }
    int dw, dh;
    fit((int)w, (int)h, max_w, max_h, &dw, &dh);
    bool ok = alloc565(out, dw, dh);
    if(ok) resample32(rgba, (int)w, (int)h, 0, 1, 2, 3, bg, (uint16_t *)out->data, dw, dh);
    free(rgba);
    return ok;
}

/* ---------------------------------------------------------------- BMP */

static bool load_bmp(const uint8_t *d, size_t n, int max_w, int max_h, lv_color_t bg, lv_image_dsc_t *out,
                     char *err, size_t errn)
{
    (void)bg;
    if(n < 54 || d[0] != 'B' || d[1] != 'M') { seterr(err, errn, "BMP inválido"); return false; }
    uint32_t off = le32(d + 10), dib = le32(d + 14);
    int32_t w = (int32_t)le32(d + 18), h = (int32_t)le32(d + 22);
    unsigned bpp = le16(d + 28), comp = le32(d + 30), ncol = dib >= 40 ? le32(d + 46) : 0;
    bool topdown = h < 0;
    if(h < 0) h = -h;
    if(w <= 0 || h <= 0 || w > 16384 || h > 16384 ||
       !(comp == 0 || (comp == 3 && (bpp == 32 || bpp == 16)))) {
        seterr(err, errn, "BMP comprimido ou formato não suportado");
        return false;
    }
    if(bpp != 1 && bpp != 4 && bpp != 8 && bpp != 16 && bpp != 24 && bpp != 32) {
        seterr(err, errn, "BMP com profundidade não suportada");
        return false;
    }
    size_t stride = (((size_t)w * bpp + 31) / 32) * 4;
    if(off + stride * (size_t)h > n) { seterr(err, errn, "BMP truncado"); return false; }
    const uint8_t *pal = d + 14 + dib;
    if(!ncol && bpp <= 8) ncol = 1u << bpp;
    int dw, dh;
    fit(w, h, max_w, max_h, &dw, &dh);
    if(!alloc565(out, dw, dh)) return false;
    uint16_t *px = (uint16_t *)out->data;
    for(int y = 0; y < dh; y++) {
        int sy = (int)((long)y * h / dh);
        const uint8_t *row = d + off + stride * (size_t)(topdown ? sy : h - 1 - sy);
        for(int x = 0; x < dw; x++) {
            int sx = (int)((long)x * w / dw);
            uint8_t r, g, b;
            if(bpp >= 24) { const uint8_t *p = row + (size_t)sx * (bpp / 8); b = p[0]; g = p[1]; r = p[2]; }
            else if(bpp == 16) {        /* 5-5-5 (ou 5-6-5 com BI_BITFIELDS) */
                uint16_t v = le16(row + (size_t)sx * 2);
                bool m565 = comp == 3 && le32(d + 54) == 0xF800;
                r = (uint8_t)((m565 ? v >> 11 : (v >> 10) & 31) << 3);
                g = (uint8_t)(m565 ? ((v >> 5) & 63) << 2 : ((v >> 5) & 31) << 3);
                b = (uint8_t)((v & 31) << 3);
            }
            else {
                unsigned idx;
                if(bpp == 8) idx = row[sx];
                else if(bpp == 4) idx = (row[sx / 2] >> ((sx & 1) ? 0 : 4)) & 15;
                else idx = (row[sx / 8] >> (7 - (sx & 7))) & 1;
                if(idx >= ncol || pal + idx * 4 + 3 > d + n) idx = 0;
                b = pal[idx * 4]; g = pal[idx * 4 + 1]; r = pal[idx * 4 + 2];
            }
            px[(size_t)y * dw + x] = (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
        }
    }
    return true;
}

/* ---------------------------------------------------------------- GIF */

static bool load_gif(const uint8_t *d, size_t n, int max_w, int max_h, lv_color_t bg, lv_image_dsc_t *out,
                     char *err, size_t errn)
{
    if(n < 13) { seterr(err, errn, "GIF inválido"); return false; }
    int w = le16(d + 6), h = le16(d + 8);
    if(w <= 0 || h <= 0 || w > GIF_MAXDIM || h > GIF_MAXDIM) { seterr(err, errn, "GIF grande demais (máx. 1200 px)"); return false; }
    gd_GIF *gif = gd_open_gif_data(d);
    if(!gif) { seterr(err, errn, "GIF inválido"); return false; }
    uint8_t *buf = malloc((size_t)gif->width * gif->height * 4);
    bool ok = false;
    if(buf && gd_get_frame(gif) > 0) {
        gd_render_frame(gif, buf);                    /* BGRA */
        int dw, dh;
        fit(gif->width, gif->height, max_w, max_h, &dw, &dh);
        ok = alloc565(out, dw, dh);
        if(ok) resample32(buf, gif->width, gif->height, 2, 1, 0, 3, bg, (uint16_t *)out->data, dw, dh);
    }
    if(!ok) seterr(err, errn, "GIF inválido");
    free(buf);
    gd_close_gif(gif);
    return ok;
}

/* ---------------------------------------------------------------- geral */

bool img_probe(const char *path, int *w, int *h)
{
    img_kind_t k = img_kind_of(path);
    if(k == IMG_SVG) {
        svgdoc_t *s = svg_open(path);
        if(!s) return false;
        svg_size(s, w, h);
        svg_close(s);
        return *w > 0 && *h > 0;
    }
    size_t n = 0;
    uint8_t *d = read_all(path, k == IMG_JPEG ? 256 * 1024 : 64, &n);
    if(!d) return false;
    bool ok = false;
    unsigned uw, uh;
    if(k == IMG_JPEG && jpeg_size(d, n, &uw, &uh)) { *w = (int)uw; *h = (int)uh; ok = true; }
    else if(k == IMG_PNG && n >= 24 && !memcmp(d, "\x89PNG", 4)) { *w = (int)be32(d + 16); *h = (int)be32(d + 20); ok = true; }
    else if(k == IMG_BMP && n >= 26 && d[0] == 'B' && d[1] == 'M') {
        int32_t hh = (int32_t)le32(d + 22);
        *w = (int)le32(d + 18);
        *h = hh < 0 ? -hh : hh;
        ok = true;
    }
    else if(k == IMG_GIF && n >= 10 && !memcmp(d, "GIF", 3)) { *w = le16(d + 6); *h = le16(d + 8); ok = true; }
    free(d);
    return ok && *w > 0 && *h > 0;
}

bool img_load_fit(const char *path, int max_w, int max_h, lv_color_t bg, lv_image_dsc_t *out, char *err, size_t errn)
{
    memset(out, 0, sizeof *out);
    seterr(err, errn, "");
    img_kind_t k = img_kind_of(path);
    if(max_w < 1 || max_h < 1) return false;
    if(k == IMG_SVG) {
        svgdoc_t *s = svg_open(path);
        if(!s) { seterr(err, errn, "SVG inválido"); return false; }
        int w, h;
        svg_size(s, &w, &h);
        bool ok = false;
        if(w > 0 && h > 0) {
            float sx = (float)max_w / (float)w, sy = (float)max_h / (float)h;
            ok = svg_render(s, sx < sy ? sx : sy, bg, max_w > max_h ? max_w : max_h, out);
        }
        svg_close(s);
        if(!ok) seterr(err, errn, "SVG inválido");
        return ok;
    }
    size_t n = 0;
    uint8_t *d = read_all(path, FILE_MAX, &n);
    if(!d) { seterr(err, errn, "não foi possível ler o arquivo"); return false; }
    bool ok = false;
    /* pelo conteúdo (a extensão pode mentir) */
    if(n >= 3 && d[0] == 0xFF && d[1] == 0xD8) {
        ok = jpeg_fit(d, n, (unsigned)max_w, (unsigned)max_h, out);
        if(!ok) seterr(err, errn, "JPEG progressivo ou inválido");
    }
    else if(n >= 8 && !memcmp(d, "\x89PNG\r\n\x1a\n", 8)) ok = load_png(d, n, max_w, max_h, bg, out, err, errn);
    else if(n >= 2 && d[0] == 'B' && d[1] == 'M') ok = load_bmp(d, n, max_w, max_h, bg, out, err, errn);
    else if(n >= 6 && !memcmp(d, "GIF8", 4)) ok = load_gif(d, n, max_w, max_h, bg, out, err, errn);
    else seterr(err, errn, "formato não suportado");
    free(d);
    return ok;
}

void img_free(lv_image_dsc_t *img)
{
    if(img->data) {
        lv_image_cache_drop(img);
        free((void *)img->data);
    }
    memset(img, 0, sizeof *img);
}
