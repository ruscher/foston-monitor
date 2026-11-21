/* imgview.c — ver imgview.h. */
#include "imgview.h"
#include "../core/theme.h"
#include "../core/imgload.h"
#include "../core/i18n.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define BAR_H     52
#define VIEW_W    800
#define VIEW_H    (480 - BAR_H)
#define SVG_MAX   2048
#define GIF_ANIM_MAX  (2u * 1024 * 1024)   /* bytes do arquivo para animar */
#define GIF_DIM_MAX   1200

static lv_obj_t *ov, *view, *img, *gif, *zoom_lbl, *busy, *z_minus, *z_plus, *z_fit;
static lv_image_dsc_t dsc, gif_dsc;
static uint8_t *gif_data;
static char cur_path[1024];
static int nat_w, nat_h;       /* tamanho natural */
static int level;              /* 0 = ajustar; cada nível multiplica por 1,5 */
static img_kind_t kind;
static imgview_nav_cb_t nav_cb;
static void *nav_ud;

static void free_image(void)
{
    img_free(&dsc);
}

/* escala de "ajustar" (cabe na área) multiplicada por 1,5^level */
static void target(int *tw, int *th)
{
    long fit_w = nat_w, fit_h = nat_h;
    if(fit_w * VIEW_H > fit_h * VIEW_W) { fit_h = fit_h * VIEW_W / fit_w; fit_w = VIEW_W; }
    else { fit_w = fit_w * VIEW_H / fit_h; fit_h = VIEW_H; }
    for(int i = 0; i < level; i++) { fit_w = fit_w * 3 / 2; fit_h = fit_h * 3 / 2; }
    *tw = (int)(fit_w > 0 ? fit_w : 1);
    *th = (int)(fit_h > 0 ? fit_h : 1);
}

/* maior lado permitido: SVG até SVG_MAX; mapas de bits até a resolução original (e SVG_MAX) */
static int max_edge(void)
{
    int nat = nat_w > nat_h ? nat_w : nat_h;
    return kind == IMG_SVG || nat > SVG_MAX ? SVG_MAX : nat;
}

