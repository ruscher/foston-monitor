/* wallpaper.c — ver wallpaper.h. Só aritmética inteira (o ARM1176 do
 * aparelho não tem FPU em uso pelo musl estático). */
#include "wallpaper.h"
#include "../assets/logo/bf_logo.h"
#include "imgload.h"
#include "settings.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define WM_X 430      /* canto superior esquerdo da marca d'água (sai pela direita) */
#define WM_Y 40

static uint16_t *buf;

static const uint8_t bayer[4][4] = {
    { 0, 8, 2, 10 }, { 12, 4, 14, 6 }, { 3, 11, 1, 9 }, { 15, 7, 13, 5 } };

/* canal em 8.8 -> n bits, com o limiar do pontilhado (0..15) */
static inline unsigned q(unsigned c88, unsigned max, unsigned thr)
{
    unsigned v = (c88 * max / 255 + thr * 16) >> 8;
    return v > max ? max : v;
}

static inline int lerp(int a, int b, int t)   /* t 0..256, resultado em 8.8 */
{
    return a * 256 + (b - a) * t;
}

bool wallpaper_build(lv_image_dsc_t *dsc, const bf_palette_t *p, lv_color_t glow, bool logo)
{
    if(!buf && !(buf = malloc((size_t)BF_WALL_W * BF_WALL_H * 2))) return false;

    /* luz vinda de cima, ao centro; no escuro a cor do tema, no claro quase branco */
    lv_color_t edge = p->bg;
    lv_color_t center = p->dark ? lv_color_mix(glow, p->bg, LV_OPA_60)
                                : lv_color_mix(lv_color_white(), p->bg, LV_OPA_70);
    lv_color_t wm = p->dark ? p->accent : p->primary;
    int wm_opa = p->dark ? 26 : 20;          /* de 255 */
    const int cx = BF_WALL_W / 2, cy = -40;
    /* t = d² * K >> 24, chega a 256 a 640 px do centro (d² máx. ~3,2e5: cabe em 32 bits) */
    const uint32_t K = 0xffffffffu / (640u * 640u);

    const uint8_t *wm_px = bf_logo_wm.data;
    int wm_w = (int)bf_logo_wm.header.w, wm_h = (int)bf_logo_wm.header.h;

    for(int y = 0; y < BF_WALL_H; y++) {
        int dy = y - cy;
        uint16_t *row = buf + (size_t)y * BF_WALL_W;
        for(int x = 0; x < BF_WALL_W; x++) {
            int dx = x - cx;
            uint32_t d2 = (uint32_t)(dx * dx + dy * dy);
            int t = (int)((d2 * K) >> 24);
            if(t > 256) t = 256;
            int r = lerp(center.red, edge.red, t);
            int g = lerp(center.green, edge.green, t);
            int b = lerp(center.blue, edge.blue, t);
            if(logo) {
                int wx = x - WM_X, wy = y - WM_Y;
                if(wx >= 0 && wy >= 0 && wx < wm_w && wy < wm_h) {
                    int a = wm_px[wy * wm_w + wx] * wm_opa / 255;   /* 0..255 */
                    if(a) {
                        r += (wm.red * 256 - r) * a / 255;
                        g += (wm.green * 256 - g) * a / 255;
                        b += (wm.blue * 256 - b) * a / 255;
                    }
                }
            }
            unsigned thr = bayer[y & 3][x & 3];
            row[x] = (uint16_t)(q((unsigned)r, 31, thr) << 11 | q((unsigned)g, 63, thr) << 5 |
                                q((unsigned)b, 31, thr));
        }
    }
    lv_memzero(dsc, sizeof *dsc);
    dsc->header.magic = LV_IMAGE_HEADER_MAGIC;
    dsc->header.cf = LV_COLOR_FORMAT_RGB565;
    dsc->header.w = BF_WALL_W;
    dsc->header.h = BF_WALL_H;
    dsc->header.stride = BF_WALL_W * 2;
    dsc->data = (const uint8_t *)buf;
    dsc->data_size = (uint32_t)BF_WALL_W * BF_WALL_H * 2;
    /* mesmo endereço com conteúdo novo: o cache de imagens do LVGL precisa esquecer */
    lv_image_cache_drop(dsc);
    return true;
}

/* ------------------------------------------------ imagem do usuário */

#define CACHE_MAGIC "BFWP2"   /* muda junto com o tratamento da imagem */

typedef struct {
    char magic[8];
    char path[256];
    long long mtime, size;
    int dark;
} cache_hdr_t;

