/* padview.c — ver padview.h.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "padview.h"
#include "../core/padmap.h"
#include "../core/svgimg.h"
#include "../core/theme.h"
#include <stdlib.h>
#include <string.h>

#define VW 400                          /* coordenadas do desenho */
#define VH 250

/* corpo com as duas pegadas (lembra um DualShock visto de frente) */
#define BODY_PATH "M95 38H305C345 38 365 52 378 90C395 140 400 190 392 222C386 246 352 250 336 232" \
                  "C318 210 300 182 270 178H130C100 182 82 210 64 232C48 250 14 246 8 222" \
                  "C0 190 5 140 22 90C35 52 55 38 95 38Z"
static const char body_svg[] =
    "<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 400 250' width='400' height='250'>"
    "<path fill='#fff' d='" BODY_PATH "'/></svg>";
static const char edge_svg[] =
    "<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 400 250' width='400' height='250'>"
    "<path fill='none' stroke='#fff' stroke-width='2.5' d='" BODY_PATH "'/></svg>";

/* as máscaras ficam prontas por largura (o nanosvg usa ponto flutuante emulado: uma vez só) */
#define CACHE 4
static struct { int w; lv_image_dsc_t body, edge; } cache[CACHE];

static int cached(int w)
{
    for(int i = 0; i < CACHE; i++) if(cache[i].w == w) return i;
    int i = 0;
    while(i < CACHE && cache[i].w) i++;
    if(i == CACHE) {                                     /* cheio: troca o primeiro */
        i = 0;
        svg_image_free(&cache[0].body);
        svg_image_free(&cache[0].edge);
        cache[0].w = 0;
    }
    if(!svg_render_mask_str(body_svg, w, &cache[i].body)) return -1;
    svg_render_mask_str(edge_svg, w, &cache[i].edge);
    cache[i].w = w;
    return i;
}

int padview_height(int w) { return w * VH / VW; }

/* ------------------------------------------------------------ estado do desenho */

typedef struct { lv_obj_t *o; int ctl; uint32_t col; bool on, done; } part_t;

typedef struct {
    int w;
    part_t part[PC__NBTN + 2];
    int n;
    lv_obj_t *dot[2], *stick[2], *trig[2], *fill[2], *ring, *arrow;
    int16_t ax[PC__NAXIS];
    int focus;
} pv_t;

static int S(const pv_t *v, int x) { return x * v->w / VW; }

static lv_obj_t *blob(lv_obj_t *par, int x, int y, int w, int h, int r)
{
    lv_obj_t *o = lv_obj_create(par);
    lv_obj_remove_style_all(o);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, w, h);
    lv_obj_set_style_radius(o, r, 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    return o;
}

/* uma peça que acende: fundo escuro e borda na cor dela; centro (cx, cy) no desenho */
static lv_obj_t *part(pv_t *v, lv_obj_t *par, int ctl, int cx, int cy, int w, int h, int r, uint32_t col,
                      const char *txt)
{
    const bf_palette_t *p = pal();
    int pw = S(v, w), ph = S(v, h);
    lv_obj_t *o = blob(par, S(v, cx) - pw / 2, S(v, cy) - ph / 2, pw, ph, r == LV_RADIUS_CIRCLE ? r : S(v, r));
    lv_obj_set_style_bg_color(o, lv_color_mix(p->surface, lv_color_black(), 200), 0);
    lv_obj_set_style_border_width(o, v->w >= 300 ? 2 : 1, 0);
    lv_obj_set_style_border_color(o, lv_color_hex(col), 0);
    lv_obj_set_style_border_opa(o, LV_OPA_50, 0);
    lv_obj_set_style_shadow_color(o, lv_color_hex(col), 0);
    if(txt && v->w >= 300) {
        lv_obj_t *l = bf_label(o, txt, BF_FONT_S, lv_color_hex(col));
        lv_obj_center(l);
    }
    if(v->n < (int)(sizeof v->part / sizeof v->part[0])) v->part[v->n++] = (part_t){ o, ctl, col, false, false };
    return o;
}