static void render(void)
{
    lv_obj_remove_flag(busy, LV_OBJ_FLAG_HIDDEN);
    lv_refr_now(NULL);                         /* mostra "renderizando" antes do trabalho */
    lv_obj_add_flag(img, LV_OBJ_FLAG_HIDDEN);  /* não desenha o buffer que vai ser liberado */
    free_image();
    int w, h;
    target(&w, &h);
    int edge = max_edge();
    if(w > edge || h > edge) {                 /* fotos grandes: no máximo 2048 px de lado */
        if(w >= h) { h = (int)((long)h * edge / w); w = edge; }
        else { w = (int)((long)w * edge / h); h = edge; }
    }
    char err[96];
    uint32_t t0 = lv_tick_get();
    bool ok = img_load_fit(cur_path, w, h, lv_color_hex(0x101010), &dsc, err, sizeof err);
    lv_obj_add_flag(busy, LV_OBJ_FLAG_HIDDEN);
    fprintf(stderr, "imagem: %s %dx%d em %u ms%s%s\n", cur_path, (int)dsc.header.w, (int)dsc.header.h,
            lv_tick_elaps(t0), ok ? "" : ": ", ok ? "" : err);
    if(!ok) {
        lv_label_set_text(zoom_lbl, "");
        lv_label_set_text(busy, tr(S_FILES_BAD_IMAGE));
        lv_obj_remove_flag(busy, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_image_set_src(img, &dsc);
    lv_obj_remove_flag(img, LV_OBJ_FLAG_HIDDEN);
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

static void nav_click(lv_event_t *e)
{
    int dir = (int)(intptr_t)lv_event_get_user_data(e);
    if(nav_cb) nav_cb(dir, nav_ud);
}

/* deslizar para os lados troca de imagem (só sem zoom: com zoom o dedo arrasta) */
static void gesture(lv_event_t *e)
{
    (void)e;
    if(!nav_cb || level) return;
    lv_dir_t d = lv_indev_get_gesture_dir(lv_indev_active());
    if(d == LV_DIR_LEFT) nav_cb(1, nav_ud);
    else if(d == LV_DIR_RIGHT) nav_cb(-1, nav_ud);
}

static lv_obj_t *tool(lv_obj_t *bar, const char *sym, const char *txt, lv_event_cb_t cb, intptr_t ud)
{
    lv_obj_t *b = bf_button(bar, sym, txt, 0);
    lv_obj_set_height(b, 44);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, (void *)ud);
    return b;
}

void imgview_set_nav(imgview_nav_cb_t cb, void *ud)
{
    nav_cb = cb;
    nav_ud = ud;
}

/* GIF animado: o arquivo inteiro na memória para o lv_gif */
static bool open_gif_anim(void)
{
    if(kind != IMG_GIF || nat_w > GIF_DIM_MAX || nat_h > GIF_DIM_MAX) return false;
    FILE *f = fopen(cur_path, "rb");
    if(!f) return false;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if(n <= 0 || (unsigned long)n > GIF_ANIM_MAX || !(gif_data = malloc((size_t)n))) { fclose(f); return false; }
    bool ok = fread(gif_data, 1, (size_t)n, f) == (size_t)n;
    fclose(f);
    if(!ok) { free(gif_data); gif_data = NULL; return false; }
    memset(&gif_dsc, 0, sizeof gif_dsc);
    gif_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    gif_dsc.header.cf = LV_COLOR_FORMAT_RAW;
    gif_dsc.data = gif_data;
    gif_dsc.data_size = (uint32_t)n;
    gif = lv_gif_create(view);
    lv_gif_set_src(gif, &gif_dsc);
    return true;
}

static void close_internal(void)
{
    if(ov) { lv_obj_delete(ov); ov = NULL; }     /* apaga o lv_gif antes dos dados dele */
    free_image();
    free(gif_data);
    gif_data = NULL;
    view = img = gif = zoom_lbl = busy = z_minus = z_plus = z_fit = NULL;
}

bool imgview_open(const char *path)
{
    close_internal();
    kind = img_kind_of(path);
    snprintf(cur_path, sizeof cur_path, "%s", path);
    if(kind == IMG_UNKNOWN || !img_probe(path, &nat_w, &nat_h) || nat_w <= 0 || nat_h <= 0) return false;
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
    if(nav_cb) tool(bar, LV_SYMBOL_LEFT, NULL, nav_click, -1);
    const char *base = strrchr(path, '/');
    lv_obj_t *t = bf_label(bar, base ? base + 1 : path, BF_FONT_M, p->text);
    bf_label_ellipsis(t, nav_cb ? 230 : 330);
    lv_obj_set_flex_grow(t, 1);
    if(nav_cb) tool(bar, LV_SYMBOL_RIGHT, NULL, nav_click, 1);
    z_minus = tool(bar, LV_SYMBOL_MINUS, NULL, zoom_click, -1);
    zoom_lbl = bf_label(bar, "", BF_FONT_S, p->text_dim);
    lv_obj_set_width(zoom_lbl, 56);
    lv_obj_set_style_text_align(zoom_lbl, LV_TEXT_ALIGN_CENTER, 0);
    z_plus = tool(bar, LV_SYMBOL_PLUS, NULL, zoom_click, 1);
    z_fit = tool(bar, LV_SYMBOL_REFRESH, tr(S_VIEW_FIT), zoom_click, 0);
    tool(bar, LV_SYMBOL_CLOSE, NULL, close_click, 0);

    view = lv_obj_create(ov);
    lv_obj_remove_style_all(view);
    lv_obj_set_size(view, VIEW_W, VIEW_H);
    lv_obj_set_pos(view, 0, BAR_H);
    lv_obj_set_flex_flow(view, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(view, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scrollbar_mode(view, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_add_flag(view, LV_OBJ_FLAG_SCROLLABLE);           /* nas duas direções (zoom) */
    lv_obj_add_style(view, &bf_st_scrollbar, LV_PART_SCROLLBAR);
    lv_obj_add_event_cb(view, gesture, LV_EVENT_GESTURE, NULL);
    img = lv_image_create(view);

    busy = bf_label(ov, tr(S_VIEW_RENDERING), BF_FONT_M, lv_color_white());
    lv_obj_center(busy);

    if(open_gif_anim()) {                       /* animado, no tamanho natural */
        lv_obj_add_flag(img, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(busy, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(z_minus, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(z_plus, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(z_fit, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(zoom_lbl, "GIF");
        return true;
    }
    render();
    return true;
}

void imgview_close(void)
{
    close_internal();
    nav_cb = NULL;
    nav_ud = NULL;
}

bool imgview_is_open(void) { return ov != NULL; }
