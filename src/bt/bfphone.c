/* bfphone — telefone do BigFoston pelo celular conectado (HFP Hands-Free; docs/phone).
 *
 * O BlueALSA (-p hfp-hf) mantém a conexão de serviço HFP; o bfphone abre o canal AT dele
 * (org.bluealsa.RFCOMM1.Open) e:
 *  - acompanha as chamadas por AT+CLCC (a cada 5 s parado, 1 s com chamada, e logo depois de
 *    RING/+CIEV/+CLIP), os indicadores (sinal, bateria, serviço) e a operadora;
 *  - grava o estado em /run/bigfoston-phone (chave=valor, reescrito por rename):
 *      link=0|1  dev=nome  operator=  signal=0-5  battery=0-5  service=0|1  voice=phone|foston
 *      sco=0|1  call=idx|dir|estado|número|nome (uma linha por chamada)  since=início (epoch)
 *      sync=idle|andamento|ok|erro motivo  data=pasta dos dados no cartão
 *  - recebe comandos, um por linha, no FIFO /run/bigfoston-phone.cmd:
 *      dial NÚMERO | redial | answer | hangup | dtmf C | voice phone|foston | sync | erase
 *      mute 0|1 | vol 1-10 | record 0|1 | hold   (mudo, volume e gravação: voz no FOSTON;
 *      hold: AT+CHLD=2, põe em espera / retoma)
 *  - voz: o FOSTON ainda não toca a voz da ligação (o MT6611 entrega o SCO pelos pinos PCM ao
 *    DSP do MT3351; docs/phone/02-audio-da-chamada.md). Com voice=phone (padrão) o bfphone
 *    desfaz cada canal de voz (SCO) assim que ele abre: o celular volta a voz para o próprio
 *    fone e a ligação nunca fica muda. voice=foston deixa o SCO aberto (testes).
 *  - sync: agenda, histórico e mensagens pelo bfpbap (PBAP e MAP) em
 *    /mnt/sd/BigFoston/telefone/<aparelho>/ (só quando o usuário pede no app Telefone).
 *
 * Privacidade: números, nomes e mensagens nunca vão para o syslog; o estado fica só na RAM.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later */
#define _GNU_SOURCE
#include <dbus/dbus.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>

#define STATE    "/run/bigfoston-phone"
#define FIFO     STATE ".cmd"
#define BTSTATE  "/run/bigfoston-bt"
#define DATADIR  "/mnt/sd/BigFoston/telefone"
#define HFP_AG   "0000111f-0000-1000-8000-00805f9b34fb"
#define MAXCALLS 6
#define BFVOICE  "/usr/sbin/bfvoice"
#define VOICE_IMG "/mnt/sd/BigFoston/voz/wavedev-text.bin"
#define VOICE_PREF "/data/bigfoston/telefone-voz"
#define VOL_PREF   "/data/bigfoston/telefone-volume"
#define REC_DIR    DATADIR "/gravacoes"
#define VOICE_CTL  "/run/bfvoice.ctl"
/* a voz pelo DSP funciona com a imagem do DSP e o patch do MT6611 no cartão (docs/phone/02) */

/* ------------------------------------------------------------------ estado */

typedef struct { int idx, dir, stat; char num[48], name[96]; } call_t;

static struct {
    int link;
    char addr[18], dev[64], op[48];
    int signal, battery, service, roam;          /* -1 = desconhecido */
    int voice_phone;                             /* 1: voz fica no celular */
    int voice_avail;                             /* bfvoice e a imagem do DSP no cartão */
    int voice_on;                                /* voz do DSP ligada agora */
    int mute, vol, rec;                          /* microfone mudo, volume 1-10, gravando */
    char rec_file[160];
    int sco;
    call_t call[MAXCALLS];
    int ncall;
    time_t since;
    char sync[200];
} st = { .signal = -1, .battery = -1, .service = -1, .roam = -1, .voice_phone = 1, .sync = "idle" };

static int now_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (int)(t.tv_sec * 1000 + t.tv_nsec / 1000000);
}

static void datadir(char *out, size_t n)
{
    char a[13] = "";
    int k = 0;
    for(const char *c = st.addr; *c && k < 12; c++) if(*c != ':') a[k++] = *c;
    snprintf(out, n, DATADIR "/%s", a);
}

