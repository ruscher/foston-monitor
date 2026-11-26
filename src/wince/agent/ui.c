/* ui.c — ver ui.h.
 *
 * Geometria em 800x480 (a tela do FOSTON); outras resoluções usam as mesmas medidas a
 * partir do canto superior esquerdo.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "ui.h"
#include "art.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32_WCE
#define UI_FLUSH() ((void)0)          /* o GDI do CE não acumula chamadas */
#else
#define UI_FLUSH() GdiFlush()         /* prévia no PC (wine) */
#endif

typedef unsigned short u16;
typedef unsigned int u32;

/* paleta escura do BigFoston (tema FOSTON, src/ui/core/theme.c) */
#define C_BG_TOP  0x13204a
#define C_BG_BOT  0x0a1027
#define C_SURF    0x182340
#define C_SURF2   0x24315a
#define C_BORDER  0x33416b
#define C_TEXT    0xeef1f6
#define C_DIM     0x9aabc4
#define C_PRIMARY 0x6c5ce7
#define C_PRIM_DK 0x3d2fb0
#define C_ACCENT  0x26dda8
#define C_OK      0x2ec27e
#define C_WARN    0xf5a524
#define C_ERR     0xe5484d
#define C_PC      0x5aa9e6
#define C_LOGBG   0x080d1f

/* geometria */
#define TOP_H   56
#define CARD_Y  72
#define CARD_H  184
#define PROG_Y  268
#define LOG_Y   326
#define MARGIN  20

static int W, H, stride;
static u16 *bg;                       /* fundo pronto (degradê e brilhos) */
static HDC base_dc, frame_dc;
static HBITMAP base_bmp, frame_bmp, base_old, frame_old;
static u16 *base_px, *frame_px;
static HFONT f_small, f_text, f_bold;
static int base_key = -1;
static RECT card_l, card_r, link_r, ov_card, ov_ok, ov_cancel;

/* ------------------------------------------------------------ cores */

static u16 to565(u32 c)
{
    return (u16)(((c >> 8) & 0xf800) | ((c >> 5) & 0x07e0) | ((c >> 3) & 0x001f));
}

/* d + (c - d) * a / 255, em 8 bits por canal */
static u16 mix(u16 d, u32 c, int a)
{
    int dr, dg, db, cr, cg, cb;
    if (a <= 0)
        return d;
    if (a >= 255)
        return to565(c);
    dr = (d >> 11) << 3; dr |= dr >> 5;
    dg = ((d >> 5) & 63) << 2; dg |= dg >> 6;
    db = (d & 31) << 3; db |= db >> 5;
    cr = (int)(c >> 16) & 255; cg = (int)(c >> 8) & 255; cb = (int)c & 255;
    /* sem divisão (o ARM926 não divide em hardware): a de 0..255 vira 0..256 e >> 8 */
    a += a >> 7;
    dr += ((cr - dr) * a) >> 8;
    dg += ((cg - dg) * a) >> 8;
    db += ((cb - db) * a) >> 8;
    return (u16)(((dr >> 3) << 11) | ((dg >> 2) << 5) | (db >> 3));
}

static u32 lerp(u32 a, u32 b, int t, int n)          /* t de 0 a n */
{
    int r, g, bl;
    if (n <= 0)
        return a;
    r = (int)((a >> 16) & 255) + ((int)((b >> 16) & 255) - (int)((a >> 16) & 255)) * t / n;
    g = (int)((a >> 8) & 255) + ((int)((b >> 8) & 255) - (int)((a >> 8) & 255)) * t / n;
    bl = (int)(a & 255) + ((int)(b & 255) - (int)(a & 255)) * t / n;
    return ((u32)r << 16) | ((u32)g << 8) | (u32)bl;
}

static COLORREF cref(u32 c) { return RGB((c >> 16) & 255, (c >> 8) & 255, c & 255); }

/* ------------------------------------------------------------ fundo */

static const unsigned char bayer[4][4] = { { 0, 8, 2, 10 }, { 12, 4, 14, 6 }, { 3, 11, 1, 9 }, { 15, 7, 13, 5 } };

/* brilho radial (centro cx,cy; raio rad; força max em 1/256) somado ao canal em 1/256 */
static int glow(int x, int y, int cx, int cy, int rad, int max)
{
    int dx = x - cx, dy = y - cy, d2 = dx * dx + dy * dy, r2 = rad * rad;
    int f;
    if (d2 >= r2)
        return 0;
    f = (r2 - d2) / (r2 / 256 + 1);                 /* 0..256 */
    return f * f / 256 * max / 256;                 /* queda suave (quadrática) */
}

