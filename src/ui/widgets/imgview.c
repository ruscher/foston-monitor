/* imgview.c — ver imgview.h. */
#include "imgview.h"
#include "../core/theme.h"
#include "../core/imgload.h"
#include "../core/i18n.h"
#include "../core/nav.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#define BAR_H         56
#define VIEW_W        BF_SCREEN_W
#define SVG_MAX       2048
#define GIF_ANIM_MAX  (2u * 1024 * 1024)   /* bytes do arquivo para animar */
#define GIF_DIM_MAX   1200
#define BG_HEX        0x101010
#define C_ACCENT      0xd6409f             /* cor da Galeria */

static lv_obj_t *ov, *view, *img, *gif, *busy, *top_bar, *bot_bar;
static lv_obj_t *zoom_lbl, *z_minus, *z_plus, *z_fit, *r_left, *r_right, *slider, *pos_lbl;
static lv_image_dsc_t dsc, gif_dsc;
static uint8_t *gif_data;
static char cur_path[1024];
static int view_h;             /* altura da área de conteúdo (360, ou 432 no Glass) */
static int nat_w, nat_h;       /* tamanho natural (sem giro) */
static int level;              /* 0 = ajustar; cada nível multiplica por 1,5 */
static int rot;                /* 0..3: quartos de volta no sentido horário */
static int level_cap;          /* maior nível que decodificou (falta de memória limita o +) */
static bool loaded;            /* imagem estática na tela (zoom/giro possíveis) */
static bool bars_on;           /* controles visíveis: valem ao trocar de imagem */
static img_kind_t kind;
static imgview_nav_cb_t nav_cb;
static void *nav_ud;
static int nav_idx, nav_count;

/* tamanho natural já girado */
static int disp_w(void) { return rot & 1 ? nat_h : nat_w; }
static int disp_h(void) { return rot & 1 ? nat_w : nat_h; }