static void write_state(void)
{
    char tmp[] = STATE ".tmp";
    FILE *f = fopen(tmp, "w");
    if(!f) return;
    fchmod(fileno(f), 0600);
    fprintf(f, "updated=%ld\nlink=%d\n", (long)time(NULL), st.link);
    if(st.link) {
        char d[96];
        datadir(d, sizeof d);
        fprintf(f, "dev=%s\naddr=%s\ndata=%s\n", st.dev, st.addr, d);
    }
    if(st.op[0]) fprintf(f, "operator=%s\n", st.op);
    fprintf(f, "signal=%d\nbattery=%d\nservice=%d\nroam=%d\n", st.signal, st.battery, st.service, st.roam);
    /* voice_foston: o FOSTON sabe tocar a voz e captar o microfone (falta o caminho do DSP:
     * docs/phone/02-audio-da-chamada.md); até lá a voz fica no celular */
    fprintf(f, "voice=%s\nvoice_foston=%d\nvoice_on=%d\nsco=%d\n", st.voice_phone ? "phone" : "foston",
            st.voice_avail, st.voice_on, st.sco);
    fprintf(f, "mute=%d\nvol=%d\nrec=%d\n", st.mute, st.vol, st.rec);
    for(int i = 0; i < st.ncall; i++)
        fprintf(f, "call=%d|%d|%d|%s|%s\n", st.call[i].idx, st.call[i].dir, st.call[i].stat, st.call[i].num,
                st.call[i].name);
    if(st.since) fprintf(f, "since=%ld\n", (long)st.since);
    fprintf(f, "sync=%s\n", st.sync);
    fclose(f);
    rename(tmp, STATE);
}

/* ------------------------------------------------------------------ agenda */

/* só os dígitos finais: "+55 (11) 9 1234-5678" e "011912345678" combinam */
static void digits_tail(const char *s, char *out, size_t n)
{
    char d[64];
    size_t k = 0;
    for(; *s && k < sizeof d - 1; s++) if(*s >= '0' && *s <= '9') d[k++] = *s;
    d[k] = 0;
    snprintf(out, n, "%s", k > 8 ? d + k - 8 : d);
}

/* copia sem partir um caractere UTF-8 no fim */
static void copy_utf8(char *dst, size_t n, const char *src)
{
    size_t k = strlen(src);
    if(k >= n) {
        k = n - 1;
        while(k > 0 && ((unsigned char)src[k] & 0xc0) == 0x80) k--;
    }
    memcpy(dst, src, k);
    dst[k] = 0;
}

static void lookup_name(const char *num, char *name, size_t n)
{
    name[0] = 0;
    char want[16], dir[96], path[128];
    digits_tail(num, want, sizeof want);
    if(strlen(want) < 4) return;
    datadir(dir, sizeof dir);
    snprintf(path, sizeof path, "%s/contatos.tsv", dir);
    FILE *f = fopen(path, "r");
    if(!f) return;
    char line[1024];
    while(fgets(line, sizeof line, f)) {
        char *tab = strchr(line, '\t');
        if(!tab) continue;
        *tab = 0;
        char *tels = tab + 1, *e = strchr(tels, '\t');
        if(e) *e = 0;
        for(char *t = strtok(tels, ";"); t; t = strtok(NULL, ";")) {
            char *c = strchr(t, ':');
            char got[16];
            digits_tail(c ? c + 1 : t, got, sizeof got);
            if(!strcmp(got, want)) { copy_utf8(name, n, line); fclose(f); return; }
        }
    }
    fclose(f);
}

/* ------------------------------------------------------------- canal AT */

static DBusConnection *bus;
static void sco_release(void);
static int at_fd = -1;
static char at_buf[1024];
static int at_len;
#define QMAX 12
static char queue[QMAX][96];
static int qn;
static int pending_since;                 /* ms do comando em espera; 0 = nenhum */
static char pending[96];
static call_t newcall[MAXCALLS];
static int nnew;
static int ind_map[16];                   /* posição do +CIND -> 1 sinal, 2 bateria, 3 serviço, 4 roam */
static int n_ind;
static int next_clcc, next_cind, want_clcc;

static void at_queue(const char *fmt, ...)
{
    if(qn >= QMAX) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(queue[qn], sizeof queue[0], fmt, ap);
    va_end(ap);
    for(int i = 0; i < qn; i++) if(!strcmp(queue[i], queue[qn])) return;   /* já na fila */
    qn++;
}

static void at_close(void)
{
    if(at_fd >= 0) close(at_fd);
    at_fd = -1;
    at_len = 0;
    qn = 0;
    pending_since = 0;
    st.ncall = 0;
    st.since = 0;
    st.op[0] = 0;
    st.signal = st.battery = st.service = st.roam = -1;
}

static int rfcomm_open(const char *addr)
{
    char dev[32], path[96];
    snprintf(dev, sizeof dev, "%s", addr);
    for(char *c = dev; *c; c++) if(*c == ':') *c = '_';
    snprintf(path, sizeof path, "/org/bluealsa/hci0/dev_%s/rfcomm", dev);
    DBusMessage *m = dbus_message_new_method_call("org.bluealsa", path, "org.bluealsa.RFCOMM1", "Open");
    if(!m) return -1;
    DBusError e;
    dbus_error_init(&e);
    DBusMessage *r = dbus_connection_send_with_reply_and_block(bus, m, 3000, &e);
    dbus_message_unref(m);
    if(!r) { dbus_error_free(&e); return -1; }
    int fd = -1;
    if(!dbus_message_get_args(r, &e, DBUS_TYPE_UNIX_FD, &fd, DBUS_TYPE_INVALID)) { dbus_error_free(&e); fd = -1; }
    dbus_message_unref(r);
    if(fd >= 0) fcntl(fd, F_SETFD, FD_CLOEXEC);
    return fd;
}