static void make_bg(void)
{
    int x, y;
    for (y = 0; y < H; y++) {
        u32 base = lerp(C_BG_TOP, C_BG_BOT, y, H - 1);
        for (x = 0; x < W; x++) {
            /* canais em 1/256 (16 bits) para o pontilhado */
            int g1 = glow(x, y, 130, 150, 420, 70);      /* roxo do tema, alto à esquerda */
            int g2 = glow(x, y, 720, 470, 340, 40);      /* verde-água, embaixo à direita */
            int r = (int)((base >> 16) & 255) * 257 + g1 * ((C_PRIMARY >> 16) & 255) + g2 * ((C_ACCENT >> 16) & 255);
            int g = (int)((base >> 8) & 255) * 257 + g1 * ((C_PRIMARY >> 8) & 255) + g2 * ((C_ACCENT >> 8) & 255);
            int b = (int)(base & 255) * 257 + g1 * (C_PRIMARY & 255) + g2 * (C_ACCENT & 255);
            int t = bayer[y & 3][x & 3] * 4096;
            int r5, g6, b5;
            if (r > 65535) r = 65535;
            if (g > 65535) g = 65535;
            if (b > 65535) b = 65535;
            r5 = (int)(((unsigned)r * 31u + (unsigned)t) >> 16);
            g6 = (int)(((unsigned)g * 63u + (unsigned)t) >> 16);
            b5 = (int)(((unsigned)b * 31u + (unsigned)t) >> 16);
            if (r5 > 31) r5 = 31;
            if (g6 > 63) g6 = 63;
            if (b5 > 31) b5 = 31;
            bg[y * stride + x] = (u16)((r5 << 11) | (g6 << 5) | b5);
        }
    }
}

/* ------------------------------------------------------------ formas */

static int isqrt(unsigned v)
{
    unsigned r = 0, bit = 1u << 30;
    while (bit > v)
        bit >>= 2;
    while (bit) {
        if (v >= r + bit) {
            v -= r + bit;
            r = (r >> 1) + bit;
        } else
            r >>= 1;
        bit >>= 2;
    }
    return (int)r;
}

/* cobertura 0..255 do pixel (px,py) no retângulo [x0,x1)x[y0,y1) de cantos com raio r */
static int rr_cov(int px, int py, int x0, int y0, int x1, int y1, int r)
{
    int cx = px * 16 + 8, cy = py * 16 + 8, kx, ky, v;
    if (px < x0 || px >= x1 || py < y0 || py >= y1)
        return 0;
    if (r <= 0)
        return 255;
    if (cx < (x0 + r) * 16) kx = (x0 + r) * 16;
    else if (cx > (x1 - r) * 16) kx = (x1 - r) * 16;
    else return 255;
    if (cy < (y0 + r) * 16) ky = (y0 + r) * 16;
    else if (cy > (y1 - r) * 16) ky = (y1 - r) * 16;
    else return 255;
    v = r * 16 + 8 - isqrt((unsigned)((cx - kx) * (cx - kx) + (cy - ky) * (cy - ky)));
    return v <= 0 ? 0 : v >= 16 ? 255 : v * 255 / 16;
}

enum { G_SOLID, G_VERT, G_HORZ };

/* retângulo arredondado: cheio (ring 0) ou só a borda de largura ring; cor c0 ou degradê
 * c0 -> c1; opa 0..255 */