/* as formas do PlayStation com linhas (a fonte não tem △ ○ ✕ □) */
static void shape(pv_t *v, lv_obj_t *o, int kind, uint32_t col)
{
    lv_color_t c = lv_color_hex(col);
    int z = S(v, 14), lw = v->w >= 300 ? 2 : 1;
    if(kind == 1 || kind == 3) {                          /* bola, quadrado */
        lv_obj_t *r = blob(o, 0, 0, z + (kind == 1 ? 1 : 0), z, kind == 1 ? LV_RADIUS_CIRCLE : 1);
        lv_obj_set_style_bg_opa(r, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(r, lw, 0);
        lv_obj_set_style_border_color(r, c, 0);
        lv_obj_center(r);
        return;
    }
    /* os pontos precisam viver junto com a linha: ficam num bloco alocado por forma */
    lv_point_precise_t *pt = lv_malloc(sizeof(lv_point_precise_t) * 6);
    if(!pt) return;
    if(kind == 0) {                                       /* xis */
        pt[0] = (lv_point_precise_t){ 0, 0 }; pt[1] = (lv_point_precise_t){ z, z };
        pt[2] = (lv_point_precise_t){ z, 0 }; pt[3] = (lv_point_precise_t){ 0, z };
    } else {                                              /* triângulo */
        pt[0] = (lv_point_precise_t){ z / 2, 0 }; pt[1] = (lv_point_precise_t){ z, z - z / 8 };
        pt[2] = (lv_point_precise_t){ 0, z - z / 8 }; pt[3] = (lv_point_precise_t){ z / 2, 0 };
    }
    for(int k = 0; k < (kind == 0 ? 2 : 1); k++) {
        lv_obj_t *l = lv_line_create(o);
        lv_line_set_points(l, kind == 0 ? pt + 2 * k : pt, kind == 0 ? 2 : 4);
        lv_obj_set_style_line_width(l, lw, 0);
        lv_obj_set_style_line_color(l, c, 0);
        lv_obj_set_style_line_rounded(l, true, 0);
        lv_obj_center(l);
    }
    lv_obj_set_user_data(o, pt);                          /* liberado com o desenho */
}

static void pv_deleted(lv_event_t *e)
{
    pv_t *v = lv_event_get_user_data(e);
    if(!v) return;
    if(v->ring) lv_anim_delete(v->ring, NULL);
    for(int i = 0; i < v->n; i++) {
        void *pt = lv_obj_get_user_data(v->part[i].o);
        if(pt) lv_free(pt);
    }
    lv_free(v);
}

/* o que está desenhado dentro da peça contrasta com o fundo aceso */
static void ink(lv_obj_t *o, bool on, uint32_t col, const pv_t *v)
{
    bool light = (((col >> 16) & 0xff) * 3 + ((col >> 8) & 0xff) * 6 + (col & 0xff)) / 10 > 170;
    lv_color_t c = on ? (light ? lv_color_hex(0x1a1d26) : lv_color_white()) : lv_color_hex(col);
    for(uint32_t i = 0; i < lv_obj_get_child_count(o); i++) {
        lv_obj_t *ch = lv_obj_get_child(o, (int32_t)i);
        if(ch == v->dot[0] || ch == v->dot[1]) continue;
        if(lv_obj_check_type(ch, &lv_line_class)) lv_obj_set_style_line_color(ch, c, 0);
        else if(lv_obj_check_type(ch, &lv_label_class)) lv_obj_set_style_text_color(ch, c, 0);
        else lv_obj_set_style_border_color(ch, c, 0);
    }
}

static void paint(pv_t *v, part_t *pt)
{
    const bf_palette_t *p = pal();
    uint32_t col = pt->done && !pt->on ? 0x2ec27e : pt->col;
    lv_obj_set_style_bg_color(pt->o, pt->on ? lv_color_hex(col) : lv_color_mix(p->surface, lv_color_black(), 200), 0);
    lv_obj_set_style_border_color(pt->o, lv_color_hex(col), 0);
    lv_obj_set_style_border_opa(pt->o, pt->on || pt->done ? LV_OPA_COVER : LV_OPA_50, 0);
    lv_obj_set_style_shadow_width(pt->o, pt->on ? S(v, 18) : 0, 0);
    lv_obj_set_style_shadow_opa(pt->o, LV_OPA_70, 0);
    ink(pt->o, pt->on, col, v);
}

/* ------------------------------------------------------------ montar */

lv_obj_t *padview_create(lv_obj_t *parent, int w, bool sony)
{
    const bf_palette_t *p = pal();
    pv_t *v = lv_malloc_zeroed(sizeof *v);
    lv_obj_t *root = bf_plain(parent);
    int h = padview_height(w);
    lv_obj_set_size(root, w, h);
    lv_obj_set_style_clip_corner(root, false, 0);
    if(!v) return root;
    v->w = w;
    v->focus = -1;
    lv_obj_set_user_data(root, v);
    lv_obj_add_event_cb(root, pv_deleted, LV_EVENT_DELETE, v);
    uint32_t acc = 0x9f86ff, key = 0xc9d1e0;

    /* gatilhos (barras que enchem) e ombros, atrás do corpo */
    for(int k = 0; k < 2; k++) {
        int x = k ? 254 : 62;
        v->trig[k] = blob(root, S(v, x), 0, S(v, 84), S(v, 11), S(v, 5));
        lv_obj_set_style_bg_color(v->trig[k], lv_color_mix(p->surface, lv_color_black(), 200), 0);
        lv_obj_set_style_border_width(v->trig[k], 1, 0);
        lv_obj_set_style_border_color(v->trig[k], lv_color_hex(acc), 0);
        lv_obj_set_style_border_opa(v->trig[k], LV_OPA_60, 0);
        lv_obj_set_style_clip_corner(v->trig[k], true, 0);
        v->fill[k] = blob(v->trig[k], 0, 0, 0, S(v, 11), 0);
        lv_obj_set_style_bg_color(v->fill[k], lv_color_hex(acc), 0);
        if(k) lv_obj_align(v->fill[k], LV_ALIGN_RIGHT_MID, 0, 0);
        part(v, root, k ? PC_RSHOULDER : PC_LSHOULDER, k ? 296 : 104, 25, 92, 22, 11, acc,
             sony ? (k ? "R1" : "L1") : (k ? "RB" : "LB"));
    }

    /* corpo: sombra, preenchimento e contorno */
    int ci = cached(w);
    if(ci >= 0) {
        lv_obj_t *sh = lv_image_create(root);
        lv_image_set_src(sh, &cache[ci].body);
        lv_obj_set_pos(sh, 0, S(v, 7));
        lv_obj_set_style_image_recolor(sh, lv_color_black(), 0);
        lv_obj_set_style_image_recolor_opa(sh, LV_OPA_COVER, 0);
        lv_obj_set_style_image_opa(sh, LV_OPA_40, 0);
        lv_obj_t *bd = lv_image_create(root);
        lv_image_set_src(bd, &cache[ci].body);
        lv_obj_set_style_image_recolor(bd, lv_color_mix(lv_color_hex(0x46506a), p->surface2, 150), 0);
        lv_obj_set_style_image_recolor_opa(bd, LV_OPA_COVER, 0);
        if(cache[ci].edge.data) {
            lv_obj_t *ed = lv_image_create(root);
            lv_image_set_src(ed, &cache[ci].edge);
            lv_obj_set_style_image_recolor(ed, lv_color_hex(acc), 0);
            lv_obj_set_style_image_recolor_opa(ed, LV_OPA_COVER, 0);
            lv_obj_set_style_image_opa(ed, LV_OPA_50, 0);
        }
    }

    /* direcional */
    static const int dp[4][2] = { { 0, -26 }, { 0, 26 }, { -26, 0 }, { 26, 0 } };
    static const char *const arrows[4] = { LV_SYMBOL_UP, LV_SYMBOL_DOWN, LV_SYMBOL_LEFT, LV_SYMBOL_RIGHT };
    for(int d = 0; d < 4; d++) part(v, root, PC_DPUP + d, 96 + dp[d][0], 104 + dp[d][1], 24, 24, 5, key, arrows[d]);

    /* botões de frente: Y em cima, A embaixo, X à esquerda, B à direita */
    static const int fb[4][3] = { { PC_Y, 0, -28 }, { PC_A, 0, 28 }, { PC_X, -28, 0 }, { PC_B, 28, 0 } };
    static const uint32_t pscol[4] = { 0x40e2a0, 0x7cb2e8, 0xff69f8, 0xff6666 };   /* △ ✕ □ ○ */
    static const uint32_t xcol[4] = { 0xf4c430, 0x6fcf4a, 0x4aa3ff, 0xff5a52 };    /* Y A X B */
    static const char *const xl[4] = { "Y", "A", "X", "B" };
    static const int pskind[4] = { 2, 0, 3, 1 };
    for(int k = 0; k < 4; k++) {
        lv_obj_t *o = part(v, root, fb[k][0], 304 + fb[k][1], 104 + fb[k][2], 28, 28, LV_RADIUS_CIRCLE,
                           sony ? pscol[k] : xcol[k], sony ? NULL : xl[k]);
        if(sony) shape(v, o, pskind[k], pscol[k]);
    }

    /* centro */
    if(sony) {
        part(v, root, PC_TOUCHPAD, 200, 80, 100, 54, 10, key, NULL);
        part(v, root, PC_BACK, 137, 58, 18, 11, 5, key, NULL);
        part(v, root, PC_START, 263, 58, 18, 11, 5, key, NULL);
        part(v, root, PC_GUIDE, 200, 132, 22, 22, LV_RADIUS_CIRCLE, key, NULL);
    } else {
        part(v, root, PC_BACK, 168, 108, 22, 13, 6, key, NULL);
        part(v, root, PC_START, 232, 108, 22, 13, 6, key, NULL);
        part(v, root, PC_GUIDE, 200, 78, 30, 30, LV_RADIUS_CIRCLE, key, NULL);
    }

    /* analógicos: anel (acende ao afundar) e o ponto que anda */
    for(int k = 0; k < 2; k++) {
        v->stick[k] = part(v, root, k ? PC_RSTICK : PC_LSTICK, k ? 252 : 148, 166, 56, 56, LV_RADIUS_CIRCLE, acc, NULL);
        v->dot[k] = blob(v->stick[k], 0, 0, S(v, 26), S(v, 26), LV_RADIUS_CIRCLE);
        lv_obj_set_style_bg_color(v->dot[k], lv_color_hex(0xe6eaf2), 0);
        lv_obj_set_style_bg_grad_color(v->dot[k], lv_color_hex(0x9aa3b5), 0);
        lv_obj_set_style_bg_grad_dir(v->dot[k], LV_GRAD_DIR_VER, 0);
        lv_obj_center(v->dot[k]);
    }

    /* destaque do registro: anel que pulsa e seta dos analógicos */
    v->ring = blob(root, 0, 0, 10, 10, LV_RADIUS_CIRCLE);
    lv_obj_set_style_bg_opa(v->ring, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(v->ring, v->w >= 300 ? 3 : 2, 0);
    lv_obj_set_style_border_color(v->ring, lv_color_hex(0xffd24a), 0);
    lv_obj_add_flag(v->ring, LV_OBJ_FLAG_HIDDEN);
    v->arrow = bf_label(root, "", BF_FONT_L, lv_color_hex(0xffd24a));
    lv_obj_add_flag(v->arrow, LV_OBJ_FLAG_HIDDEN);
    for(int i = 0; i < v->n; i++) paint(v, &v->part[i]);
    return root;
}

/* ------------------------------------------------------------ ao vivo */

void padview_state(lv_obj_t *pv, const bool *btn, const int16_t *ax)
{
    pv_t *v = pv ? lv_obj_get_user_data(pv) : NULL;
    if(!v) return;
    for(int i = 0; i < v->n; i++) {
        part_t *pt = &v->part[i];
        if(pt->ctl >= PC__NBTN || btn[pt->ctl] == pt->on) continue;
        pt->on = btn[pt->ctl];
        paint(v, pt);
    }
    int travel = S(v, 14);
    for(int k = 0; k < 2; k++) {
        int ix = (k ? PC_RIGHTX : PC_LEFTX) - PC__NBTN, iy = ix + 1;
        if(ax[ix] != v->ax[ix] || ax[iy] != v->ax[iy]) {
            v->ax[ix] = ax[ix];
            v->ax[iy] = ax[iy];
            lv_obj_align(v->dot[k], LV_ALIGN_CENTER, ax[ix] * travel / 1000, ax[iy] * travel / 1000);
        }
        int it = (k ? PC_RTRIGGER : PC_LTRIGGER) - PC__NBTN;
        if(ax[it] != v->ax[it]) {
            v->ax[it] = ax[it];
            lv_obj_set_width(v->fill[k], S(v, 84) * (ax[it] < 0 ? 0 : ax[it]) / 1000);
            lv_obj_align(v->fill[k], k ? LV_ALIGN_RIGHT_MID : LV_ALIGN_LEFT_MID, 0, 0);
        }
    }
}

static void ring_anim(void *o, int32_t v) { lv_obj_set_style_border_opa(o, (lv_opa_t)v, 0); }

void padview_focus(lv_obj_t *pv, int ctl)
{
    pv_t *v = pv ? lv_obj_get_user_data(pv) : NULL;
    if(!v || v->focus == ctl) return;
    v->focus = ctl;
    lv_anim_delete(v->ring, ring_anim);
    lv_obj_add_flag(v->ring, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(v->arrow, LV_OBJ_FLAG_HIDDEN);
    if(ctl < 0) return;
    /* o alvo: a peça, o analógico (eixo) ou o gatilho */
    lv_obj_t *t = NULL;
    const char *arrow = NULL;
    switch(ctl) {
    case PC_LEFTX: t = v->stick[0]; arrow = LV_SYMBOL_RIGHT; break;
    case PC_LEFTY: t = v->stick[0]; arrow = LV_SYMBOL_DOWN; break;
    case PC_RIGHTX: t = v->stick[1]; arrow = LV_SYMBOL_RIGHT; break;
    case PC_RIGHTY: t = v->stick[1]; arrow = LV_SYMBOL_DOWN; break;
    case PC_LTRIGGER: t = v->trig[0]; break;
    case PC_RTRIGGER: t = v->trig[1]; break;
    default:
        for(int i = 0; i < v->n; i++) if(v->part[i].ctl == ctl) t = v->part[i].o;
    }
    if(!t) return;
    lv_obj_update_layout(pv);
    int x = lv_obj_get_x(t), y = lv_obj_get_y(t), w = lv_obj_get_width(t), h = lv_obj_get_height(t), m = S(v, 7);
    lv_obj_set_pos(v->ring, x - m, y - m);
    lv_obj_set_size(v->ring, w + 2 * m, h + 2 * m);
    lv_obj_set_style_radius(v->ring, lv_obj_get_style_radius(t, 0) == LV_RADIUS_CIRCLE ? LV_RADIUS_CIRCLE
                                     : lv_obj_get_style_radius(t, 0) + m, 0);
    lv_obj_remove_flag(v->ring, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(v->ring);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, v->ring);
    lv_anim_set_exec_cb(&a, ring_anim);
    lv_anim_set_values(&a, LV_OPA_COVER, LV_OPA_20);
    lv_anim_set_duration(&a, 600);
    lv_anim_set_playback_duration(&a, 600);
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
    lv_anim_start(&a);
    if(arrow) {
        lv_label_set_text(v->arrow, arrow);
        lv_obj_update_layout(v->arrow);
        bool right = ctl == PC_LEFTX || ctl == PC_RIGHTX;
        lv_obj_set_pos(v->arrow, right ? x + w + m + S(v, 4) : x + w / 2 - lv_obj_get_width(v->arrow) / 2,
                       right ? y + h / 2 - lv_obj_get_height(v->arrow) / 2 : y + h + m);
        lv_obj_remove_flag(v->arrow, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(v->arrow);
    }
}

void padview_done(lv_obj_t *pv, int ctl, bool done)
{
    pv_t *v = pv ? lv_obj_get_user_data(pv) : NULL;
    if(!v) return;
    int c = ctl;
    if(ctl == PC_LEFTX || ctl == PC_LEFTY) c = PC_LSTICK;          /* eixos: marca o anel */
    if(ctl == PC_RIGHTX || ctl == PC_RIGHTY) c = PC_RSTICK;
    if(ctl == PC_LTRIGGER || ctl == PC_RTRIGGER) {
        lv_obj_set_style_border_color(v->trig[ctl == PC_RTRIGGER], lv_color_hex(done ? 0x2ec27e : 0x9f86ff), 0);
        lv_obj_set_style_border_opa(v->trig[ctl == PC_RTRIGGER], done ? LV_OPA_COVER : LV_OPA_60, 0);
        return;
    }
    for(int i = 0; i < v->n; i++)
        if(v->part[i].ctl == c && v->part[i].done != done) {
            v->part[i].done = done;
            paint(v, &v->part[i]);
        }
}
