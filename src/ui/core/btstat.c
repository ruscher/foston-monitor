/* btstat.c — ver btstat.h. */
#define _DEFAULT_SOURCE
#include "btstat.h"
#include "theme.h"
#include "../widgets/dialog.h"
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static const char *state_path(void)
{
    const char *e = getenv("BIGFOSTON_BT_STATUS");
    return e && *e ? e : "/run/bigfoston-bt";
}

/* copia o campo até '|' (ou fim) e avança */
static const char *field(const char *s, char *out, size_t n)
{
    size_t k = 0;
    while(*s && *s != '|') { if(k + 1 < n) out[k++] = *s; s++; }
    out[k] = 0;
    return *s == '|' ? s + 1 : s;
}

void bt_read(bt_info_t *b)
{
    memset(b, 0, sizeof *b);
    FILE *f = fopen(state_path(), "r");
    if(!f) return;
    char line[512], t[96];
    long updated = 0;
    while(fgets(line, sizeof line, f)) {
        line[strcspn(line, "\n")] = 0;
        char *v = strchr(line, '=');
        if(!v) continue;
        *v++ = 0;
        if(!strcmp(line, "updated")) updated = atol(v);
        else if(!strcmp(line, "enabled")) b->enabled = atoi(v);
        else if(!strcmp(line, "adapter")) b->adapter = atoi(v);
        else if(!strcmp(line, "powered")) b->powered = atoi(v);
        else if(!strcmp(line, "discoverable")) b->discoverable = atoi(v);
        else if(!strcmp(line, "discovering")) b->discovering = atoi(v);
        else if(!strcmp(line, "name")) snprintf(b->name, sizeof b->name, "%s", v);
        else if(!strcmp(line, "dev") && b->ndev < BT_MAXDEV) {
            bt_dev_t *d = &b->dev[b->ndev++];
            const char *s = field(v, d->addr, sizeof d->addr);
            s = field(s, t, sizeof t); d->paired = atoi(t);
            s = field(s, t, sizeof t); d->connected = atoi(t);
            s = field(s, t, sizeof t); d->trusted = atoi(t);
            s = field(s, t, sizeof t); d->rssi = atoi(t);
            s = field(s, d->icon, sizeof d->icon);
            field(s, d->name, sizeof d->name);
        } else if(!strcmp(line, "req")) {
            b->req = true;
            const char *s = field(v, b->req_kind, sizeof b->req_kind);
            s = field(s, b->req_code, sizeof b->req_code);
            s = field(s, b->req_addr, sizeof b->req_addr);
            field(s, b->req_name, sizeof b->req_name);
        } else if(!strcmp(line, "player")) {
            b->player = true;
            const char *s = field(v, b->pl_status, sizeof b->pl_status);
            s = field(s, b->pl_title, sizeof b->pl_title);
            s = field(s, b->pl_artist, sizeof b->pl_artist);
            s = field(s, b->pl_dev, sizeof b->pl_dev);
            s = field(s, b->pl_album, sizeof b->pl_album);
            s = field(s, t, sizeof t); b->pl_dur = (unsigned)strtoul(t, NULL, 10);
            s = field(s, t, sizeof t); b->pl_pos = (unsigned)strtoul(t, NULL, 10);
            s = field(s, t, sizeof t); b->pl_pos_at = atol(t);
            s = field(s, b->pl_repeat, sizeof b->pl_repeat);
            s = field(s, b->pl_shuffle, sizeof b->pl_shuffle);
            s = field(s, b->pl_genre, sizeof b->pl_genre);
            s = field(s, t, sizeof t); b->pl_trackno = (unsigned)strtoul(t, NULL, 10);
            field(s, t, sizeof t); b->pl_ntracks = (unsigned)strtoul(t, NULL, 10);
        } else if(!strcmp(line, "stream")) {
            b->streaming = !strcmp(v, "active");
        } else if(!strcmp(line, "result")) {
            const char *s = field(v, b->res_op, sizeof b->res_op);
            s = field(s, b->res_state, sizeof b->res_state);
            field(s, b->res_err, sizeof b->res_err);
        }
    }
    fclose(f);
    /* o agente reescreve pelo menos a cada 10 s; no simulador o arquivo é fixo */
    b->available = getenv("BIGFOSTON_BT_STATUS") || labs((long)time(NULL) - updated) < 30;
}

