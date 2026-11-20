/* scr_ssh.c — SSH: estado real (porta 22 em /proc/net/tcp*) + endereço. */
#include "screens.h"
#include "../core/theme.h"
#include "../core/sysinfo.h"
#include "../widgets/page.h"
#include <stdio.h>

static void create(lv_obj_t *parent, int arg)
{
    (void)arg;
    const bf_palette_t *p = pal();
    lv_obj_t *pg = page_create(parent);
    page_header(pg, BF_SYMBOL_KEY, p->accent, tr(S_SET_SSH), tr(S_SUB_SSH));
    lv_obj_t *c = bf_card(pg);
    lv_obj_set_size(c, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(c, BF_SP_M, 0);
    bf_tri_t st = sysinfo_ssh_listening();
    lv_obj_t *v = page_kv(c, tr(S_SSH_STATE), st == BF_YES ? tr(S_SSH_ACTIVE) :
                          st == BF_NO ? tr(S_SSH_INACTIVE) : tr(S_UNKNOWN));
    lv_obj_set_style_text_color(v, st == BF_YES ? p->success : p->text_dim, 0);
    char ifn[32], ip[48], mac[24], b[96];
    sysinfo_net(ifn, sizeof ifn, ip, sizeof ip, mac, sizeof mac);
    if(ip[0]) snprintf(b, sizeof b, "ssh %s", ip);
    else snprintf(b, sizeof b, "%s", tr(S_NO_NETWORK));
    lv_obj_t *cmd = page_kv(c, tr(S_SI_IP), b);
    lv_obj_set_style_text_font(cmd, BF_FONT_MONO, 0);
    lv_obj_t *n = bf_label(pg, tr(S_SSH_TOGGLE), BF_FONT_S, p->text_dim);
    lv_obj_set_width(n, LV_PCT(100));
    lv_label_set_long_mode(n, LV_LABEL_LONG_WRAP);
}

const bf_screen_def_t scr_ssh_def = { create, NULL, S_SET_SSH, NULL };