/* pede ao BlueZ a conexão HFP (o celular às vezes só liga o A2DP) */
static void connect_hfp(const char *addr)
{
    char path[64];
    snprintf(path, sizeof path, "/org/bluez/hci0/dev_%s", addr);
    for(char *c = path + 20; *c; c++) if(*c == ':') *c = '_';
    DBusMessage *m = dbus_message_new_method_call("org.bluez", path, "org.bluez.Device1", "ConnectProfile");
    if(!m) return;
    const char *u = HFP_AG;
    dbus_message_append_args(m, DBUS_TYPE_STRING, &u, DBUS_TYPE_INVALID);
    dbus_message_set_no_reply(m, TRUE);
    dbus_connection_send(bus, m, NULL);
    dbus_connection_flush(bus);
    dbus_message_unref(m);
}

static int same_calls(void)
{
    if(nnew != st.ncall) return 0;
    for(int i = 0; i < nnew; i++)
        if(newcall[i].idx != st.call[i].idx || newcall[i].stat != st.call[i].stat ||
           strcmp(newcall[i].num, st.call[i].num)) return 0;
    return 1;
}

static void commit_calls(void)
{
    if(same_calls()) return;
    for(int i = 0; i < nnew; i++) {               /* nome: o da chamada anterior ou o da agenda */
        newcall[i].name[0] = 0;
        for(int j = 0; j < st.ncall; j++)
            if(st.call[j].idx == newcall[i].idx && !strcmp(st.call[j].num, newcall[i].num))
                snprintf(newcall[i].name, sizeof newcall[i].name, "%s", st.call[j].name);
        if(!newcall[i].name[0] && newcall[i].num[0]) lookup_name(newcall[i].num, newcall[i].name, sizeof newcall[i].name);
    }
    int had_active = 0, has_active = 0;
    for(int i = 0; i < st.ncall; i++) if(st.call[i].stat == 0) had_active = 1;
    for(int i = 0; i < nnew; i++) if(newcall[i].stat == 0) has_active = 1;
    if(has_active && !had_active) st.since = time(NULL);
    if(!nnew) { st.since = 0; sco_release(); }
    memcpy(st.call, newcall, sizeof newcall);
    st.ncall = nnew;
    write_state();
}

/* "a","b",... -> campo n (0 = primeiro), sem aspas */
static void csv_field(const char *s, int n, char *out, size_t on)
{
    out[0] = 0;
    for(int i = 0; i < n && s; i++) {
        int q = 0;
        for(; *s && (q || *s != ','); s++) if(*s == '"') q = !q;
        s = *s ? s + 1 : NULL;
    }
    if(!s) return;
    while(*s == ' ') s++;
    size_t k = 0;
    int q = 0;
    for(; *s && (q || *s != ',') && k + 1 < on; s++) {
        if(*s == '"') { q = !q; continue; }
        out[k++] = *s;
    }
    out[k] = 0;
}

static void on_ind(int pos, int v)
{
    if(pos < 1 || pos > n_ind) return;
    switch(ind_map[pos - 1]) {
    case 1: st.signal = v; break;
    case 2: st.battery = v; break;
    case 3: st.service = v; break;
    case 4: st.roam = v; break;
    case 5: want_clcc = 1; break;             /* call/callsetup/callheld mudou */
    }
}

