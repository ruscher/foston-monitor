/* fmstat.c — ver fmstat.h. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "fmstat.h"
#include "settings.h"
#include "status.h"
#include "i18n.h"
#include "nav.h"
#include "theme.h"
#include "../widgets/dialog.h"
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#define BFFM "/usr/sbin/bffm"

static const char *state_path(void)
{
    const char *e = getenv("BIGFOSTON_FM_STATUS");
    return e && *e ? e : "/run/bigfoston-fm";
}

static bool sim(void) { return getenv("BIGFOSTON_FM_STATUS") != NULL; }

bool fm_available(void) { return sim() || access(BFFM, X_OK) == 0; }

bool fm_freq_valid(int f) { return f >= FM_MIN && f <= FM_MAX; }

void fm_format(int f, char *out, size_t n)
{
    char sep = i18n_get_lang() == LANG_EN ? '.' : ',';
    snprintf(out, n, "%d%c%d", f / 10, sep, f % 10);
}

/* ------------------------------------------------------------ comando em segundo plano */

static pid_t cmd_pid, tone_pid;
static int cmd_fd = -1;
static char result[96];
static bool starting;                     /* "on" em andamento: estado Inicializando */

bool fm_busy(void) { return cmd_pid > 0; }
const char *fm_last_result(void) { return result; }

static void reap(void)
{
    if(cmd_pid > 0) {
        int st;
        if(waitpid(cmd_pid, &st, WNOHANG) == cmd_pid) {
            char b[96] = "";
            ssize_t k = cmd_fd >= 0 ? read(cmd_fd, b, sizeof b - 1) : 0;
            b[k > 0 ? k : 0] = 0;
            b[strcspn(b, "\n")] = 0;
            snprintf(result, sizeof result, "%s", b[0] ? b : "erro");
            if(cmd_fd >= 0) close(cmd_fd);
            cmd_fd = -1;
            cmd_pid = 0;
            starting = false;
        }
    }
    if(tone_pid > 0 && waitpid(tone_pid, NULL, WNOHANG) == tone_pid) tone_pid = 0;
}

/* simulador: escreve o estado como o bffm escreveria */
static void sim_state(const char *st, int f, bool mute)
{
    FILE *o = fopen(state_path(), "w");
    if(!o) return;
    fprintf(o, "state=%s\nfreq=%d.%d\nchip=KT0801A\nspeaker_off=%d\nsince=%ld\n", st, f / 10, f % 10, mute,
            (long)time(NULL) - 754);
    fclose(o);
}

static bool run(char *const argv[])
{
    reap();
    if(cmd_pid > 0) return false;
    int pfd[2];
    if(pipe2(pfd, O_CLOEXEC) < 0) return false;
    pid_t p = fork();
    if(p == 0) {
        dup2(pfd[1], 1);
        int nul = open("/dev/null", O_RDWR);
        if(nul >= 0) dup2(nul, 2);
        execv(BFFM, argv);
        _exit(127);
    }
    close(pfd[1]);
    if(p < 0) { close(pfd[0]); return false; }
    cmd_pid = p;
    cmd_fd = pfd[0];
    result[0] = 0;
    return true;
}

static time_t off_at;
time_t fm_off_at(void) { return off_at; }

bool fm_on(int f, int mode)
{
    if(!fm_freq_valid(f)) return false;
    char fs[16];
    snprintf(fs, sizeof fs, "%d.%d", f / 10, f % 10);
    fm_conf_t *c = fm_conf();
    off_at = c->timer_min > 0 ? time(NULL) + c->timer_min * 60 : 0;
    if(sim()) { sim_state("transmitindo", f, mode == 0); snprintf(result, sizeof result, "ok %s", fs); return true; }
    char *av[] = { "bffm", "on", fs, mode ? "ambos" : "fm", NULL };
    starting = run(av);
    return starting;
}

bool fm_off(void)
{
    off_at = 0;
    if(sim()) { sim_state("desligado", 0, false); snprintf(result, sizeof result, "ok"); return true; }
    char *av[] = { "bffm", "off", NULL };
    if(cmd_pid > 0) {                     /* desligar tem prioridade: espera o comando atual */
        for(int i = 0; i < 40 && cmd_pid > 0; i++) { usleep(50000); reap(); }
    }
    return run(av);
}

bool fm_set_freq(int f)
{
    if(!fm_freq_valid(f)) return false;
    char fs[16];
    snprintf(fs, sizeof fs, "%d.%d", f / 10, f % 10);
    if(sim()) { fm_info_t i; fm_read(&i); sim_state("transmitindo", f, i.speaker_off); return true; }
    char *av[] = { "bffm", "freq", fs, NULL };
    return run(av);
}

bool fm_set_mode(int mode)
{
    if(sim()) { fm_info_t i; fm_read(&i); if(i.state == FM_ON) sim_state("transmitindo", i.freq, mode == 0); return true; }
    char *av[] = { "bffm", "saida", mode ? "ambos" : "fm", NULL };
    return run(av);
}

