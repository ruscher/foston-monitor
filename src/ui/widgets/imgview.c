/* imgview.c — ver imgview.h. */
#include "imgview.h"
#include "../core/theme.h"
#include "../core/cover.h"
#include "../core/svgimg.h"
#include "../core/i18n.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define BAR_H     52
#define VIEW_W    800
#define VIEW_H    (480 - BAR_H)
#define SVG_MAX   2048
#define JPEG_MAX  (16 * 1024 * 1024)

static lv_obj_t *ov, *view, *img, *zoom_lbl, *busy;
static lv_image_dsc_t dsc;
static svgdoc_t *svg;
static uint8_t *jpeg;
static size_t jpeg_len;
static int nat_w, nat_h;       /* tamanho natural */
static int level;              /* 0 = ajustar; cada nível multiplica por 1,5 */
static bool is_svg;

static void free_image(void)
{
    if(is_svg) svg_image_free(&dsc);
    else jpeg_free(&dsc);
    memset(&dsc, 0, sizeof dsc);
}

/* escala de "ajustar" (cabe na área) multiplicada por 1,5^level */
static int target_w(void)
{
    long fit_w = nat_w, fit_h = nat_h;
    if(fit_w * VIEW_H > fit_h * VIEW_W) { fit_h = fit_h * VIEW_W / fit_w; fit_w = VIEW_W; }
    else { fit_w = fit_w * VIEW_H / fit_h; fit_h = VIEW_H; }
    long w = fit_w;
    for(int i = 0; i < level; i++) w = w * 3 / 2;
    return (int)w;
}

/* maior lado permitido: SVG até SVG_MAX; JPEG até a resolução original (e SVG_MAX) */
static int max_edge(void)
{
    int nat = nat_w > nat_h ? nat_w : nat_h;
    return is_svg || nat > SVG_MAX ? SVG_MAX : nat;
}

static void render(void)
{
    lv_obj_remove_flag(busy, LV_OBJ_FLAG_HIDDEN);
    lv_refr_now(NULL);                         /* mostra "renderizando" antes do trabalho */
    lv_image_set_src(img, NULL);
    free_image();
    int w = target_w();
    bool ok;
    uint32_t t0 = lv_tick_get();
    if(is_svg) ok = svg_render(svg, (float)w / (float)nat_w, lv_color_white(), SVG_MAX, &dsc);
    else {
        int h = (int)((long)w * nat_h / nat_w);
        int edge = max_edge();
        if(w > edge || h > edge) {              /* fotos grandes: no máximo 2048 px de lado */
            if(w >= h) { h = (int)((long)h * edge / w); w = edge; }
            else { w = (int)((long)w * edge / h); h = edge; }
        }
        ok = jpeg_fit(jpeg, jpeg_len, (unsigned)w, (unsigned)h, &dsc);
    }
    lv_obj_add_flag(busy, LV_OBJ_FLAG_HIDDEN);
    fprintf(stderr, "imagem: %s %dx%d em %u ms\n", is_svg ? "SVG" : "JPEG",
            (int)dsc.header.w, (int)dsc.header.h, lv_tick_elaps(t0));
    if(!ok) { lv_label_set_text(zoom_lbl, tr(S_FILES_BAD_IMAGE)); return; }
    lv_image_set_src(img, &dsc);
    lv_obj_update_layout(view);
    /* centraliza a rolagem na imagem */
    int sx = ((int)dsc.header.w - VIEW_W) / 2, sy = ((int)dsc.header.h - VIEW_H) / 2;
    lv_obj_scroll_to(view, sx > 0 ? sx : 0, sy > 0 ? sy : 0, LV_ANIM_OFF);
    char z[32];
    snprintf(z, sizeof z, "%d%%", (int)((long)dsc.header.w * 100 / nat_w));
    lv_label_set_text(zoom_lbl, z);
}

static void zoom_click(lv_event_t *e)
{
    int d = (int)(intptr_t)lv_event_get_user_data(e);
    if(d == 0) level = 0;
    else if(d < 0) { if(!level) return; level--; }
    else {
        int edge = dsc.header.w > dsc.header.h ? (int)dsc.header.w : (int)dsc.header.h;
        if(edge >= max_edge()) return;                 /* já no limite */
        level++;
    }
    render();
}