static void on_line(char *l)
{
    /* o BlueALSA repassa as respostas finais com ':' na frente (":OK", ":ERROR") */
    while(*l == ':' || *l == ' ') l++;
    if(!strncmp(l, "+CLCC:", 6)) {
        if(nnew >= MAXCALLS) return;
        call_t *c = &newcall[nnew];
        memset(c, 0, sizeof *c);
        char f[48];
        csv_field(l + 6, 0, f, sizeof f); c->idx = atoi(f);
        csv_field(l + 6, 1, f, sizeof f); c->dir = atoi(f);
        csv_field(l + 6, 2, f, sizeof f); c->stat = atoi(f);
        csv_field(l + 6, 5, c->num, sizeof c->num);
        for(char *p = c->num; *p; p++) if(*p == '|' || *p == '\t') *p = ' ';
        nnew++;
    } else if(!strncmp(l, "+CIND:", 6)) {
        if(strchr(l, '(')) {                      /* AT+CIND=? : nomes dos indicadores */
            n_ind = 0;
            for(char *p = strchr(l, '('); p && n_ind < 16; p = strchr(p + 1, '(')) {
                if(p[1] != '"') continue;
                int k = 0;
                if(!strncmp(p + 2, "signal", 6)) k = 1;
                else if(!strncmp(p + 2, "battchg", 7)) k = 2;
                else if(!strncmp(p + 2, "service", 7)) k = 3;
                else if(!strncmp(p + 2, "roam", 4)) k = 4;
                else if(!strncmp(p + 2, "call", 4)) k = 5;
                ind_map[n_ind++] = k;
            }
        } else {
            char *p = l + 6;
            for(int i = 1; i <= n_ind && *p; i++) {
                on_ind(i, atoi(p));
                p = strchr(p, ',');
                if(!p) break;
                p++;
            }
            write_state();
        }
    } else if(!strncmp(l, "+CIEV:", 6)) {
        int pos = 0, v = 0;
        if(sscanf(l + 6, "%d,%d", &pos, &v) == 2) on_ind(pos, v);
        want_clcc = 1;
        write_state();
    } else if(!strncmp(l, "+COPS:", 6)) {
        csv_field(l + 6, 2, st.op, sizeof st.op);
        for(char *p = st.op; *p; p++) if(*p == '\n' || *p == '|') *p = ' ';
        write_state();
    } else if(!strcmp(l, "RING") || !strncmp(l, "+CLIP:", 6) || !strncmp(l, "+CCWA:", 6)) {
        want_clcc = 1;
    } else if(!strcmp(l, "OK") || !strcmp(l, "ERROR") || !strncmp(l, "+CME ERROR", 10) ||
              !strcmp(l, "NO CARRIER") || !strcmp(l, "BUSY") || !strcmp(l, "NO ANSWER")) {
        if(pending_since && !strcmp(pending, "AT+CLCC") && !strcmp(l, "OK")) commit_calls();
        if(pending_since && strcmp(l, "OK") && strncmp(pending, "AT+C", 4) && strncmp(pending, "AT+V", 4))
            syslog(LOG_INFO, "comando do telefone recusado (%s)", l[0] == '+' ? "CME" : l);
        /* depois de um comando recusado (não o próprio CLCC), relê as chamadas */
        if(pending_since && strcmp(l, "OK") && strcmp(pending, "AT+CLCC")) want_clcc = 1;
        pending_since = 0;
    }
}

static void at_read(void)
{
    ssize_t k = read(at_fd, at_buf + at_len, sizeof at_buf - 1 - (size_t)at_len);
    if(k <= 0) {
        syslog(LOG_INFO, "canal AT do HFP fechado");
        at_close();
        st.link = 0;
        write_state();
        return;
    }
    at_len += (int)k;
    at_buf[at_len] = 0;
    char *s = at_buf, *e;
    while((e = strpbrk(s, "\r\n"))) {
        *e = 0;
        if(*s) on_line(s);
        s = e + 1;
    }
    at_len = (int)strlen(s);
    memmove(at_buf, s, (size_t)at_len + 1);
    if(at_len > (int)sizeof at_buf - 64) at_len = 0;          /* lixo sem fim de linha */
}

static void at_pump(void)
{
    int t = now_ms();
    if(pending_since && t - pending_since < 4000) return;   /* espera OK/ERROR (até 4 s) */
    pending_since = 0;
    if(!qn) {
        if(want_clcc || t - next_clcc >= 0) {
            at_queue("AT+CLCC");
            want_clcc = 0;
            next_clcc = t + (st.ncall ? 1000 : 5000);
        } else if(t - next_cind >= 0) {
            at_queue("AT+CIND?");
            at_queue("AT+COPS?");
            next_cind = t + 30000;
        }
    }
    if(!qn) return;
    snprintf(pending, sizeof pending, "%s", queue[0]);
    memmove(queue[0], queue[1], sizeof queue[0] * (size_t)--qn);
    if(!strcmp(pending, "AT+CLCC")) nnew = 0;
    char b[104];
    int n = snprintf(b, sizeof b, "%s\r", pending);
    if(write(at_fd, b, (size_t)n) != n) { at_close(); st.link = 0; write_state(); return; }
    pending_since = t ? t : 1;
}

/* ------------------------------------------------------------- voz (SCO) */

static int run_bfvoice(const char *what)
{
    pid_t p = fork();
    if(p == 0) {
        int nul = open("/dev/null", O_WRONLY);
        if(nul >= 0) { dup2(nul, 1); dup2(nul, 2); }
        execl(BFVOICE, "bfvoice", what, (char *)NULL);
        _exit(127);
    }
    if(p < 0) return -1;
    int stt = 0;
    while(waitpid(p, &stt, 0) < 0 && errno == EINTR) {}
    return WIFEXITED(stt) && WEXITSTATUS(stt) == 0 ? 0 : -1;
}

static void voice_check(void)
{
    st.voice_avail = !access(BFVOICE, X_OK) && !access(VOICE_IMG, R_OK);
}