/* escala de "ajustar" (cabe na área) multiplicada por 1,5^level */
static void target(int *tw, int *th)
{
    long fit_w = disp_w(), fit_h = disp_h();
    if(fit_w * view_h > fit_h * VIEW_W) { fit_h = fit_h * VIEW_W / fit_w; fit_w = VIEW_W; }
    else { fit_w = fit_w * view_h / fit_h; fit_h = view_h; }
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

/* gira os pixels RGB565 em r quartos de volta no sentido horário */
static bool rotate_pixels(lv_image_dsc_t *d, int r)
{
    int w = (int)d->header.w, h = (int)d->header.h;
    uint16_t *s = (uint16_t *)d->data;
    size_t n = (size_t)w * h;
    if(r == 2) {                                   /* meia volta: no lugar */
        for(size_t i = 0, j = n - 1; i < j; i++, j--) { uint16_t t = s[i]; s[i] = s[j]; s[j] = t; }
        return true;
    }
    uint16_t *o = malloc(n * 2);
    if(!o) return false;
    /* destino h x w; percorre o destino em ordem (escrita sequencial) */
    for(int y = 0; y < w; y++) {
        uint16_t *row = o + (size_t)y * h;
        if(r == 1) for(int x = 0; x < h; x++) row[x] = s[(size_t)(h - 1 - x) * w + y];
        else       for(int x = 0; x < h; x++) row[x] = s[(size_t)x * w + (w - 1 - y)];
    }
    free(s);
    d->data = (const uint8_t *)o;
    d->header.w = (uint32_t)h;
    d->header.h = (uint32_t)w;
    d->header.stride = (uint32_t)h * 2;
    return true;
}

static void set_enabled(lv_obj_t *o, bool on)
{
    if(!o) return;
    if(on) lv_obj_remove_state(o, LV_STATE_DISABLED);
    else lv_obj_add_state(o, LV_STATE_DISABLED);
    lv_obj_set_style_opa(o, on ? LV_OPA_COVER : LV_OPA_40, 0);
}

static void update_buttons(void)
{
    int edge = dsc.header.w > dsc.header.h ? (int)dsc.header.w : (int)dsc.header.h;
    set_enabled(z_minus, loaded && level > 0);
    set_enabled(z_plus, loaded && edge < max_edge() && level < level_cap);
    set_enabled(z_fit, loaded && level > 0);
    set_enabled(r_left, loaded);
    set_enabled(r_right, loaded);
}

static void apply_bars(void)
{
    if(!top_bar) return;
    if(bars_on) { lv_obj_remove_flag(top_bar, LV_OBJ_FLAG_HIDDEN); lv_obj_remove_flag(bot_bar, LV_OBJ_FLAG_HIDDEN); }
    else { lv_obj_add_flag(top_bar, LV_OBJ_FLAG_HIDDEN); lv_obj_add_flag(bot_bar, LV_OBJ_FLAG_HIDDEN); }
}

/* canto do objeto de w x h na área: centrado se cabe, senão 0 (rolagem) */
static int off_x(int w) { return w < VIEW_W ? (VIEW_W - w) / 2 : 0; }
static int off_y(int h) { return h < view_h ? (view_h - h) / 2 : 0; }

static void place(lv_obj_t *o, int w, int h) { lv_obj_set_pos(o, off_x(w), off_y(h)); }

static int clamp1000(long v) { return v < 0 ? 0 : v > 1000 ? 1000 : (int)v; }

static void show_error(const char *detail)
{
    loaded = false;
    lv_obj_add_flag(img, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(zoom_lbl, "");
    lv_label_set_text_fmt(busy, "%s%s%s", tr(S_VIEW_CANT_OPEN), detail && *detail ? "\n" : "", detail ? detail : "");
    lv_obj_set_style_text_color(busy, pal()->warning, 0);
    lv_obj_remove_flag(busy, LV_OBJ_FLAG_HIDDEN);
    bars_on = true;                            /* o aviso vem com os controles para sair */
    apply_bars();
    update_buttons();
}

/* keep_center: mantém o ponto do meio da tela (zoom); senão centraliza a imagem */
static void render(bool keep_center)
{
    int fx = 500, fy = 500;                    /* centro atual em milésimos da imagem */
    if(keep_center && loaded && dsc.header.w && dsc.header.h) {
        int w0 = (int)dsc.header.w, h0 = (int)dsc.header.h;
        fx = clamp1000(((long)lv_obj_get_scroll_x(view) + VIEW_W / 2 - off_x(w0)) * 1000 / w0);
        fy = clamp1000(((long)lv_obj_get_scroll_y(view) + view_h / 2 - off_y(h0)) * 1000 / h0);
    }
    lv_obj_set_style_text_color(busy, lv_color_white(), 0);
    lv_label_set_text(busy, tr(S_VIEW_RENDERING));
    lv_obj_remove_flag(busy, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(img, LV_OBJ_FLAG_HIDDEN);  /* não desenha o buffer que vai ser liberado */
    lv_refr_now(NULL);                         /* mostra "renderizando" antes do trabalho */
    img_free(&dsc);
    loaded = false;
    int w, h;
    target(&w, &h);
    int edge = max_edge();
    if(w > edge || h > edge) {                 /* fotos grandes: no máximo 2048 px de lado */
        if(w >= h) { h = (int)((long)h * edge / w); w = edge; }
        else { w = (int)((long)w * edge / h); h = edge; }
        if(w < 1) w = 1;
        if(h < 1) h = 1;
    }
    char err[96] = "";
    uint32_t t0 = lv_tick_get();
    /* decodifica na orientação do arquivo e gira depois */
    bool ok = img_load_fit(cur_path, rot & 1 ? h : w, rot & 1 ? w : h, lv_color_hex(BG_HEX), &dsc, err, sizeof err);
    if(ok && rot && !rotate_pixels(&dsc, rot)) {
        img_free(&dsc);
        snprintf(err, sizeof err, "%s", tr(S_VIEW_NO_MEMORY));
        ok = false;
    }
    fprintf(stderr, "imagem: %s %dx%d giro %d em %u ms%s%s\n", cur_path, (int)dsc.header.w, (int)dsc.header.h,
            rot * 90, lv_tick_elaps(t0), ok ? "" : ": ", ok ? "" : err);
    if(!ok && level > 0) {                     /* zoom grande demais: fica no nível anterior */
        level_cap = --level;
        render(false);
        return;
    }
    if(!ok && rot) {                           /* sem memória para girar: volta ao original */
        rot = 0;
        render(false);
        return;
    }
    if(!ok) { show_error(err); return; }
    loaded = true;
    lv_obj_add_flag(busy, LV_OBJ_FLAG_HIDDEN);
    lv_image_set_src(img, &dsc);
    lv_obj_set_size(img, (int32_t)dsc.header.w, (int32_t)dsc.header.h);
    place(img, (int)dsc.header.w, (int)dsc.header.h);
    lv_obj_remove_flag(img, LV_OBJ_FLAG_HIDDEN);
    lv_obj_update_layout(view);
    int sx = (int)((long)dsc.header.w * fx / 1000) - VIEW_W / 2;
    int sy = (int)((long)dsc.header.h * fy / 1000) - view_h / 2;
    int mx = (int)dsc.header.w - VIEW_W, my = (int)dsc.header.h - view_h;
    sx = sx > mx ? mx : sx;
    sy = sy > my ? my : sy;
    lv_obj_scroll_to(view, sx > 0 ? sx : 0, sy > 0 ? sy : 0, LV_ANIM_OFF);
    char z[16];
    snprintf(z, sizeof z, "%d%%", (int)((long)dsc.header.w * 100 / disp_w()));
    lv_label_set_text(zoom_lbl, z);
    update_buttons();
}

void imgview_zoom(int d)
{
    if(!ov || !loaded) return;
    if(d == 0) { if(!level) return; level = 0; render(false); return; }
    if(d < 0) { if(!level) return; level--; }
    else {
        int edge = dsc.header.w > dsc.header.h ? (int)dsc.header.w : (int)dsc.header.h;
        if(edge >= max_edge() || level >= level_cap) return;   /* já no limite */
        level++;
    }
    render(level > 0);
}

static void zoom_click(lv_event_t *e) { imgview_zoom((int)(intptr_t)lv_event_get_user_data(e)); }

void imgview_rotate(int dir)
{
    if(!ov || !loaded) return;
    rot = (rot + (dir < 0 ? 3 : 1)) & 3;
    level = 0;                                         /* a imagem girada volta a caber */
    render(false);
}

static void rotate_click(lv_event_t *e) { imgview_rotate((int)(intptr_t)lv_event_get_user_data(e)); }

static void close_click(lv_event_t *e) { (void)e; imgview_close(); }

static void go(int index)
{
    if(nav_cb && index >= 0 && index < nav_count && index != nav_idx) nav_cb(index, nav_ud);
}

static void step_click(lv_event_t *e) { go(nav_idx + (int)(intptr_t)lv_event_get_user_data(e)); }

static void slider_event(lv_event_t *e)
{
    int v = (int)lv_slider_get_value(slider);
    if(lv_event_get_code(e) == LV_EVENT_VALUE_CHANGED) lv_label_set_text_fmt(pos_lbl, "%d / %d", v, nav_count);
    else go(v - 1);                                   /* soltou: abre a imagem */
}

/* toque mostra/esconde os controles; deslizar (sem zoom) troca de imagem */
static void view_event(lv_event_t *e)
{
    if(lv_event_get_code(e) == LV_EVENT_GESTURE) {
        lv_dir_t d = lv_indev_get_gesture_dir(lv_indev_active());
        if(!nav_cb || level) return;                  /* com zoom o dedo arrasta a imagem */
        if(d == LV_DIR_LEFT || d == LV_DIR_RIGHT) {
            lv_indev_wait_release(lv_indev_active());  /* não vira toque */
            go(nav_idx + (d == LV_DIR_LEFT ? 1 : -1));
        }
        return;
    }
    bars_on = !bars_on;
    apply_bars();
}

static lv_obj_t *tool(lv_obj_t *bar, const char *sym, const char *txt, lv_event_cb_t cb, intptr_t ud)
{
    lv_obj_t *b = bf_button(bar, sym, txt, 0);
    lv_obj_set_height(b, 44);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, (void *)ud);
    return b;
}

static lv_obj_t *bar_create(lv_align_t align)
{
    lv_obj_t *o = lv_obj_create(ov);
    lv_obj_remove_style_all(o);
    lv_obj_set_size(o, LV_PCT(100), BAR_H);
    lv_obj_align(o, align, 0, 0);
    lv_obj_set_style_bg_color(o, pal()->surface, 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_hor(o, BF_SP_S, 0);
    lv_obj_set_style_pad_column(o, BF_SP_S, 0);
    lv_obj_add_flag(o, LV_OBJ_FLAG_CLICKABLE);         /* toque na barra não esconde os controles */
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(o, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(o, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    return o;
}

void imgview_set_nav(imgview_nav_cb_t cb, void *ud, int index, int count)
{
    nav_cb = cb;
    nav_ud = ud;
    nav_idx = index;
    nav_count = count;
}

void imgview_show_controls(bool on)
{
    bars_on = on;
    apply_bars();
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
    if(!lv_gif_is_loaded(gif)) {                /* decodificador recusou: vai como imagem estática */
        lv_obj_delete(gif);
        gif = NULL;
        free(gif_data);
        gif_data = NULL;
        return false;
    }
    place(gif, nat_w, nat_h);
    return true;
}

static void close_internal(void)
{
    if(ov) { lv_obj_delete(ov); ov = NULL; }     /* apaga o lv_gif antes dos dados dele */
    img_free(&dsc);
    free(gif_data);
    gif_data = NULL;
    loaded = false;
    view = img = gif = busy = top_bar = bot_bar = NULL;
    zoom_lbl = z_minus = z_plus = z_fit = r_left = r_right = slider = pos_lbl = NULL;
}

static const char *kind_name(img_kind_t k)
{
    return k == IMG_JPEG ? "JPEG" : k == IMG_PNG ? "PNG" : k == IMG_BMP ? "BMP" : k == IMG_GIF ? "GIF" : "SVG";
}

/* "4000 × 3000 · 2,4 MB · JPEG" */
static void info_text(char *out, size_t n, bool probed)
{
    struct stat st;
    long long b = stat(cur_path, &st) == 0 ? (long long)st.st_size : -1;
    char sz[24] = "";
    if(b >= 1024 * 1024) snprintf(sz, sizeof sz, "%lld,%lld MB", b >> 20, ((b & 0xfffff) * 10) >> 20);
    else if(b >= 1024) snprintf(sz, sizeof sz, "%lld KB", (b + 512) / 1024);
    else if(b >= 0) snprintf(sz, sizeof sz, "%lld B", b);
    char dim[32] = "";
    if(probed) snprintf(dim, sizeof dim, "%d \xC3\x97 %d  \xC2\xB7  ", nat_w, nat_h);
    snprintf(out, n, "%s%s%s%s", dim, sz, sz[0] ? "  \xC2\xB7  " : "", kind_name(kind));
}

bool imgview_open(const char *path)
{
    close_internal();
    kind = img_kind_of(path);
    if(kind == IMG_UNKNOWN) return false;
    snprintf(cur_path, sizeof cur_path, "%s", path);
    bool probed = img_probe(path, &nat_w, &nat_h) && nat_w > 0 && nat_h > 0;
    if(!probed) nat_w = nat_h = 1;
    level = 0;
    rot = 0;
    level_cap = 64;
    view_h = BF_CONTENT_H;
    const bf_palette_t *p = pal();

    /* na área de conteúdo: a barra superior do sistema (e a inferior) continuam */
    ov = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(ov);
    lv_obj_set_size(ov, VIEW_W, view_h);
    lv_obj_set_pos(ov, 0, BF_TOPBAR_H);
    lv_obj_set_style_bg_color(ov, lv_color_hex(BG_HEX), 0);
    lv_obj_set_style_bg_opa(ov, LV_OPA_COVER, 0);
    lv_obj_add_flag(ov, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(ov, LV_OBJ_FLAG_SCROLLABLE);

    view = lv_obj_create(ov);
    lv_obj_remove_style_all(view);
    lv_obj_set_size(view, VIEW_W, view_h);
    lv_obj_set_scrollbar_mode(view, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_add_flag(view, LV_OBJ_FLAG_SCROLLABLE);           /* nas duas direções (zoom) */
    lv_obj_remove_flag(view, LV_OBJ_FLAG_GESTURE_BUBBLE);    /* o gesto fica aqui, não no pai */
    lv_obj_remove_flag(view, LV_OBJ_FLAG_SCROLL_ELASTIC);
    lv_obj_add_style(view, &bf_st_scrollbar, LV_PART_SCROLLBAR);
    lv_obj_add_event_cb(view, view_event, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(view, view_event, LV_EVENT_GESTURE, NULL);
    img = lv_image_create(view);
    lv_obj_add_flag(img, LV_OBJ_FLAG_HIDDEN);

    /* controles (escondidos até tocar na imagem) */
    top_bar = bar_create(LV_ALIGN_TOP_MID);
    bf_nav_entry_t cur = nav_current();
    tool(top_bar, LV_SYMBOL_LEFT, nav_title(cur.scr, cur.arg), close_click, 0);
    const char *base = strrchr(path, '/');
    lv_obj_t *t = bf_label(top_bar, base ? base + 1 : path, BF_FONT_M, p->text);
    bf_label_ellipsis(t, 120);
    lv_obj_set_flex_grow(t, 1);
    z_minus = tool(top_bar, LV_SYMBOL_MINUS, NULL, zoom_click, -1);
    zoom_lbl = bf_label(top_bar, "", BF_FONT_S, p->text);
    lv_obj_set_width(zoom_lbl, 56);
    lv_obj_set_style_text_align(zoom_lbl, LV_TEXT_ALIGN_CENTER, 0);
    z_plus = tool(top_bar, LV_SYMBOL_PLUS, NULL, zoom_click, 1);
    z_fit = tool(top_bar, BF_SYMBOL_EXPAND, tr(S_VIEW_FIT), zoom_click, 0);
    r_left = tool(top_bar, BF_SYMBOL_ROT_LEFT, NULL, rotate_click, -1);
    r_right = tool(top_bar, BF_SYMBOL_ROT_RIGHT, NULL, rotate_click, 1);

    bot_bar = bar_create(LV_ALIGN_BOTTOM_MID);
    if(nav_cb && nav_count > 1) {
        lv_obj_t *prev = tool(bot_bar, LV_SYMBOL_LEFT, NULL, step_click, -1);
        set_enabled(prev, nav_idx > 0);
        slider = lv_slider_create(bot_bar);
        lv_obj_set_height(slider, 12);
        lv_obj_set_width(slider, 1);                               /* o tamanho vem do flex_grow */
        lv_obj_set_flex_grow(slider, 1);
        lv_obj_set_ext_click_area(slider, 18);
        lv_obj_set_style_bg_color(slider, p->text_dim, LV_PART_MAIN);
        lv_obj_set_style_bg_opa(slider, LV_OPA_40, LV_PART_MAIN);
        lv_obj_set_style_bg_color(slider, lv_color_hex(C_ACCENT), LV_PART_INDICATOR);
        lv_obj_set_style_bg_color(slider, p->text, LV_PART_KNOB);
        lv_obj_set_style_pad_all(slider, 7, LV_PART_KNOB);
        lv_slider_set_range(slider, 1, nav_count);
        lv_slider_set_value(slider, nav_idx + 1, LV_ANIM_OFF);
        lv_obj_add_event_cb(slider, slider_event, LV_EVENT_VALUE_CHANGED, NULL);
        lv_obj_add_event_cb(slider, slider_event, LV_EVENT_RELEASED, NULL);
        pos_lbl = bf_label(bot_bar, "", BF_FONT_S, p->text);
        lv_label_set_text_fmt(pos_lbl, "%d / %d", nav_idx + 1, nav_count);
        lv_obj_set_width(pos_lbl, 88);
        lv_obj_set_style_text_align(pos_lbl, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_t *next = tool(bot_bar, LV_SYMBOL_RIGHT, NULL, step_click, 1);
        set_enabled(next, nav_idx < nav_count - 1);
    }
    char info[96];
    info_text(info, sizeof info, probed);
    lv_obj_t *il = bf_label(bot_bar, info, BF_FONT_S, p->text_dim);
    bf_label_ellipsis(il, 230);
    lv_obj_set_style_text_align(il, LV_TEXT_ALIGN_RIGHT, 0);
    if(!slider) {                                  /* sem navegação: a informação ocupa a barra */
        lv_obj_set_flex_grow(il, 1);
        lv_obj_set_style_text_align(il, LV_TEXT_ALIGN_CENTER, 0);
    }

    busy = bf_label(ov, "", BF_FONT_M, lv_color_white());
    lv_label_set_long_mode(busy, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(busy, VIEW_W - 2 * BF_SP_XL);
    lv_obj_set_style_text_align(busy, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(busy);
    apply_bars();

    if(!probed) { show_error(NULL); return true; }
    if(open_gif_anim()) {                       /* animado, no tamanho natural */
        lv_obj_add_flag(busy, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(zoom_lbl, "GIF");
        update_buttons();                       /* loaded = false: sem zoom nem giro */
        return true;
    }
    render(false);
    return true;
}

void imgview_close(void)
{
    close_internal();
    nav_cb = NULL;
    nav_ud = NULL;
    nav_idx = nav_count = 0;
    bars_on = false;
}

bool imgview_is_open(void) { return ov != NULL; }