static void close_click(lv_event_t *e) { (void)e; imgview_close(); }

static lv_obj_t *tool(lv_obj_t *bar, const char *sym, const char *txt, lv_event_cb_t cb, intptr_t ud)
{
    lv_obj_t *b = bf_button(bar, sym, txt, 0);
    lv_obj_set_height(b, 44);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, (void *)ud);
    return b;
}

bool imgview_open(const char *path)
{
    imgview_close();
    const char *dot = strrchr(path, '.');
    is_svg = dot && !strcasecmp(dot, ".svg");
    if(is_svg) {
        svg = svg_open(path);
        if(!svg) return false;
        svg_size(svg, &nat_w, &nat_h);
    }
    else {
        FILE *f = fopen(path, "rb");
        if(!f) return false;
        fseek(f, 0, SEEK_END);
        long n = ftell(f);
        fseek(f, 0, SEEK_SET);
        if(n <= 0 || n > JPEG_MAX || !(jpeg = malloc((size_t)n))) { fclose(f); return false; }
        jpeg_len = fread(jpeg, 1, (size_t)n, f);
        fclose(f);
        unsigned w, h;
        if(!jpeg_size(jpeg, jpeg_len, &w, &h)) { free(jpeg); jpeg = NULL; return false; }
        nat_w = (int)w;
        nat_h = (int)h;
    }
    if(nat_w <= 0 || nat_h <= 0) { imgview_close(); return false; }
    level = 0;

    const bf_palette_t *p = pal();
    ov = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(ov);
    lv_obj_set_size(ov, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(ov, lv_color_hex(0x101010), 0);
    lv_obj_set_style_bg_opa(ov, LV_OPA_COVER, 0);
    lv_obj_add_flag(ov, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(ov, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *bar = lv_obj_create(ov);
    lv_obj_remove_style_all(bar);
    lv_obj_set_size(bar, LV_PCT(100), BAR_H);
    lv_obj_set_style_bg_color(bar, p->surface, 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_hor(bar, BF_SP_S, 0);
    lv_obj_set_style_pad_column(bar, BF_SP_S, 0);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    const char *base = strrchr(path, '/');
    lv_obj_t *t = bf_label(bar, base ? base + 1 : path, BF_FONT_M, p->text);
    bf_label_ellipsis(t, 330);
    lv_obj_set_flex_grow(t, 1);
    tool(bar, LV_SYMBOL_MINUS, NULL, zoom_click, -1);
    zoom_lbl = bf_label(bar, "", BF_FONT_S, p->text_dim);
    lv_obj_set_width(zoom_lbl, 56);
    lv_obj_set_style_text_align(zoom_lbl, LV_TEXT_ALIGN_CENTER, 0);
    tool(bar, LV_SYMBOL_PLUS, NULL, zoom_click, 1);
    tool(bar, LV_SYMBOL_REFRESH, tr(S_VIEW_FIT), zoom_click, 0);
    tool(bar, LV_SYMBOL_CLOSE, NULL, close_click, 0);

    view = lv_obj_create(ov);
    lv_obj_remove_style_all(view);
    lv_obj_set_size(view, VIEW_W, VIEW_H);
    lv_obj_set_pos(view, 0, BAR_H);
    lv_obj_set_flex_flow(view, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(view, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scrollbar_mode(view, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_add_flag(view, LV_OBJ_FLAG_SCROLLABLE);
    img = lv_image_create(view);

    busy = bf_label(ov, tr(S_VIEW_RENDERING), BF_FONT_M, lv_color_white());
    lv_obj_center(busy);
    render();
    return true;
}

void imgview_close(void)
{
    if(ov) { lv_obj_delete(ov); ov = NULL; }
    free_image();
    svg_close(svg);
    svg = NULL;
    free(jpeg);
    jpeg = NULL;
    view = img = zoom_lbl = busy = NULL;
}

bool imgview_is_open(void) { return ov != NULL; }