/* canal de voz pedido pelo FOSTON no meio da ligação: um socket SCO do kernel para o celular
 * (o HF pode abrir o SCO no HFP sem negociação de codec), mantido aberto e sem dados — o som
 * vai pelo PCM do chip ao DSP. O BlueALSA não serve: como HF ele não abre o SCO sozinho. */
#define BTPROTO_SCO 2
struct sockaddr_sco_ { unsigned short family; uint8_t bdaddr[6]; };
static int hold_fd[2] = { -1, -1 };

static void sco_release(void)
{
    for(int i = 0; i < 2; i++) if(hold_fd[i] >= 0) { close(hold_fd[i]); hold_fd[i] = -1; }
}

static void sco_request(void)
{
    if(hold_fd[0] >= 0 || !st.addr[0]) return;
    unsigned v[6];
    if(sscanf(st.addr, "%x:%x:%x:%x:%x:%x", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) != 6) return;
    int sk = socket(31, SOCK_SEQPACKET | SOCK_CLOEXEC | SOCK_NONBLOCK, BTPROTO_SCO);
    if(sk < 0) { syslog(LOG_ERR, "socket SCO: %m"); return; }
    struct sockaddr_sco_ a = { 31, { 0 } };
    for(int i = 0; i < 6; i++) a.bdaddr[i] = (uint8_t)v[5 - i];
    if(connect(sk, (struct sockaddr *)&a, sizeof a) < 0 && errno != EINPROGRESS) {
        syslog(LOG_INFO, "pedido do canal de voz recusado: %m");
        close(sk);
        return;
    }
    hold_fd[0] = sk;
    syslog(LOG_INFO, "canal de voz pedido ao celular");
}

/* controle do bfvoice durante a ligação (mudo, volume, gravação) */
static void voice_ctl(void)
{
    FILE *f = fopen(VOICE_CTL ".tmp", "w");
    if(!f) return;
    fprintf(f, "mute=%d gain=%d rec=%s\n", st.mute, 100 + st.vol * 30, st.rec ? st.rec_file : "-");
    fclose(f);
    rename(VOICE_CTL ".tmp", VOICE_CTL);
}

/* bfvoice run: liga o DSP e faz a ponte dos blocos de 20 ms até receber SIGTERM */
static pid_t voice_pid;

static void voice_start(void)
{
    if(st.voice_on) return;
    st.mute = 0;
    st.rec = 0;
    voice_ctl();
    pid_t p = fork();
    if(p == 0) {
        int nul = open("/dev/null", O_WRONLY);
        if(nul >= 0) { dup2(nul, 1); dup2(nul, 2); }
        execl(BFVOICE, "bfvoice", "run", (char *)NULL);
        _exit(127);
    }
    if(p > 0) {
        voice_pid = p;
        st.voice_on = 1;
        syslog(LOG_INFO, "voz da ligação no FOSTON (DSP ligado)");
    } else {
        syslog(LOG_ERR, "bfvoice não iniciou: voz volta para o celular");
        st.voice_phone = 1;
    }
    write_state();
}

static void voice_stop(void)
{
    if(voice_pid > 0) {
        kill(voice_pid, SIGTERM);
        for(int i = 0; i < 30 && waitpid(voice_pid, NULL, WNOHANG) == 0; i++) usleep(100000);
        if(waitpid(voice_pid, NULL, WNOHANG) == 0) { kill(voice_pid, SIGKILL); waitpid(voice_pid, NULL, 0); }
        voice_pid = 0;
    } else if(!access("/run/bfvoice.saved", F_OK)) run_bfvoice("stop");   /* sobra de outra execução */
    if(st.voice_on || st.rec || st.mute) { st.voice_on = 0; st.rec = 0; st.mute = 0; write_state(); }
}

static void sco_drop(void);

#define BTPROTO_HCI 1
#define SOL_HCI     0
#define HCI_FILTER  2
struct sockaddr_hci { unsigned short family, dev, channel; };
struct hci_filter { uint32_t type_mask; uint32_t event_mask[2]; uint16_t opcode; };
static int hci = -1;

static void hci_open(void)
{
    hci = socket(31, SOCK_RAW | SOCK_CLOEXEC | SOCK_NONBLOCK, BTPROTO_HCI);
    if(hci < 0) return;
    struct sockaddr_hci a = { 31, 0, 0 };
    struct hci_filter f = { 1u << 4, { 0, 0 }, 0 };
    f.event_mask[0x03 >> 5] |= 1u << (0x03 & 31);           /* Connection Complete */
    f.event_mask[0x05 >> 5] |= 1u << (0x05 & 31);           /* Disconnection Complete */
    f.event_mask[0x2c >> 5] |= 1u << (0x2c & 31);           /* Synchronous Connection Complete */
    if(bind(hci, (struct sockaddr *)&a, sizeof a) < 0 ||
       setsockopt(hci, SOL_HCI, HCI_FILTER, &f, sizeof f) < 0) { close(hci); hci = -1; }
}