bool fm_tone(int secs)
{
    reap();
    if(tone_pid > 0 || sim()) return false;
    char s[8];
    snprintf(s, sizeof s, "%d", secs);
    pid_t p = fork();
    if(p == 0) {
        int nul = open("/dev/null", O_RDWR);
        if(nul >= 0) { dup2(nul, 1); dup2(nul, 2); }
        execl(BFFM, "bffm", "tom", s, (char *)NULL);
        _exit(127);
    }
    if(p > 0) tone_pid = p;
    return p > 0;
}

void fm_tone_stop(void) { if(tone_pid > 0) kill(tone_pid, SIGTERM); }
bool fm_tone_running(void) { reap(); return tone_pid > 0; }

/* ------------------------------------------------------------ estado */

void fm_read(fm_info_t *f)
{
    memset(f, 0, sizeof *f);
    reap();
    if(!fm_available()) { f->state = FM_UNAVAILABLE; return; }
    FILE *o = fopen(state_path(), "r");
    char st[32] = "desligado";
    if(o) {
        char l[128];
        while(fgets(l, sizeof l, o)) {
            l[strcspn(l, "\n")] = 0;
            char *v = strchr(l, '=');
            if(!v) continue;
            *v++ = 0;
            if(!strcmp(l, "state")) snprintf(st, sizeof st, "%s", v);
            else if(!strcmp(l, "freq")) { int a = 0, b = 0; sscanf(v, "%d.%d", &a, &b); f->freq = a * 10 + b; }
            else if(!strcmp(l, "speaker_off")) f->speaker_off = atoi(v);
            else if(!strcmp(l, "since")) f->since = (time_t)atol(v);
            else if(!strcmp(l, "error")) snprintf(f->error, sizeof f->error, "%s", v);
        }
        fclose(o);
    }
    if(!strcmp(st, "transmitindo")) f->state = FM_ON;
    else if(!strcmp(st, "ligando") || starting) f->state = FM_STARTING;
    else if(!strcmp(st, "erro")) f->state = FM_ERROR;
    else f->state = FM_OFF;
}

/* ------------------------------------------------------------ configuração */

static fm_conf_t conf;
static bool conf_loaded;

fm_conf_t *fm_conf(void)
{
    if(conf_loaded) return &conf;
    conf_loaded = true;
    memset(&conf, 0, sizeof conf);
    conf.freq = 881;
    char p[300];
    data_path(p, sizeof p, "fm.conf");
    FILE *o = fopen(p, "r");
    if(!o) return &conf;
    char l[128];
    while(fgets(l, sizeof l, o)) {
        l[strcspn(l, "\n")] = 0;
        if(!strncmp(l, "freq=", 5)) { int f = atoi(l + 5); if(fm_freq_valid(f)) conf.freq = f; }
        else if(!strncmp(l, "mode=", 5)) conf.mode = atoi(l + 5) ? 1 : 0;
        else if(!strncmp(l, "timer=", 6)) conf.timer_min = atoi(l + 6);
        else if(!strncmp(l, "help=", 5)) conf.help_seen = atoi(l + 5);
        else if(!strncmp(l, "fav=", 4) && conf.nfav < FM_MAXFAV) {
            char *bar = strchr(l + 4, '|');
            if(!bar) continue;
            *bar = 0;
            fm_fav_t *v = &conf.fav[conf.nfav];
            v->freq = atoi(l + 4);
            snprintf(v->name, sizeof v->name, "%s", bar + 1);
            for(char *c = v->name; *c; c++) if(*c == '|') *c = ' ';
            conf.nfav++;                  /* inválida fica na lista (avisada), não é usada */
        }
    }
    fclose(o);
    return &conf;
}

void fm_conf_save(void)
{
    char p[300], t[310];
    data_path(p, sizeof p, "fm.conf");
    snprintf(t, sizeof t, "%s.tmp", p);
    FILE *o = fopen(t, "w");
    if(!o) return;
    fprintf(o, "freq=%d\nmode=%d\ntimer=%d\nhelp=%d\n", conf.freq, conf.mode, conf.timer_min, conf.help_seen);
    for(int i = 0; i < conf.nfav; i++) fprintf(o, "fav=%d|%s\n", conf.fav[i].freq, conf.fav[i].name);
    fclose(o);
    rename(t, p);
}

/* ------------------------------------------------------------ temporizador e bateria */

static bool bat_warned;

static void bat_answer(int b, void *ud) { (void)ud; if(b == 1) fm_off(); }

void fm_poll_global(void)
{
    reap();
    if(!off_at && !fm_available()) return;
    fm_info_t f;
    fm_read(&f);
    if(f.state != FM_ON) { bat_warned = false; if(f.state == FM_OFF) off_at = 0; return; }
    if(off_at && time(NULL) >= off_at) { fm_off(); return; }      /* temporizador */
    const bf_status_t *s = status_get();
    if(!bat_warned && s->battery_pct >= 0 && s->battery_pct <= 10 && s->battery_state == BF_BAT_DISCHARGING) {
        bat_warned = true;
        const char *btns[] = { tr(S_FM_KEEP), tr(S_FM_TURN_OFF) };
        dialog_open(LV_SYMBOL_BATTERY_1, pal()->warning, tr(S_FM_BAT_TITLE), tr(S_FM_BAT_BODY), btns, 2, 1, -1,
                    bat_answer, NULL);
    }
}
