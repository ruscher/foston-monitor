/* wallpaper.c — ver wallpaper.h. Só aritmética inteira (o ARM1176 do
 * aparelho não tem FPU em uso pelo musl estático). */
#include "wallpaper.h"
#include "../assets/logo/bf_logo.h"
#include <stdint.h>
#include <stdlib.h>

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