static uint16_t sco_handle;
static int sco_drops, sco_drop_t0;

static void hci_read(void)
{
    uint8_t b[260];
    ssize_t n = read(hci, b, sizeof b);
    if(n < 4 || b[0] != 0x04) return;
    uint8_t ev = b[1];
    const uint8_t *p = b + 3;
    if((ev == 0x2c || (ev == 0x03 && n >= 3 + 11 && p[9] == 0x00)) && n >= 3 + 10 && p[0] == 0) {
        sco_handle = (uint16_t)(p[1] | p[2] << 8);
        st.sco = 1;
        int t = now_ms();
        if(t - sco_drop_t0 > 30000) { sco_drop_t0 = t; sco_drops = 0; }
        /* voz no celular: desfaz o canal de voz (no máximo 20 vezes em 30 s, para não brigar
         * com um celular que insiste); voz no FOSTON: liga o DSP */
        if(st.voice_phone || !st.voice_avail) {
            if(sco_drops < 20) { sco_drop(); sco_drops++; }
        } else voice_start();
        write_state();
    } else if(ev == 0x05 && n >= 3 + 4 && p[0] == 0 && st.sco && (uint16_t)(p[1] | p[2] << 8) == sco_handle) {
        st.sco = 0;
        voice_stop();
        sco_release();
        write_state();
    }
}

static void sco_drop(void)
{
    if(!st.sco || hci < 0) return;
    uint8_t c[] = { 0x01, 0x06, 0x04, 3, (uint8_t)sco_handle, (uint8_t)(sco_handle >> 8), 0x13 };
    if(write(hci, c, sizeof c) == (ssize_t)sizeof c)
        syslog(LOG_INFO, "voz da ligação mantida no celular (canal de voz desfeito)");
}

/* ------------------------------------------------------- sincronização */

static pid_t sync_pid;
static int sync_step;
static char sync_dir[96];
static const char *const steps[][2] = {
    { "pb", "contatos.tsv" }, { "cch", "historico.tsv" }, { "msg/inbox", "mensagens.tsv" },
    { "msg/sent", "enviadas.tsv" },
};
#define NSTEPS (int)(sizeof steps / sizeof steps[0])
static int sync_fd = -1;
static char sync_res[NSTEPS][40];

static void mkdirs(const char *d)
{
    char p[128];
    snprintf(p, sizeof p, "%s", d);
    for(char *c = p + 1; *c; c++) if(*c == '/') { *c = 0; mkdir(p, 0700); *c = '/'; }
    mkdir(p, 0700);
}

static void sync_next(void)
{
    if(sync_step >= NSTEPS) {
        /* resumo: ok se a agenda veio; cada parte com o seu resultado */
        snprintf(st.sync, sizeof st.sync, "%s %s|%s|%s|%s", strncmp(sync_res[0], "ok", 2) ? "erro" : "ok",
                 sync_res[0], sync_res[1], sync_res[2], sync_res[3]);
        for(char *c = st.sync; *c; c++) if(*c == '\n') *c = 0;
        char f[128];
        snprintf(f, sizeof f, "%s/sincronizado", sync_dir);
        FILE *o = fopen(f, "w");
        if(o) { fprintf(o, "%ld\n", (long)time(NULL)); fclose(o); }
        sync()  ;
        write_state();
        return;
    }
    int pfd[2];
    if(pipe2(pfd, O_CLOEXEC) < 0) { snprintf(st.sync, sizeof st.sync, "erro pipe"); write_state(); return; }
    char out[160];
    snprintf(out, sizeof out, "%s/%s", sync_dir, steps[sync_step][1]);
    pid_t p = fork();
    if(p == 0) {
        dup2(pfd[1], 1);
        execl("/usr/sbin/bfpbap", "bfpbap", st.addr, "0", steps[sync_step][0], out, (char *)NULL);
        _exit(127);
    }
    close(pfd[1]);
    if(p < 0) { close(pfd[0]); snprintf(st.sync, sizeof st.sync, "erro fork"); write_state(); return; }
    sync_pid = p;
    sync_fd = pfd[0];
}

static void sync_done(void)
{
    char b[64] = "";
    ssize_t n = read(sync_fd, b, sizeof b - 1);
    b[n > 0 ? n : 0] = 0;
    close(sync_fd);
    sync_fd = -1;
    waitpid(sync_pid, NULL, 0);
    sync_pid = 0;
    char *nl = strchr(b, '\n');
    if(nl) *nl = 0;
    snprintf(sync_res[sync_step], sizeof sync_res[0], "%s", b[0] ? b : "erro falhou");
    for(char *c = sync_res[sync_step]; *c; c++) if(*c == '|') *c = ' ';
    sync_step++;
    sync_next();
}

