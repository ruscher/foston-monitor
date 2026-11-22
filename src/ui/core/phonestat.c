/* phonestat.c — ver phonestat.h. */
#define _DEFAULT_SOURCE
#include "phonestat.h"
#include "theme.h"
#include "nav.h"
#include "../widgets/dialog.h"
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static char last_data[96];

static const char *state_path(void)
{
    const char *e = getenv("BIGFOSTON_PHONE_STATUS");
    return e && *e ? e : "/run/bigfoston-phone";
}

static const char *field(const char *s, char *out, size_t n)
{
    size_t k = 0;
    while(*s && *s != '|') { if(k + 1 < n) out[k++] = *s; s++; }
    out[k] = 0;
    return *s == '|' ? s + 1 : s;
}

void ph_read(ph_info_t *p)
{
    memset(p, 0, sizeof *p);
    p->signal = p->battery = p->service = -1;
    p->voice_phone = true;
    snprintf(p->sync, sizeof p->sync, "idle");
    FILE *f = fopen(state_path(), "r");
    if(!f) return;
    p->available = true;
    char line[512], t[32];
    while(fgets(line, sizeof line, f)) {
        line[strcspn(line, "\n")] = 0;
        char *v = strchr(line, '=');
        if(!v) continue;
        *v++ = 0;
        if(!strcmp(line, "link")) p->link = atoi(v);
        else if(!strcmp(line, "dev")) snprintf(p->dev, sizeof p->dev, "%s", v);
        else if(!strcmp(line, "operator")) snprintf(p->op, sizeof p->op, "%s", v);
        else if(!strcmp(line, "data")) snprintf(p->data, sizeof p->data, "%s", v);
        else if(!strcmp(line, "signal")) p->signal = atoi(v);
        else if(!strcmp(line, "battery")) p->battery = atoi(v);
        else if(!strcmp(line, "service")) p->service = atoi(v);
        else if(!strcmp(line, "voice")) p->voice_phone = strcmp(v, "foston") != 0;
        else if(!strcmp(line, "sco")) p->sco = atoi(v);
        else if(!strcmp(line, "since")) p->since = (time_t)atol(v);
        else if(!strcmp(line, "sync")) snprintf(p->sync, sizeof p->sync, "%s", v);
        else if(!strcmp(line, "call") && p->ncall < PH_MAXCALLS) {
            ph_call_t *c = &p->call[p->ncall++];
            const char *s = field(v, t, sizeof t); c->idx = atoi(t);
            s = field(s, t, sizeof t); c->dir = atoi(t);
            s = field(s, t, sizeof t); c->stat = atoi(t);
            s = field(s, c->num, sizeof c->num);
            field(s, c->name, sizeof c->name);
        }
    }
    fclose(f);
    if(p->data[0]) snprintf(last_data, sizeof last_data, "%s", p->data);
}

const char *ph_last_data(void) { return last_data; }

bool ph_cmd(const char *fmt, ...)
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
    int fd = open(fifo, O_WRONLY | O_NONBLOCK | O_CLOEXEC);
    if(fd < 0) return false;
    bool ok = write(fd, buf, (size_t)n) == n;
    close(fd);
    return ok;
}

/* ------------------------------------------------ chamada recebida */

static lv_obj_t *ring_bd;
static char ring_key[64];

static void ring_deleted(lv_event_t *e) { (void)e; ring_bd = NULL; }

static void ring_answer(int btn, void *ud)
{
    (void)ud;
    if(btn == 1) { ph_cmd("answer"); nav_go(SCR_PHONE, 0); }
    else if(btn == 0) ph_cmd("hangup");
}

void ph_poll_global(void)
{
    if(access(state_path(), R_OK)) return;
    ph_info_t p;
    ph_read(&p);
    const ph_call_t *in = NULL;
    for(int i = 0; i < p.ncall; i++) if(p.call[i].stat == PH_INCOMING) in = &p.call[i];
    char key[64] = "";
    if(in) snprintf(key, sizeof key, "%d%s", in->idx, in->num);
    if(ring_bd && (!in || strcmp(key, ring_key))) { lv_obj_delete(ring_bd); ring_bd = NULL; }
    /* na tela do Telefone a chamada já aparece com os botões */
    if(!in || ring_bd || nav_current().scr == SCR_PHONE) return;
    snprintf(ring_key, sizeof ring_key, "%s", key);
    char title[160];
    const char *who = in->name[0] ? in->name : in->num[0] ? in->num : tr(S_PH_UNKNOWN);
    snprintf(title, sizeof title, tr(S_PH_INCOMING_T), who);
    const char *btns[] = { tr(S_PH_REJECT), tr(S_PH_ANSWER) };
    lv_obj_t *card = dialog_open(LV_SYMBOL_CALL, pal()->success, title,
                                 in->name[0] && in->num[0] ? in->num : tr(S_PH_VOICE_NOTE),
                                 btns, 2, 1, 0, ring_answer, NULL);
    ring_bd = lv_obj_get_parent(card);
    lv_obj_add_event_cb(ring_bd, ring_deleted, LV_EVENT_DELETE, NULL);
}