static void rr(u16 *px, int x0, int y0, int w, int h, int r, u32 c0, u32 c1, int grad, int opa, int ring)
{
    int x, y, x1 = x0 + w, y1 = y0 + h;
    int ix0 = x0 + ring, iy0 = y0 + ring, ix1 = x1 - ring, iy1 = y1 - ring, ir = r - ring;
    if (ir < 0) ir = 0;
    for (y = y0 < 0 ? 0 : y0; y < y1 && y < H; y++) {
        u32 rowc = grad == G_VERT ? lerp(c0, c1, y - y0, h - 1) : c0;
        int edge_row = y < y0 + r || y >= y1 - r;
        u16 *row = px + y * stride;
        for (x = x0 < 0 ? 0 : x0; x < x1 && x < W; x++) {
            int a, c;
            if (edge_row || x < x0 + r || x >= x1 - r)
                a = rr_cov(x, y, x0, y0, x1, y1, r);
            else
                a = 255;
            if (ring) {
                /* no miolo da borda não há nada: pula até a outra lateral */
                if (y >= iy0 + ir && y < iy1 - ir && x >= ix0 + ir && x < ix1 - ir) {
                    x = ix1 - ir - 1;
                    continue;
                }
                a -= rr_cov(x, y, ix0, iy0, ix1, iy1, ir);
            }
            if (a <= 0)
                continue;
            c = a * opa / 255;
            row[x] = mix(row[x], grad == G_HORZ ? lerp(c0, c1, x - x0, w - 1) : rowc, c);
        }
    }
}

/* máscara A8 tingida */
static void blit_a8(u16 *px, const art_t *a, int x0, int y0, u32 color, int opa)
{
    int x, y;
    for (y = 0; y < a->h; y++) {
        int yy = y0 + y;
        if (yy < 0 || yy >= H)
            continue;
        for (x = 0; x < a->w; x++) {
            int xx = x0 + x, al = a->px[y * a->w + x];
            if (xx < 0 || xx >= W || !al)
                continue;
            px[yy * stride + xx] = mix(px[yy * stride + xx], color, al * opa / 255);
        }
    }
}

static void blit_rgba(u16 *px, const art_t *a, int x0, int y0)
{
    int x, y;
    for (y = 0; y < a->h; y++) {
        int yy = y0 + y;
        if (yy < 0 || yy >= H)
            continue;
        for (x = 0; x < a->w; x++) {
            const unsigned char *p = a->px + (y * a->w + x) * 4;
            int xx = x0 + x;
            if (xx < 0 || xx >= W || !p[3])
                continue;
            px[yy * stride + xx] = mix(px[yy * stride + xx], ((u32)p[0] << 16) | ((u32)p[1] << 8) | p[2], p[3]);
        }
    }
}

/* ------------------------------------------------------------ texto */

static void text(HDC dc, HFONT f, u32 color, int x0, int y0, int x1, int y1, const WCHAR *s, UINT flags)
{
    RECT r;
    SetRect(&r, x0, y0, x1, y1);
    SelectObject(dc, f);
    SetTextColor(dc, cref(color));
    SetBkMode(dc, TRANSPARENT);
    DrawTextW(dc, s, -1, &r, flags | DT_NOPREFIX);
}

