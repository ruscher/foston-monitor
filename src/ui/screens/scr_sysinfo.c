/* scr_sysinfo.c — "fastfetch" gráfico: logotipo ASCII + dados reais do sistema. */
#define _DEFAULT_SOURCE
#include "screens.h"
#include "../core/theme.h"
#include "../core/sysinfo.h"
#include "../core/settings.h"
#include "../core/app.h"
#include <stdio.h>
#include <string.h>
#include <sys/utsname.h>

#ifndef BIGFOSTON_VERSION
#define BIGFOSTON_VERSION "0.6.5"
#endif

static const char *const logo[] = {
    " ____  _       ",
    "| __ )(_) __ _ ",
    "|  _ \\| |/ _` |",
    "| |_) | | (_| |",
    "|____/|_|\\__, |",
    "         |___/ ",
};

static lv_obj_t *add_line(lv_obj_t *col, const char *key, const char *val)
{
    const bf_palette_t *p = pal();
    lv_obj_t *row = bf_plain(col);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_t *k = bf_label(row, key, BF_FONT_MONO, p->primary);
    lv_obj_set_width(k, 150);
    lv_obj_t *v = bf_label(row, val, BF_FONT_MONO, p->text);
    bf_label_ellipsis(v, 386);
    return v;
}

static void create(lv_obj_t *parent, int arg)
{
    (void)arg;
    const bf_palette_t *p = pal();
    lv_obj_t *pg = bf_plain(parent);
    lv_obj_set_size(pg, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_pad_all(pg, BF_SP_L, 0);
    lv_obj_set_style_pad_left(pg, BF_SP_XL, 0);
    lv_obj_set_flex_flow(pg, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(pg, BF_SP_XL, 0);

    /* coluna esquerda: logotipo ASCII + paleta (como no fastfetch) */
    lv_obj_t *left = bf_plain(pg);
    lv_obj_set_size(left, 200, LV_PCT(100));
    lv_obj_set_flex_flow(left, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(left, 0, 0);
    lv_obj_set_style_pad_top(left, BF_SP_M, 0);
    for(unsigned i = 0; i < sizeof logo / sizeof logo[0]; i++)
        bf_label(left, logo[i], BF_FONT_MONO, i < 4 ? p->accent : p->primary);
    lv_obj_t *fos = bf_label(left, " F O S T O N", BF_FONT_MONO, p->text);
    lv_obj_set_style_margin_top(fos, 6, 0);
    lv_obj_t *ver = bf_label(left, " OS " BIGFOSTON_VERSION, BF_FONT_MONO, p->text_dim);
    (void)ver;
    lv_obj_t *sw = bf_plain(left);
    lv_obj_set_size(sw, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_margin_top(sw, BF_SP_L, 0);
    lv_obj_set_style_pad_left(sw, 8, 0);
    lv_obj_set_flex_flow(sw, LV_FLEX_FLOW_ROW);
    static const uint32_t sw_col[] = { 0x1b1f27, 0xe5484d, 0x2ec27e, 0xf5a524, 0x3584e4, 0x9b6ad6, 0x33c7de, 0xe9edf4 };
    for(unsigned i = 0; i < 8; i++) {
        lv_obj_t *b = lv_obj_create(sw);
        lv_obj_remove_style_all(b);
        lv_obj_set_size(b, 20, 20);
        lv_obj_set_style_bg_color(b, lv_color_hex(sw_col[i]), 0);
        lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
        if(i == 0 || i == 7) { lv_obj_set_style_border_width(b, 1, 0); lv_obj_set_style_border_color(b, p->border, 0); }
    }

    /* coluna direita: chave: valor */
    lv_obj_t *col = bf_plain(pg);
    lv_obj_set_height(col, LV_PCT(100));
    lv_obj_set_flex_grow(col, 1);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(col, 2, 0);
    bf_scrollable(col);

    char user[64], host[64], buf[192], a[32], b[32];
    sysinfo_user(user, sizeof user);
    sysinfo_hostname(host, sizeof host);
    lv_obj_t *h = lv_label_create(col);
    lv_label_set_text_fmt(h, "%s@%s", user, host);
    lv_obj_set_style_text_font(h, BF_FONT_MONO, 0);
    lv_obj_set_style_text_color(h, p->accent, 0);
    lv_obj_t *rule = lv_obj_create(col);
    lv_obj_remove_style_all(rule);
    lv_obj_set_size(rule, LV_PCT(100), 2);
    lv_obj_set_style_bg_color(rule, p->border, 0);
    lv_obj_set_style_bg_opa(rule, LV_OPA_COVER, 0);
    lv_obj_set_style_margin_ver(rule, 3, 0);

    add_line(col, tr(S_SI_OS), "BigFoston OS " BIGFOSTON_VERSION);
    sysinfo_os_pretty(buf, sizeof buf);
    add_line(col, tr(S_SI_BASE), buf);
    struct utsname u;
    if(uname(&u) == 0) {
        add_line(col, tr(S_SI_KERNEL), u.release);
        add_line(col, tr(S_SI_ARCH), u.machine);
    }
    int cores = 0;
    char cpu[160];
    sysinfo_cpu_model(cpu, sizeof cpu, &cores);
    snprintf(buf, sizeof buf, "%s x%d", cpu, cores);
    add_line(col, tr(S_SI_CPU), buf);

    unsigned long tot, av;
    if(sysinfo_mem(&tot, &av)) {
        fmt_bytes((unsigned long long)(tot - av) * 1024, a, sizeof a);
        fmt_bytes((unsigned long long)tot * 1024, b, sizeof b);
        snprintf(buf, sizeof buf, "%s / %s (%lu%%)", a, b, (tot - av) * 100 / tot);
    }
    else snprintf(buf, sizeof buf, "—");
    add_line(col, tr(S_SI_RAM), buf);

    unsigned long long dt, da;
    if(sysinfo_disk(data_dir(), &dt, &da)) {
        fmt_bytes(dt - da, a, sizeof a);
        fmt_bytes(dt, b, sizeof b);
        snprintf(buf, sizeof buf, "%s / %s (%s)", a, b, data_dir());
    }
    else snprintf(buf, sizeof buf, "—");
    add_line(col, tr(S_SI_DISK), buf);

    fmt_duration(sysinfo_uptime(), buf, sizeof buf);
    add_line(col, tr(S_SI_UPTIME), buf);

    lv_display_t *d = app_display();
    snprintf(buf, sizeof buf, "%dx%d, %d bpp", (int)lv_display_get_horizontal_resolution(d),
             (int)lv_display_get_vertical_resolution(d),
             (int)lv_color_format_get_bpp(lv_display_get_color_format(d)));
    add_line(col, tr(S_SI_RES), buf);
    snprintf(buf, sizeof buf, "bigfoston-shell, LVGL %d.%d.%d", LVGL_VERSION_MAJOR,
             LVGL_VERSION_MINOR, LVGL_VERSION_PATCH);
    add_line(col, tr(S_SI_UI), buf);

    char ifn[32], ip[48], mac[24];
    sysinfo_net(ifn, sizeof ifn, ip, sizeof ip, mac, sizeof mac);
    if(ip[0]) snprintf(buf, sizeof buf, "%s (%s)", ip, ifn);
    else snprintf(buf, sizeof buf, "%s", tr(S_NO_NETWORK));
    add_line(col, tr(S_SI_IP), buf);
    add_line(col, tr(S_SI_MAC), mac[0] ? mac : "—");

    bf_tri_t ssh = sysinfo_ssh_listening();
    lv_obj_t *sv = add_line(col, tr(S_SI_SSH), ssh == BF_YES ? tr(S_SSH_ACTIVE) :
                            ssh == BF_NO ? tr(S_SSH_INACTIVE) : tr(S_UNKNOWN));
    lv_obj_set_style_text_color(sv, ssh == BF_YES ? p->success : p->text_dim, 0);
}

const bf_screen_def_t scr_sysinfo_def = { create, NULL, S_SET_SYSINFO, NULL };