static void fill_dsc(lv_image_dsc_t *dsc)
{
    lv_memzero(dsc, sizeof *dsc);
    dsc->header.magic = LV_IMAGE_HEADER_MAGIC;
    dsc->header.cf = LV_COLOR_FORMAT_RGB565;
    dsc->header.w = BF_WALL_W;
    dsc->header.h = BF_WALL_H;
    dsc->header.stride = BF_WALL_W * 2;
    dsc->data = (const uint8_t *)buf;
    dsc->data_size = (uint32_t)BF_WALL_W * BF_WALL_H * 2;
    lv_image_cache_drop(dsc);
}

static void cache_key(cache_hdr_t *h, const char *path, const struct stat *st, bool dark)
{
    memset(h, 0, sizeof *h);
    memcpy(h->magic, CACHE_MAGIC, sizeof CACHE_MAGIC);
    snprintf(h->path, sizeof h->path, "%s", path);
    h->mtime = (long long)st->st_mtime;
    h->size = (long long)st->st_size;
    h->dark = dark;
}

bool wallpaper_from_image(lv_image_dsc_t *dsc, const char *path, const bf_palette_t *p)
{
    struct stat st;
    if(stat(path, &st) || !S_ISREG(st.st_mode)) return false;
    if(!buf && !(buf = malloc((size_t)BF_WALL_W * BF_WALL_H * 2))) return false;
    const size_t bytes = (size_t)BF_WALL_W * BF_WALL_H * 2;
    char cpath[300];
    data_path(cpath, sizeof cpath, "wallpaper.cache");
    cache_hdr_t want, got;
    cache_key(&want, path, &st, p->dark);

    FILE *f = fopen(cpath, "rb");
    if(f) {
        bool ok = fread(&got, sizeof got, 1, f) == 1 && !memcmp(&got, &want, sizeof want) &&
                  fread(buf, 1, bytes, f) == bytes;
        fclose(f);
        if(ok) { fill_dsc(dsc); return true; }
    }

    /* decodifica já reduzida para perto do tamanho que preenche 800x360 */
    int w, h;
    if(!img_probe(path, &w, &h) || w <= 0 || h <= 0) return false;
    int tw, th;
    if((long long)w * BF_WALL_H >= (long long)h * BF_WALL_W) { th = BF_WALL_H; tw = (int)(((long long)w * BF_WALL_H + h - 1) / h); }
    else { tw = BF_WALL_W; th = (int)(((long long)h * BF_WALL_W + w - 1) / w); }
    lv_image_dsc_t img;
    if(!img_load_fit(path, tw, th, p->bg, &img, NULL, 0)) return false;
    int iw = (int)img.header.w, ih = (int)img.header.h, is = (int)img.header.stride / 2;
    /* recorte central com a proporção da tela (amplia por vizinho se a imagem for pequena) */
    int cw, ch;
    if((long long)iw * BF_WALL_H >= (long long)ih * BF_WALL_W) { ch = ih; cw = (int)((long long)ih * BF_WALL_W / BF_WALL_H); }
    else { cw = iw; ch = (int)((long long)iw * BF_WALL_H / BF_WALL_W); }
    int ox = (iw - cw) / 2, oy = (ih - ch) / 2;
    const uint16_t *src = (const uint16_t *)img.data;
    /* no escuro, 80 % do brilho; no claro, só 30 % da imagem sobre a cor de fundo (com 70 %,
     * uma imagem escura deixava o texto escuro do tema claro ilegível) */
    unsigned mul = p->dark ? 205 : 77, br = p->bg.red, bgc = p->bg.green, bb = p->bg.blue;
    for(int y = 0; y < BF_WALL_H; y++) {
        const uint16_t *srow = src + (size_t)(oy + y * ch / BF_WALL_H) * is + ox;
        uint16_t *row = buf + (size_t)y * BF_WALL_W;
        for(int x = 0; x < BF_WALL_W; x++) {
            uint16_t c = srow[x * cw / BF_WALL_W];
            unsigned r = (c >> 11) << 3, g = ((c >> 5) & 63) << 2, b = (c & 31) << 3;
            if(p->dark) { r = r * mul >> 8; g = g * mul >> 8; b = b * mul >> 8; }
            else { r = (r * mul + br * (256 - mul)) >> 8; g = (g * mul + bgc * (256 - mul)) >> 8; b = (b * mul + bb * (256 - mul)) >> 8; }
            unsigned thr = bayer[y & 3][x & 3];
            row[x] = (uint16_t)(q(r << 8, 31, thr) << 11 | q(g << 8, 63, thr) << 5 | q(b << 8, 31, thr));
        }
    }
    img_free(&img);

    char tmp[310];
    snprintf(tmp, sizeof tmp, "%s.tmp", cpath);
    if((f = fopen(tmp, "wb"))) {
        bool ok = fwrite(&want, sizeof want, 1, f) == 1 && fwrite(buf, 1, bytes, f) == bytes;
        ok = fclose(f) == 0 && ok;
        if(ok) rename(tmp, cpath);
        else unlink(tmp);
    }
    fill_dsc(dsc);
    return true;
}