static void sync_start(void)
{
    if(sync_pid || !st.link) return;
    datadir(sync_dir, sizeof sync_dir);
    mkdirs(sync_dir);
    sync_step = 0;
    snprintf(st.sync, sizeof st.sync, "andamento");
    write_state();
    sync_next();
}

/* apaga do cartão os dados do celular (agenda, histórico, mensagens) */
static void erase_data(void)
{
    char d[96];
    datadir(d, sizeof d);
    DIR *dir = opendir(d);
    if(dir) {
        struct dirent *e;
        char p[384];
        while((e = readdir(dir)))
            if(e->d_name[0] != '.') { snprintf(p, sizeof p, "%s/%s", d, e->d_name); unlink(p); }
        closedir(dir);
        rmdir(d);
        sync();
    }
    snprintf(st.sync, sizeof st.sync, "idle");
    for(int i = 0; i < st.ncall; i++) st.call[i].name[0] = 0;
    write_state();
}

/* ------------------------------------------------------------- comandos */

static void command(char *l)
{
    char *arg = strchr(l, ' ');
    if(arg) *arg++ = 0;
    if(!strcmp(l, "voice") && arg) {
        voice_check();
        int phone = strcmp(arg, "foston") != 0 || !st.voice_avail;
        st.voice_phone = phone;
        FILE *f = fopen(VOICE_PREF, "w");              /* a escolha vale para as próximas ligações */
        if(f) { fputs(phone ? "phone\n" : "foston\n", f); fclose(f); }
        if(phone) {                                     /* volta a voz para o celular */
            voice_stop();
            sco_release();
            sco_drop();
        } else if(st.ncall) {                           /* no meio da ligação: pede o canal de voz */
            if(st.sco) voice_start();
            else sco_request();
        }
        write_state();
        return;
    }
    if(!strcmp(l, "vol") && arg) {
        int v = atoi(arg);
        st.vol = v < 1 ? 1 : v > 10 ? 10 : v;
        FILE *f = fopen(VOL_PREF, "w");
        if(f) { fprintf(f, "%d\n", st.vol); fclose(f); }
        voice_ctl();
        if(at_fd >= 0 && !st.voice_on) at_queue("AT+VGS=%d", st.vol * 3 / 2);   /* voz no celular */
        write_state();
        return;
    }
    if(!strcmp(l, "mute") && arg) {
        st.mute = st.voice_on && atoi(arg);              /* só o microfone do FOSTON */
        voice_ctl();
        write_state();
        return;
    }
    if(!strcmp(l, "record") && arg) {
        int on = atoi(arg) && st.voice_on;
        if(on && !st.rec) {
            mkdirs(REC_DIR);
            time_t t = time(NULL);
            struct tm tm;
            localtime_r(&t, &tm);
            char n[40];
            strftime(n, sizeof n, "%Y%m%d-%H%M%S", &tm);
            snprintf(st.rec_file, sizeof st.rec_file, REC_DIR "/ligacao-%s.wav", n);
        }
        st.rec = on;
        voice_ctl();
        write_state();
        return;
    }
    if(!strcmp(l, "sync")) { sync_start(); return; }
    if(!strcmp(l, "erase")) { if(!sync_pid) erase_data(); return; }
    if(at_fd < 0) return;
    if(!strcmp(l, "dial") && arg) {
        char n[40];
        size_t k = 0;
        for(char *c = arg; *c && k < sizeof n - 1; c++)
            if((*c >= '0' && *c <= '9') || *c == '*' || *c == '#' || (*c == '+' && k == 0)) n[k++] = *c;
        n[k] = 0;
        if(k) at_queue("ATD%s;", n);
    } else if(!strcmp(l, "redial")) at_queue("AT+BLDN");
    else if(!strcmp(l, "answer")) at_queue("ATA");
    else if(!strcmp(l, "hangup")) at_queue("AT+CHUP");
    else if(!strcmp(l, "hold")) at_queue("AT+CHLD=2");
    else if(!strcmp(l, "dtmf") && arg && strchr("0123456789*#ABCD", arg[0]) && arg[0]) at_queue("AT+VTS=%c", arg[0]);
    want_clcc = 1;
}

/* ------------------------------------------------------------- aparelho */

/* celular conectado no /run/bigfoston-bt (dev=addr|pareado|conectado|confiável|rssi|ícone|nome) */
static int find_phone(char *addr, char *name)
{
    FILE *f = fopen(BTSTATE, "r");
    if(!f) return 0;
    char l[256];
    int found = 0;
    while(!found && fgets(l, sizeof l, f)) {
        if(strncmp(l, "dev=", 4)) continue;
        l[strcspn(l, "\n")] = 0;
        char *fld[7];
        int n = 0;
        for(char *s = l + 4; n < 7; n++) { fld[n] = s; s = strchr(s, '|'); if(!s) { n++; break; } *s++ = 0; }
        if(n < 7 || strcmp(fld[2], "1") || strncmp(fld[5], "phone", 5)) continue;
        snprintf(addr, 18, "%s", fld[0]);
        snprintf(name, 64, "%s", fld[6]);
        found = 1;
    }
    fclose(f);
    return found;
}