unsigned bt_position_ms(const bt_info_t *b)
{
    unsigned p = b->pl_pos;
    if(!strcmp(b->pl_status, "playing") && b->pl_pos_at > 0) {
        struct timespec t;
        clock_gettime(CLOCK_MONOTONIC, &t);
        long now = t.tv_sec * 1000L + t.tv_nsec / 1000000L, d = now - b->pl_pos_at;
        if(d > 0) p += (unsigned)d;
    }
    if(b->pl_dur && p > b->pl_dur) p = b->pl_dur;
    return p;
}

bool bt_cmd(const char *fmt, ...)
{
    char buf[128];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof buf - 1, fmt, ap);
    va_end(ap);
    if(n <= 0 || n >= (int)sizeof buf - 1) return false;
    buf[n++] = '\n';
    char fifo[300];
    snprintf(fifo, sizeof fifo, "%s.cmd", state_path());
    /* O_NONBLOCK: sem o agente lendo, falha em vez de travar a interface */
    int fd = open(fifo, O_WRONLY | O_NONBLOCK | O_CLOEXEC);
    if(fd < 0) return false;
    bool ok = write(fd, buf, (size_t)n) == n;
    close(fd);
    return ok;
}

/* ------------------------------------------------ diálogo de pareamento */

static lv_obj_t *pair_bd;                 /* fundo do diálogo aberto */
static char pair_key[32];                 /* código + aparelho do diálogo aberto */

static void pair_deleted(lv_event_t *e) { (void)e; pair_bd = NULL; }

static void pair_answer(int btn, void *ud)
{
    (void)ud;
    bt_cmd(btn == 1 ? "accept" : "reject");
}

void bt_poll_global(void)
{
    if(access(state_path(), R_OK)) return;
    bt_info_t b;
    bt_read(&b);
    char key[32];
    snprintf(key, sizeof key, "%s%s", b.req ? b.req_code : "", b.req ? b.req_addr : "");
    if(pair_bd && (!b.req || strcmp(key, pair_key))) {   /* cancelado ou trocado */
        lv_obj_delete(pair_bd);
        pair_bd = NULL;
    }
    if(!b.req || pair_bd) return;
    snprintf(pair_key, sizeof pair_key, "%s", key);
    const char *who = b.req_name[0] ? b.req_name : b.req_addr;
    char title[96], body[256];
    if(!strcmp(b.req_kind, "confirm")) {
        snprintf(title, sizeof title, tr(S_BT_PAIR_Q), who);
        snprintf(body, sizeof body, "%s", tr(S_BT_PAIR_CODE));
    } else {
        snprintf(title, sizeof title, tr(S_BT_AUTH_Q), who);
        snprintf(body, sizeof body, "%s", tr(S_BT_AUTH_BODY));
    }
    const char *btns[] = { tr(S_BT_REJECT), tr(S_BT_ACCEPT) };
    lv_obj_t *card = dialog_open(LV_SYMBOL_BLUETOOTH, lv_color_hex(0x3584e4), title, body, btns, 2, 1, -1,
                                 pair_answer, NULL);
    pair_bd = lv_obj_get_parent(card);
    lv_obj_add_event_cb(pair_bd, pair_deleted, LV_EVENT_DELETE, NULL);
    /* o código em letras grandes: é o que o usuário compara com o celular */
    if(!strcmp(b.req_kind, "confirm")) {
        lv_obj_t *code = bf_label(card, b.req_code, BF_FONT_CLOCK, pal()->text);
        lv_obj_move_to_index(code, 2);
        lv_obj_set_style_text_letter_space(code, 6, 0);
        lv_obj_set_width(code, LV_PCT(100));
        lv_obj_set_style_text_align(code, LV_TEXT_ALIGN_CENTER, 0);
    }
}