/* altura do texto quebrado em linhas na largura w */
static int text_height(HDC dc, HFONT f, int w, const WCHAR *s)
{
    RECT r;
    SetRect(&r, 0, 0, w, 0);
    SelectObject(dc, f);
    DrawTextW(dc, s, -1, &r, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
    return r.bottom - r.top;
}

static HFONT font(int px, int weight)
{
    LOGFONTW lf;
    memset(&lf, 0, sizeof(lf));
    lf.lfHeight = -px;
    lf.lfWeight = weight;
    lf.lfQuality = ANTIALIASED_QUALITY;
    wcscpy(lf.lfFaceName, L"Tahoma");
    return CreateFontIndirectW(&lf);
}

/* ------------------------------------------------------------ cartões */

/* botão-pílula dentro do cartão: ícone + texto */
static void pill(u16 *px, HDC dc, int x, int y, int w, int h, int primary, int enabled, int pressed,
                 const art_t *icon, const WCHAR *label)
{
    u32 fg = enabled ? C_TEXT : C_DIM;
    int tw, ix;
    RECT m;
    if (primary && enabled)
        rr(px, x, y, w, h, h / 2, pressed ? C_PRIM_DK : C_PRIMARY, pressed ? 0x1d9f7a : C_ACCENT, G_HORZ, 255, 0);
    else {
        rr(px, x, y, w, h, h / 2, pressed ? C_BORDER : C_SURF2, 0, G_SOLID, 255, 0);
        rr(px, x, y, w, h, h / 2, C_BORDER, 0, G_SOLID, 255, 1);
    }
    /* centraliza ícone + texto juntos */
    SetRect(&m, 0, 0, w, h);
    SelectObject(dc, f_bold);
    DrawTextW(dc, label, -1, &m, DT_CALCRECT | DT_SINGLELINE | DT_NOPREFIX);
    tw = m.right - m.left;
    ix = x + (w - (tw + (icon ? icon->w + 10 : 0))) / 2;
    if (icon) {
        blit_a8(px, icon, ix, y + (h - icon->h) / 2, fg, 255);
        ix += icon->w + 10;
    }
    UI_FLUSH();
    text(dc, f_bold, fg, ix, y, ix + tw + 4, y + h, label, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
}

static void card(u16 *px, HDC dc, const RECT *rc, int highlight, int pressed)
{
    int w = rc->right - rc->left, h = rc->bottom - rc->top;
    rr(px, rc->left + 2, rc->top + 6, w, h, 20, 0x000000, 0, G_SOLID, 70, 0);   /* sombra */
    rr(px, rc->left, rc->top, w, h, 20, pressed ? C_SURF2 : C_SURF, 0, G_SOLID, 235, 0);
    if (highlight)
        rr(px, rc->left, rc->top, w, h, 20, C_PRIMARY, C_ACCENT, G_HORZ, 255, 2);
    else
        rr(px, rc->left, rc->top, w, h, 20, C_BORDER, 0, G_SOLID, 255, 1);
    (void)dc;
}

static void build_base(const ui_state_t *s)
{
    u16 *px = base_px;
    int lx = card_l.left, rx = card_r.left, y = CARD_Y, cw = card_l.right - card_l.left;
    int counting = s->autoboot == 1, booting = s->autoboot == 3;
    const WCHAR *lsub, *rsub, *lbtn, *rbtn;
    memcpy(px, bg, (size_t)stride * H * 2);

    /* barra superior: símbolo, "Dual Boot", versão; filete roxo -> verde-água */
    rr(px, 0, 0, W, TOP_H, 0, C_SURF, 0, G_SOLID, 230, 0);
    rr(px, 0, TOP_H - 2, W, 2, 0, C_PRIMARY, C_ACCENT, G_HORZ, 255, 0);
    blit_a8(px, &art_logo_sm, MARGIN, (TOP_H - 2 - art_logo_sm.h) / 2, C_TEXT, 255);
    blit_a8(px, &art_title, MARGIN + art_logo_sm.w + 12, (TOP_H - 2 - art_title.h) / 2, C_TEXT, 255);

    /* BigFoston */
    card(px, base_dc, &card_l, counting || booting, s->pressed == UI_HIT_LINUX);
    rr(px, lx + 20, y + 20, 92, 92, 24, 0x8577f2, C_PRIM_DK, G_VERT, s->linux_ok ? 255 : 90, 0);
    rr(px, lx + 20, y + 20, 92, 92, 24, 0xffffff, 0, G_SOLID, 40, 1);
    blit_a8(px, &art_logo_lg, lx + 20 + (92 - art_logo_lg.w) / 2, y + 20 + (92 - art_logo_lg.h) / 2, 0xffffff,
            s->linux_ok ? 255 : 110);
    blit_a8(px, &art_bigfoston, lx + 128, y + 26, C_TEXT, 255);
    /* Windows CE */
    card(px, base_dc, &card_r, 0, s->pressed == UI_HIT_CE);
    rr(px, rx + 20, y + 20, 92, 92, 24, C_SURF2, 0x141c38, G_VERT, 255, 0);
    rr(px, rx + 20, y + 20, 92, 92, 24, 0xffffff, 0, G_SOLID, 40, 1);
    blit_rgba(px, &art_flag, rx + 20 + (92 - art_flag.w) / 2, y + 20 + (92 - art_flag.h) / 2);
    blit_a8(px, &art_wince, rx + 128, y + 26, C_TEXT, 255);

    if (s->setup == UI_SETUP_READY) {
        lsub = L"Grava o sistema na memória interna";
        lbtn = s->busy ? L"Instalando..." : L"Instalar o Dual Boot";
    } else if (s->setup == UI_SETUP_NOFILES) {
        lsub = L"Prepare o cartão com o BigFoston OS Installer no computador";
        lbtn = L"Indisponível";
    } else if (!s->linux_ok) {
        lsub = L"Linux não instalado na memória interna";
        lbtn = L"Indisponível";
    } else if (booting) {
        lsub = L"Carregando o Linux...";
        lbtn = L"Iniciando";
    } else {
        lsub = L"O Linux do FOSTON";
        lbtn = counting ? L"Iniciar agora" : L"Iniciar o BigFoston";
    }
    rsub = L"O sistema original do aparelho";
    rbtn = counting ? L"Ficar no Windows CE" : L"Voltar ao Windows CE";
    UI_FLUSH();
    text(base_dc, f_text, C_DIM, lx + 128, y + 64, lx + cw - 16, y + 118, lsub, DT_LEFT | DT_TOP | DT_WORDBREAK);
    text(base_dc, f_text, C_DIM, rx + 128, y + 64, rx + cw - 16, y + 118, rsub, DT_LEFT | DT_TOP | DT_WORDBREAK);
    UI_FLUSH();
    pill(px, base_dc, lx + 20, y + CARD_H - 66, cw - 40, 46, 1,
         !s->busy && !booting && (s->setup ? s->setup == UI_SETUP_READY : s->linux_ok), s->pressed == UI_HIT_LINUX,
         &art_ic_power, lbtn);
    UI_FLUSH();
    pill(px, base_dc, rx + 20, y + CARD_H - 66, cw - 40, 46, 0, !booting && !s->busy, s->pressed == UI_HIT_CE,
         &art_ic_back, rbtn);
    UI_FLUSH();
    text(base_dc, f_small, C_DIM, W / 2, 0, W - MARGIN, TOP_H - 2, s->version, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
    if (s->can_restore && !s->busy) {               /* "Restaurar o original": pílula discreta */
        rr(px, link_r.left, link_r.top, link_r.right - link_r.left, link_r.bottom - link_r.top, 15,
           s->pressed == UI_HIT_RESTORE ? C_BORDER : C_SURF2, 0, G_SOLID, 255, 0);
        rr(px, link_r.left, link_r.top, link_r.right - link_r.left, link_r.bottom - link_r.top, 15, C_BORDER, 0,
           G_SOLID, 255, 1);
        blit_a8(px, &art_ic_back, link_r.left + 10, link_r.top + (link_r.bottom - link_r.top - art_ic_back.h) / 2,
                C_DIM, 255);
        UI_FLUSH();
        text(base_dc, f_small, C_TEXT, link_r.left + 34, link_r.top, link_r.right - 8, link_r.bottom,
             L"Restaurar o original", DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    }

    /* caixa do registro (o texto entra a cada quadro) */
    rr(px, MARGIN, LOG_Y, W - 2 * MARGIN, H - LOG_Y - 12, 14, C_LOGBG, 0, G_SOLID, 215, 0);
    rr(px, MARGIN, LOG_Y, W - 2 * MARGIN, H - LOG_Y - 12, 14, C_BORDER, 0, G_SOLID, 255, 1);
    blit_a8(px, &art_ic_term, MARGIN + 14, LOG_Y + 9, C_ACCENT, 255);
    UI_FLUSH();
    text(base_dc, f_small, C_DIM, MARGIN + 38, LOG_Y + 6, W / 2, LOG_Y + 28, L"Registro", DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    UI_FLUSH();
}

/* ------------------------------------------------------------ partes que mudam */

static void draw_progress(const ui_state_t *s)
{
    u16 *px = frame_px;
    WCHAR b[120];
    const WCHAR *hint = L"";
    u32 col = C_TEXT;
    int bar = 0, fill = 0, bw = W - 2 * MARGIN;
    if (s->status) {                                /* instalando, restaurando, pelo cartão */
        wcsncpy(b, s->status, 119);
        b[119] = 0;
        col = s->busy ? C_TEXT : C_DIM;
        goto draw;
    }
    switch (s->cd_total_ms || s->autoboot != 1 ? s->autoboot : 4) {
    case 4:                                         /* antes da contagem: conferindo o cartão */
        _snwprintf(b, 119, L"Preparando o início do BigFoston...");
        hint = L"Toque em Windows CE para ficar";
        break;
    case 1: {
        DWORD left = s->cd_total_ms > s->cd_elapsed_ms ? s->cd_total_ms - s->cd_elapsed_ms : 0;
        unsigned sec = (unsigned)((left + 999) / 1000);
        _snwprintf(b, 119, L"Iniciando o BigFoston em %u s", sec);
        hint = L"Toque em Windows CE para ficar";
        bar = 1;
        fill = s->cd_total_ms ? (int)((long long)bw * (long long)s->cd_elapsed_ms / (long long)s->cd_total_ms) : bw;
        break;
    }
    case 2:
        _snwprintf(b, 119, L"Início automático cancelado");
        hint = s->linux_ok ? L"Escolha um sistema acima" : L"";
        col = C_DIM;
        break;
    case 3:
        _snwprintf(b, 119, L"Iniciando o BigFoston...");
        hint = L"O reset volta ao Windows CE";
        bar = 1;
        fill = bw;
        break;
    default:
        if (s->requests == 1)
            _snwprintf(b, 119, L"Conectado ao PC  ·  1 pedido");
        else if (s->requests)
            _snwprintf(b, 119, L"Conectado ao PC  ·  %lu pedidos", (unsigned long)s->requests);
        else if (s->port && s->port[0])
            _snwprintf(b, 119, L"Pronto na porta %ls  ·  aguardando o PC", s->port);
        else
            _snwprintf(b, 119, L"Escolha um sistema");
        col = C_DIM;
        break;
    }
    b[119] = 0;
draw:
    if (fill > bw)
        fill = bw;
    blit_a8(px, &art_ic_power, MARGIN, PROG_Y + 3, s->autoboot == 1 || s->autoboot == 3 ? C_ACCENT : C_DIM, 255);
    if (bar) {
        rr(px, MARGIN, PROG_Y + 34, bw, 12, 6, C_SURF2, 0, G_SOLID, 255, 0);
        if (fill > 0) {
            /* o degradê é o da barra inteira: cresce revelando roxo -> verde-água */
            int x;
            u16 *row;
            int y, r = 6;
            for (y = PROG_Y + 34; y < PROG_Y + 46; y++) {
                row = px + y * stride;
                for (x = MARGIN; x < MARGIN + fill; x++) {
                    int a = rr_cov(x, y, MARGIN, PROG_Y + 34, MARGIN + (fill < 12 ? 12 : fill), PROG_Y + 46, r);
                    if (a)
                        row[x] = mix(row[x], lerp(C_PRIMARY, C_ACCENT, x - MARGIN, bw - 1), a);
                }
            }
        }
    }
    UI_FLUSH();
    text(frame_dc, f_bold, col, MARGIN + 28, PROG_Y, hint[0] ? W / 2 + 120 : W - MARGIN, PROG_Y + 26, b,
         DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    text(frame_dc, f_small, C_DIM, W / 2, PROG_Y, W - MARGIN, PROG_Y + 26, hint, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
}

static void draw_log(const ui_state_t *s)
{
    static const art_t *const icons[] = { &art_ic_info, &art_ic_check, &art_ic_warn, &art_ic_warn, &art_ic_usb };
    static const u32 colors[] = { C_DIM, C_OK, C_WARN, C_ERR, C_PC };
    int top = LOG_Y + 32, bottom = H - 12 - 8, tx = MARGIN + 38, tw = W - MARGIN - 14 - tx;
    int i, y = bottom, hgt[UI_LINES], first;
    if (s->nlines <= 0)
        return;
    /* da mais nova (embaixo) para cima, enquanto couber; linhas longas quebram */
    for (first = s->nlines - 1; first >= 0; first--) {
        hgt[first] = text_height(frame_dc, f_small, tw, s->lines[first].text);
        if (hgt[first] < 18)
            hgt[first] = 18;
        if (y - hgt[first] < top)
            break;
        y -= hgt[first] + 2;
    }
    first++;
    y = top;                                        /* como um terminal: de cima para baixo */
    for (i = first; i < s->nlines; i++) {
        int k = s->lines[i].kind;
        int newest = i == s->nlines - 1;
        if (k < 0 || k > UI_L_PC)
            k = UI_L_INFO;
        blit_a8(frame_px, icons[k], MARGIN + 14, y + 1, colors[k], newest ? 255 : 170);
        UI_FLUSH();
        text(frame_dc, f_small, newest ? C_TEXT : C_DIM, tx, y, tx + tw, y + hgt[i], s->lines[i].text,
             DT_LEFT | DT_TOP | DT_WORDBREAK);
        y += hgt[i] + 2;
    }
}

/* ------------------------------------------------------------ confirmação */

static void draw_overlay(const ui_state_t *s)
{
    u16 *px = frame_px;
    const WCHAR *title, *body, *ok, *cancel = L"Cancelar";
    int x, y, danger = 0, w = ov_card.right - ov_card.left, h = ov_card.bottom - ov_card.top;
    for (y = 0; y < H; y++)                          /* escurece a tela inteira */
        for (x = 0; x < W; x++)
            px[y * stride + x] = mix(px[y * stride + x], 0x000000, 150);
    switch (s->overlay) {
    case UI_OV_INSTALL:
        title = L"Instalar o Dual Boot?";
        body = L"Grava o BigFoston OS na memória interna (pasta BigFoston, sem tocar no Windows CE) e faz "
               L"esta tela abrir sozinha ao ligar. Os valores originais ficam guardados no cartão para "
               L"desfazer depois. Não desligue o aparelho até terminar.";
        ok = L"Instalar";
        break;
    case UI_OV_RESTORE:
        title = L"Restaurar o FOSTON original?";
        body = L"Desliga o início automático, apaga a pasta BigFoston da memória interna e devolve os "
               L"valores guardados. Os arquivos do cartão continuam lá para instalar de novo quando quiser.";
        ok = L"Restaurar";
        danger = 1;
        break;
    default:
        title = s->ov_title ? s->ov_title : L"";
        body = s->ov_body ? s->ov_body : L"";
        ok = L"OK";
        cancel = NULL;
        break;
    }
    rr(px, ov_card.left + 2, ov_card.top + 8, w, h, 22, 0x000000, 0, G_SOLID, 90, 0);
    rr(px, ov_card.left, ov_card.top, w, h, 22, C_SURF, 0, G_SOLID, 255, 0);
    rr(px, ov_card.left, ov_card.top, w, h, 22, danger ? C_ERR : C_PRIMARY, danger ? C_ERR : C_ACCENT, G_HORZ, 255, 2);
    UI_FLUSH();
    text(frame_dc, f_bold, C_TEXT, ov_card.left + 28, ov_card.top + 22, ov_card.right - 28, ov_card.top + 50, title,
         DT_LEFT | DT_TOP | DT_SINGLELINE);
    text(frame_dc, f_text, C_DIM, ov_card.left + 28, ov_card.top + 60, ov_card.right - 28, ov_card.bottom - 84, body,
         DT_LEFT | DT_TOP | DT_WORDBREAK);
    UI_FLUSH();
    if (cancel)
        pill(px, frame_dc, ov_cancel.left, ov_cancel.top, ov_cancel.right - ov_cancel.left, ov_cancel.bottom - ov_cancel.top,
             0, 1, s->pressed == UI_HIT_OV_CANCEL, &art_ic_back, cancel);
    UI_FLUSH();
    if (danger) {
        rr(px, ov_ok.left, ov_ok.top, ov_ok.right - ov_ok.left, ov_ok.bottom - ov_ok.top, 25,
           s->pressed == UI_HIT_OV_OK ? 0xa8323a : C_ERR, 0, G_SOLID, 255, 0);
        UI_FLUSH();
        text(frame_dc, f_bold, C_TEXT, ov_ok.left, ov_ok.top, ov_ok.right, ov_ok.bottom, ok,
             DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    } else
        pill(px, frame_dc, ov_ok.left, ov_ok.top, ov_ok.right - ov_ok.left, ov_ok.bottom - ov_ok.top, 1, 1,
             s->pressed == UI_HIT_OV_OK, cancel ? &art_ic_power : &art_ic_check, ok);
}

/* ------------------------------------------------------------ interface */

static HBITMAP dib(HDC screen, u16 **bits)
{
    struct { BITMAPINFOHEADER h; DWORD masks[3]; } bi;
    void *p = NULL;
    HBITMAP b;
    memset(&bi, 0, sizeof(bi));
    bi.h.biSize = sizeof(bi.h);
    bi.h.biWidth = W;
    bi.h.biHeight = -H;                     /* de cima para baixo (como o cmd_shot) */
    bi.h.biPlanes = 1;
    bi.h.biBitCount = 16;
    bi.h.biCompression = BI_BITFIELDS;
    bi.masks[0] = 0xF800;
    bi.masks[1] = 0x07E0;
    bi.masks[2] = 0x001F;
    b = CreateDIBSection(screen, (BITMAPINFO *)&bi, DIB_RGB_COLORS, &p, NULL, 0);
    *bits = (u16 *)p;
    return b;
}

int ui_init(HDC screen, int width, int height)
{
    int cw;
    if (base_dc)
        return 1;
    W = width;
    H = height;
    stride = ((W * 2 + 3) & ~3) / 2;
    bg = (u16 *)malloc((size_t)stride * H * 2);
    base_bmp = dib(screen, &base_px);
    frame_bmp = dib(screen, &frame_px);
    base_dc = CreateCompatibleDC(screen);
    frame_dc = CreateCompatibleDC(screen);
    if (!bg || !base_bmp || !frame_bmp || !base_px || !frame_px || !base_dc || !frame_dc) {
        ui_free();
        return 0;
    }
    base_old = (HBITMAP)SelectObject(base_dc, base_bmp);
    frame_old = (HBITMAP)SelectObject(frame_dc, frame_bmp);
    f_small = font(15, FW_NORMAL);
    f_text = font(17, FW_NORMAL);
    f_bold = font(18, FW_BOLD);
    cw = (W - 3 * MARGIN) / 2;
    SetRect(&card_l, MARGIN, CARD_Y, MARGIN + cw, CARD_Y + CARD_H);
    SetRect(&card_r, W - MARGIN - cw, CARD_Y, W - MARGIN, CARD_Y + CARD_H);
    SetRect(&link_r, W - 330, 11, W - 150, TOP_H - 13);
    SetRect(&ov_card, (W - 560) / 2, (H - 280) / 2, (W + 560) / 2, (H + 280) / 2);
    SetRect(&ov_cancel, ov_card.left + 24, ov_card.bottom - 74, (ov_card.left + ov_card.right) / 2 - 8, ov_card.bottom - 24);
    SetRect(&ov_ok, (ov_card.left + ov_card.right) / 2 + 8, ov_card.bottom - 74, ov_card.right - 24, ov_card.bottom - 24);
    make_bg();
    base_key = -1;
    return 1;
}

void ui_free(void)
{
    if (base_dc) { if (base_old) SelectObject(base_dc, base_old); DeleteDC(base_dc); }
    if (frame_dc) { if (frame_old) SelectObject(frame_dc, frame_old); DeleteDC(frame_dc); }
    if (base_bmp) DeleteObject(base_bmp);
    if (frame_bmp) DeleteObject(frame_bmp);
    if (f_small) DeleteObject(f_small);
    if (f_text) DeleteObject(f_text);
    if (f_bold) DeleteObject(f_bold);
    free(bg);
    bg = NULL;
    base_dc = frame_dc = NULL;
    base_bmp = frame_bmp = base_old = frame_old = NULL;
    f_small = f_text = f_bold = NULL;
    base_px = frame_px = NULL;
    base_key = -1;
}

void ui_paint(HDC dst, const ui_state_t *s, const RECT *clip)
{
    int key;
    RECT all;
    if (!base_dc)
        return;
    /* a base (fundo, barra, cartões) só muda com o estado dos botões */
    key = (s->autoboot & 7) | (s->linux_ok ? 8 : 0) | (s->pressed << 4) | (s->setup << 8) |
          (s->can_restore ? 1 << 10 : 0) | (s->busy ? 1 << 11 : 0);
    if (key != base_key) {
        build_base(s);
        base_key = key;
    }
    memcpy(frame_px, base_px, (size_t)stride * H * 2);
    draw_progress(s);
    draw_log(s);
    if (s->overlay)
        draw_overlay(s);
    UI_FLUSH();
    SetRect(&all, 0, 0, W, H);
    if (!clip)
        clip = &all;
    BitBlt(dst, clip->left, clip->top, clip->right - clip->left, clip->bottom - clip->top, frame_dc, clip->left,
           clip->top, SRCCOPY);
}

int ui_hit_overlay(int x, int y)
{
    POINT p;
    p.x = x;
    p.y = y;
    if (PtInRect(&ov_ok, p))
        return UI_HIT_OV_OK;
    if (PtInRect(&ov_cancel, p))
        return UI_HIT_OV_CANCEL;
    return UI_HIT_NONE;
}

int ui_hit(int x, int y)
{
    POINT p;
    p.x = x;
    p.y = y;
    if (PtInRect(&link_r, p))
        return UI_HIT_RESTORE;
    if (PtInRect(&card_l, p))
        return UI_HIT_LINUX;
    if (PtInRect(&card_r, p))
        return UI_HIT_CE;
    return UI_HIT_NONE;
}

void ui_progress_rect(RECT *r)
{
    SetRect(r, 0, PROG_Y, W, PROG_Y + 50);
}