static volatile sig_atomic_t quit;
static void on_sig(int s) { (void)s; quit = 1; }

int main(void)
{
    openlog("bfphone", LOG_PID, LOG_DAEMON);
    umask(077);                              /* dados do celular: só o root lê */
    signal(SIGPIPE, SIG_IGN);
    signal(SIGTERM, on_sig);
    signal(SIGINT, on_sig);
    DBusError e;
    dbus_error_init(&e);
    bus = dbus_bus_get(DBUS_BUS_SYSTEM, &e);
    if(!bus) { syslog(LOG_ERR, "D-Bus: %s", e.message); return 1; }
    unlink(FIFO);
    if(mkfifo(FIFO, 0600) < 0) { syslog(LOG_ERR, "fifo: %m"); return 1; }
    int ff = open(FIFO, O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if(ff < 0) return 1;
    hci_open();
    voice_check();
    FILE *pf = fopen(VOICE_PREF, "r");                 /* última escolha do usuário */
    if(pf) { char v[16] = ""; if(fgets(v, sizeof v, pf)) st.voice_phone = strncmp(v, "foston", 6) != 0; fclose(pf); }
    st.vol = 5;
    FILE *vf = fopen(VOL_PREF, "r");
    if(vf) { int v; if(fscanf(vf, "%d", &v) == 1 && v >= 1 && v <= 10) st.vol = v; fclose(vf); }
    voice_stop();                                       /* sobra de uma execução anterior */
    write_state();
    int next_vcheck = 0;
    char cmdbuf[256];
    int cmdlen = 0;
    int next_try = 0, phone_since = 0, hfp_asked = 0;

    while(!quit) {
        int t = now_ms();
        if(t - next_try >= 0 && at_fd < 0) {
            next_try = t + 2000;
            char a[18], n[64];
            if(find_phone(a, n)) {
                if(strcmp(a, st.addr)) { snprintf(st.addr, sizeof st.addr, "%s", a); phone_since = t; hfp_asked = 0; }
                snprintf(st.dev, sizeof st.dev, "%s", n);
                at_fd = rfcomm_open(a);
                if(at_fd >= 0) {
                    syslog(LOG_INFO, "canal AT do HFP aberto");
                    st.link = 1;
                    qn = 0;
                    at_queue("AT+CIND=?");
                    at_queue("AT+CIND?");
                    at_queue("AT+CLIP=1");
                    at_queue("AT+COPS=3,0");
                    at_queue("AT+COPS?");
                    at_queue("AT+CLCC");
                    next_clcc = t + 5000;
                    next_cind = t + 30000;
                    write_state();
                } else if(!hfp_asked && t - phone_since > 8000) {
                    hfp_asked = 1;                 /* conectado sem HFP: pede uma vez */
                    connect_hfp(a);
                }
            } else if(st.link || st.addr[0]) {
                st.addr[0] = 0;
                st.link = 0;
                write_state();
            }
        }
        if(at_fd >= 0) at_pump();
        if(t - next_vcheck >= 0) {                     /* o cartão pode montar depois */
            next_vcheck = t + 10000;
            int was = st.voice_avail;
            voice_check();
            if(was != st.voice_avail) write_state();
        }

        struct pollfd p[4] = { { ff, POLLIN, 0 }, { at_fd, POLLIN, 0 }, { hci, POLLIN, 0 }, { sync_fd, POLLIN, 0 } };
        if(poll(p, 4, 250) < 0 && errno != EINTR) break;
        if(p[0].revents & POLLIN) {
            ssize_t k = read(ff, cmdbuf + cmdlen, sizeof cmdbuf - 1 - (size_t)cmdlen);
            if(k > 0) {
                cmdlen += (int)k;
                cmdbuf[cmdlen] = 0;
                char *s = cmdbuf, *nl;
                while((nl = strchr(s, '\n'))) { *nl = 0; if(*s) command(s); s = nl + 1; }
                cmdlen = (int)strlen(s);
                memmove(cmdbuf, s, (size_t)cmdlen + 1);
                if(cmdlen > 200) cmdlen = 0;
            }
        }
        if(at_fd >= 0 && (p[1].revents & (POLLIN | POLLHUP | POLLERR))) at_read();
        if(hci >= 0 && (p[2].revents & POLLIN)) hci_read();
        if(sync_fd >= 0 && (p[3].revents & (POLLIN | POLLHUP))) sync_done();
        dbus_connection_read_write_dispatch(bus, 0);
    }
    if(sync_pid) kill(sync_pid, SIGTERM);
    voice_stop();
    sco_release();
    unlink(FIFO);
    unlink(STATE);
    return 0;
}
