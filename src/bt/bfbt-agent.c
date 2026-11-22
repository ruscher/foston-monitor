/* bfbt-agent — ponte entre o BlueZ (D-Bus) e o shell do BigFoston, que é estático e não
 * fala D-Bus (docs/hardware-enablement/08-bluetooth-profiles.md).
 *
 *  - agente de pareamento "DisplayYesNo": o código aparece na tela do FOSTON e o usuário
 *    confirma; serviços de aparelhos pareados e confiáveis são autorizados direto;
 *  - estado em /run/bigfoston-bt (chave=valor, reescrito por rename a cada mudança);
 *  - comandos, um por linha, no FIFO /run/bigfoston-bt.cmd:
 *      on | off                       liga/desliga o Bluetooth (S35bluetooth + marca)
 *      visible 0|1 | scan 0|1         visível por DiscoverableTimeout / procurar aparelhos (20 s)
 *      pair|connect|disconnect|forget AA:BB:CC:DD:EE:FF
 *      accept | reject                responde o pedido de pareamento pendente
 *      play | pause | next | prev     controla a música do celular (AVRCP)
 *  - ao ligar, conecta uma vez os aparelhos pareados e confiáveis (como um rádio de carro).
 *
 * SPDX-License-Identifier: GPL-3.0-or-later */
#define _GNU_SOURCE
#include <dbus/dbus.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>

#define STATE   "/run/bigfoston-bt"
#define FIFO    STATE ".cmd"
#define FLAG    "/data/bigfoston/bluetooth-ligado"
#define SERVICE "/etc/init.d/S35bluetooth"
#define AGENT   "/bigfoston/agent"
#define BLUEZ   "org.bluez"
#define MAXDEV  32

typedef struct {
    char path[96], addr[18], name[64], icon[32];
    int paired, connected, trusted, rssi, has_rssi;
} dev_t_;

static DBusConnection *bus;
static volatile sig_atomic_t quit;
static int dirty = 1, agent_ok, reconnect_done;   /* reconnect_done: tentativas feitas */
static long powered_at;
static long scan_until;                             /* busca: encerra sozinha (rouba o rádio do áudio) */
#define SCAN_MS 20000                             /* quando o adaptador apareceu ligado */
static char adapter[64];                 /* /org/bluez/hci0 ou "" */
static char last_result[160];

/* pedido de pareamento esperando o usuário */
static DBusMessage *req_msg;
static char req_kind[16], req_dev[96];
static unsigned req_pass;

/* ------------------------------------------------------------ utilidades */

static long mono_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000L + t.tv_nsec / 1000000L;
}

static void run_service(const char *arg1, const char *arg2)
{
    pid_t p = fork();
    if(p == 0) {
        setsid();
        execl(SERVICE, "S35bluetooth", arg1, arg2, (char *)NULL);
        _exit(127);
    }
}

static void dev_path(char *out, size_t n, const char *addr)
{
    char a[18];
    snprintf(a, sizeof a, "%s", addr);
    for(char *c = a; *c; c++) if(*c == ':') *c = '_';
    snprintf(out, n, "%s/dev_%s", adapter, a);
}

static int valid_addr(const char *a)
{
    if(strlen(a) != 17) return 0;
    for(int i = 0; i < 17; i++) {
        char c = a[i];
        if(i % 3 == 2 ? c != ':' : !((c >= '0' && c <= '9') || (c >= 'A' && c <= 'F'))) return 0;
    }
    return 1;
}

/* texto seguro para uma linha chave=valor com campos separados por | */
static void clean(char *dst, size_t n, const char *src)
{
    size_t k = 0;
    for(; src && *src && k + 1 < n; src++) {
        unsigned char c = (unsigned char)*src;
        dst[k++] = (c < 0x20 || c == '|') ? ' ' : (char)c;
    }
    dst[k] = 0;
}

/* --------------------------------------------------- chamadas assíncronas */

static void call_done(DBusPendingCall *pc, void *ud)
{
    const char *what = ud;
    DBusMessage *r = dbus_pending_call_steal_reply(pc);
    if(r && dbus_message_get_type(r) == DBUS_MESSAGE_TYPE_ERROR) {
        const char *msg = NULL;
        dbus_message_get_args(r, NULL, DBUS_TYPE_STRING, &msg, DBUS_TYPE_INVALID);
        snprintf(last_result, sizeof last_result, "%s|erro|%s", what, dbus_message_get_error_name(r));
        syslog(LOG_INFO, "%s: %s %s", what, dbus_message_get_error_name(r), msg ? msg : "");
    } else
        snprintf(last_result, sizeof last_result, "%s|ok|", what);
    if(r) dbus_message_unref(r);
    dbus_pending_call_unref(pc);
    dirty = 1;
}

/* chama path.iface.method sem esperar; o resultado vai para "result=" */
static void call_async(const char *path, const char *iface, const char *method, const char *what, int timeout_ms,
                       int first_type, ...)
{
    DBusMessage *m = dbus_message_new_method_call(BLUEZ, path, iface, method);
    if(!m) return;
    if(first_type != DBUS_TYPE_INVALID) {
        va_list ap;
        va_start(ap, first_type);
        dbus_message_append_args_valist(m, first_type, ap);
        va_end(ap);
    }
    DBusPendingCall *pc = NULL;
    if(dbus_connection_send_with_reply(bus, m, &pc, timeout_ms) && pc)
        dbus_pending_call_set_notify(pc, call_done, (void *)what, NULL);
    dbus_message_unref(m);
    snprintf(last_result, sizeof last_result, "%s|andamento|", what);
}

static void set_bool(const char *path, const char *iface, const char *prop, int v)
{
    DBusMessage *m = dbus_message_new_method_call(BLUEZ, path, "org.freedesktop.DBus.Properties", "Set");
    if(!m) return;
    DBusMessageIter it, var;
    dbus_bool_t b = v ? TRUE : FALSE;
    dbus_message_iter_init_append(m, &it);
    dbus_message_iter_append_basic(&it, DBUS_TYPE_STRING, &iface);
    dbus_message_iter_append_basic(&it, DBUS_TYPE_STRING, &prop);
    dbus_message_iter_open_container(&it, DBUS_TYPE_VARIANT, "b", &var);
    dbus_message_iter_append_basic(&var, DBUS_TYPE_BOOLEAN, &b);
    dbus_message_iter_close_container(&it, &var);
    dbus_connection_send(bus, m, NULL);
    dbus_message_unref(m);
}

/* ------------------------------------------------------ agente do BlueZ */

static void reply_simple(DBusMessage *msg, const char *err)
{
    DBusMessage *r = err ? dbus_message_new_error(msg, err, "recusado no BigFoston") : dbus_message_new_method_return(msg);
    if(r) { dbus_connection_send(bus, r, NULL); dbus_message_unref(r); }
}

static void req_clear(void)
{
    if(req_msg) dbus_message_unref(req_msg);
    req_msg = NULL;
    req_kind[0] = req_dev[0] = 0;
    dirty = 1;
}

static void req_answer(int yes)
{
    if(!req_msg) return;
    if(yes && !strcmp(req_kind, "confirm")) set_bool(req_dev, "org.bluez.Device1", "Trusted", 1);
    reply_simple(req_msg, yes ? NULL : "org.bluez.Error.Rejected");
    req_clear();
}

static void req_set(DBusMessage *msg, const char *kind, const char *dev, unsigned pass)
{
    if(req_msg) req_answer(0);                      /* um pedido por vez */
    req_msg = dbus_message_ref(msg);
    snprintf(req_kind, sizeof req_kind, "%s", kind);
    snprintf(req_dev, sizeof req_dev, "%s", dev);
    req_pass = pass;
    dirty = 1;
}

static int dev_bool(const char *path, const char *prop);

static DBusHandlerResult agent_msg(DBusConnection *c, DBusMessage *msg, void *ud)
{
    (void)c; (void)ud;
    if(!dbus_message_has_interface(msg, "org.bluez.Agent1")) return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
    const char *mem = dbus_message_get_member(msg), *dev = NULL, *uuid = NULL;
    dbus_uint32_t pass = 0;
    if(!strcmp(mem, "RequestConfirmation") &&
       dbus_message_get_args(msg, NULL, DBUS_TYPE_OBJECT_PATH, &dev, DBUS_TYPE_UINT32, &pass, DBUS_TYPE_INVALID)) {
        syslog(LOG_INFO, "pedido de pareamento com código");
        req_set(msg, "confirm", dev, pass);
    } else if(!strcmp(mem, "RequestAuthorization") &&
              dbus_message_get_args(msg, NULL, DBUS_TYPE_OBJECT_PATH, &dev, DBUS_TYPE_INVALID)) {
        req_set(msg, "authorize", dev, 0);
    } else if(!strcmp(mem, "AuthorizeService") &&
              dbus_message_get_args(msg, NULL, DBUS_TYPE_OBJECT_PATH, &dev, DBUS_TYPE_STRING, &uuid, DBUS_TYPE_INVALID)) {
        if(dev_bool(dev, "Paired") == 1 && dev_bool(dev, "Trusted") == 1) reply_simple(msg, NULL);
        else req_set(msg, "service", dev, 0);
    } else if(!strcmp(mem, "Cancel")) {
        req_clear();
        reply_simple(msg, NULL);
    } else if(!strcmp(mem, "Release") || !strcmp(mem, "DisplayPasskey") || !strcmp(mem, "DisplayPinCode")) {
        reply_simple(msg, NULL);
    } else {
        /* RequestPinCode / RequestPasskey: aparelhos antigos que pedem digitar código */
        reply_simple(msg, "org.bluez.Error.Rejected");
    }
    return DBUS_HANDLER_RESULT_HANDLED;
}

static void agent_register(void)
{
    agent_ok = 0;
    const char *path = AGENT, *cap = "DisplayYesNo";
    for(int k = 0; k < 2; k++) {
        DBusMessage *m = dbus_message_new_method_call(BLUEZ, "/org/bluez", "org.bluez.AgentManager1",
                                                      k ? "RequestDefaultAgent" : "RegisterAgent");
        if(!m) return;
        if(k) dbus_message_append_args(m, DBUS_TYPE_OBJECT_PATH, &path, DBUS_TYPE_INVALID);
        else dbus_message_append_args(m, DBUS_TYPE_OBJECT_PATH, &path, DBUS_TYPE_STRING, &cap, DBUS_TYPE_INVALID);
        DBusError e;
        dbus_error_init(&e);
        DBusMessage *r = dbus_connection_send_with_reply_and_block(bus, m, 3000, &e);
        dbus_message_unref(m);
        if(r) dbus_message_unref(r);
        if(dbus_error_is_set(&e)) {
            int already = !strcmp(e.name, "org.bluez.Error.AlreadyExists");
            dbus_error_free(&e);
            if(!already) return;
        }
    }
    agent_ok = 1;
    syslog(LOG_INFO, "agente de pareamento registrado");
}

/* ------------------------------------------------- leitura do estado */

/* lê um booleano de Device1 direto (usado pelo agente) */
static int dev_bool(const char *path, const char *prop)
{
    const char *iface = "org.bluez.Device1";
    DBusMessage *m = dbus_message_new_method_call(BLUEZ, path, "org.freedesktop.DBus.Properties", "Get");
    if(!m) return -1;
    dbus_message_append_args(m, DBUS_TYPE_STRING, &iface, DBUS_TYPE_STRING, &prop, DBUS_TYPE_INVALID);
    DBusMessage *r = dbus_connection_send_with_reply_and_block(bus, m, 2000, NULL);
    dbus_message_unref(m);
    if(!r) return -1;
    int v = -1;
    DBusMessageIter it, var;
    if(dbus_message_iter_init(r, &it) && dbus_message_iter_get_arg_type(&it) == DBUS_TYPE_VARIANT) {
        dbus_message_iter_recurse(&it, &var);
        if(dbus_message_iter_get_arg_type(&var) == DBUS_TYPE_BOOLEAN) {
            dbus_bool_t b;
            dbus_message_iter_get_basic(&var, &b);
            v = b ? 1 : 0;
        }
    }
    dbus_message_unref(r);
    return v;
}

typedef struct {
    int present, powered, discoverable, discovering;
    char name[64];
    dev_t_ dev[MAXDEV];
    int ndev;
    char pl_status[16], pl_title[96], pl_artist[96], pl_album[96], pl_dev[96], pl_path[128];
    unsigned pl_dur, pl_pos;              /* ms (AVRCP Track.Duration, Position) */
    long pl_pos_at;                       /* CLOCK_MONOTONIC (ms) da leitura da posição */
    char stream[12];                      /* MediaTransport1.State: idle, pending, active */
} state_t;

static void var_get(DBusMessageIter *var, int type, void *out)
{
    if(dbus_message_iter_get_arg_type(var) == type) dbus_message_iter_get_basic(var, out);
}

static void var_str(DBusMessageIter *var, char *out, size_t n)
{
    int t = dbus_message_iter_get_arg_type(var);
    if(t == DBUS_TYPE_STRING || t == DBUS_TYPE_OBJECT_PATH) {
        const char *s;
        dbus_message_iter_get_basic(var, &s);
        clean(out, n, s);
    }
}

static void var_bool(DBusMessageIter *var, int *out)
{
    dbus_bool_t b = FALSE;
    if(dbus_message_iter_get_arg_type(var) == DBUS_TYPE_BOOLEAN) { dbus_message_iter_get_basic(var, &b); *out = b ? 1 : 0; }
}

/* props: a{sv} de uma interface */
static void read_iface(state_t *s, const char *path, const char *iface, DBusMessageIter *props)
{
    int is_ad = !strcmp(iface, "org.bluez.Adapter1"), is_dev = !strcmp(iface, "org.bluez.Device1"),
        is_pl = !strcmp(iface, "org.bluez.MediaPlayer1"), is_tr = !strcmp(iface, "org.bluez.MediaTransport1");
    if(!is_ad && !is_dev && !is_pl && !is_tr) return;
    dev_t_ *d = NULL;
    if(is_ad) {
        if(adapter[0] && strcmp(adapter, path)) return;
        s->present = 1;
        snprintf(adapter, sizeof adapter, "%s", path);
    }
    if(is_pl) { snprintf(s->pl_path, sizeof s->pl_path, "%s", path); s->pl_pos_at = mono_ms(); }
    if(is_dev) {
        if(s->ndev >= MAXDEV) return;
        d = &s->dev[s->ndev++];
        memset(d, 0, sizeof *d);
        snprintf(d->path, sizeof d->path, "%s", path);
    }
    DBusMessageIter e;
    dbus_message_iter_recurse(props, &e);
    for(; dbus_message_iter_get_arg_type(&e) == DBUS_TYPE_DICT_ENTRY; dbus_message_iter_next(&e)) {
        DBusMessageIter kv, var;
        const char *k;
        dbus_message_iter_recurse(&e, &kv);
        dbus_message_iter_get_basic(&kv, &k);
        dbus_message_iter_next(&kv);
        dbus_message_iter_recurse(&kv, &var);
        if(is_ad) {
            if(!strcmp(k, "Powered")) var_bool(&var, &s->powered);
            else if(!strcmp(k, "Discoverable")) var_bool(&var, &s->discoverable);
            else if(!strcmp(k, "Discovering")) var_bool(&var, &s->discovering);
            else if(!strcmp(k, "Alias")) var_str(&var, s->name, sizeof s->name);
        } else if(is_dev) {
            if(!strcmp(k, "Address")) var_str(&var, d->addr, sizeof d->addr);
            else if(!strcmp(k, "Alias")) var_str(&var, d->name, sizeof d->name);
            else if(!strcmp(k, "Icon")) var_str(&var, d->icon, sizeof d->icon);
            else if(!strcmp(k, "Paired")) var_bool(&var, &d->paired);
            else if(!strcmp(k, "Connected")) var_bool(&var, &d->connected);
            else if(!strcmp(k, "Trusted")) var_bool(&var, &d->trusted);
            else if(!strcmp(k, "RSSI")) { dbus_int16_t r = 0; var_get(&var, DBUS_TYPE_INT16, &r); d->rssi = r; d->has_rssi = 1; }
        } else if(is_tr) {
            char st[12] = "";
            if(!strcmp(k, "State")) var_str(&var, st, sizeof st);
            /* um transporte ativo vale mais que outro parado (dois aparelhos) */
            if(st[0] && (strcmp(s->stream, "active") || !strcmp(st, "active"))) snprintf(s->stream, sizeof s->stream, "%s", st);
        } else {
            if(!strcmp(k, "Status")) var_str(&var, s->pl_status, sizeof s->pl_status);
            else if(!strcmp(k, "Position")) var_get(&var, DBUS_TYPE_UINT32, &s->pl_pos);
            else if(!strcmp(k, "Device")) var_str(&var, s->pl_dev, sizeof s->pl_dev);
            else if(!strcmp(k, "Track") && dbus_message_iter_get_arg_type(&var) == DBUS_TYPE_ARRAY) {
                DBusMessageIter te;
                dbus_message_iter_recurse(&var, &te);
                for(; dbus_message_iter_get_arg_type(&te) == DBUS_TYPE_DICT_ENTRY; dbus_message_iter_next(&te)) {
                    DBusMessageIter tkv, tv;
                    const char *tk;
                    dbus_message_iter_recurse(&te, &tkv);
                    dbus_message_iter_get_basic(&tkv, &tk);
                    dbus_message_iter_next(&tkv);
                    dbus_message_iter_recurse(&tkv, &tv);
                    if(!strcmp(tk, "Title")) var_str(&tv, s->pl_title, sizeof s->pl_title);
                    else if(!strcmp(tk, "Artist")) var_str(&tv, s->pl_artist, sizeof s->pl_artist);
                    else if(!strcmp(tk, "Album")) var_str(&tv, s->pl_album, sizeof s->pl_album);
                    else if(!strcmp(tk, "Duration")) var_get(&tv, DBUS_TYPE_UINT32, &s->pl_dur);
                }
            }
        }
    }
}

static int read_state(state_t *s)
{
    memset(s, 0, sizeof *s);
    DBusMessage *m = dbus_message_new_method_call(BLUEZ, "/", "org.freedesktop.DBus.ObjectManager", "GetManagedObjects");
    if(!m) return 0;
    DBusMessage *r = dbus_connection_send_with_reply_and_block(bus, m, 3000, NULL);
    dbus_message_unref(m);
    if(!r) { adapter[0] = 0; return 0; }
    char had[64];
    snprintf(had, sizeof had, "%s", adapter);
    adapter[0] = 0;
    DBusMessageIter it, objs;
    if(dbus_message_iter_init(r, &it) && dbus_message_iter_get_arg_type(&it) == DBUS_TYPE_ARRAY) {
        /* duas passadas: o adaptador primeiro (os caminhos dos aparelhos dependem dele) */
        for(int pass = 0; pass < 2; pass++) {
            dbus_message_iter_recurse(&it, &objs);
            for(; dbus_message_iter_get_arg_type(&objs) == DBUS_TYPE_DICT_ENTRY; dbus_message_iter_next(&objs)) {
                DBusMessageIter ent, ifs;
                const char *path;
                dbus_message_iter_recurse(&objs, &ent);
                dbus_message_iter_get_basic(&ent, &path);
                dbus_message_iter_next(&ent);
                dbus_message_iter_recurse(&ent, &ifs);
                for(; dbus_message_iter_get_arg_type(&ifs) == DBUS_TYPE_DICT_ENTRY; dbus_message_iter_next(&ifs)) {
                    DBusMessageIter ie;
                    const char *iface;
                    dbus_message_iter_recurse(&ifs, &ie);
                    dbus_message_iter_get_basic(&ie, &iface);
                    dbus_message_iter_next(&ie);
                    int ad = !strcmp(iface, "org.bluez.Adapter1");
                    if(pass == 0 ? ad : !ad) read_iface(s, path, iface, &ie);
                }
            }
        }
    }
    dbus_message_unref(r);
    if(!adapter[0] && had[0]) { reconnect_done = 0; powered_at = 0; }
    return 1;
}

/* ordem da lista: conectados, pareados, depois os com sinal mais forte */
static int dev_cmp(const void *a, const void *b)
{
    const dev_t_ *x = a, *y = b;
    if(x->connected != y->connected) return y->connected - x->connected;
    if(x->paired != y->paired) return y->paired - x->paired;
    if(x->has_rssi != y->has_rssi) return y->has_rssi - x->has_rssi;
    return y->rssi - x->rssi;
}

static void write_state(const state_t *s)
{
    char tmp[64];
    snprintf(tmp, sizeof tmp, "%s.tmp", STATE);
    FILE *f = fopen(tmp, "w");
    if(!f) return;
    int enabled = access(FLAG, F_OK) == 0;
    fprintf(f, "updated=%ld\nenabled=%d\nagent=%d\nadapter=%d\npowered=%d\ndiscoverable=%d\ndiscovering=%d\nname=%s\n",
            (long)time(NULL), enabled, agent_ok, s->present, s->powered, s->discoverable, s->discovering, s->name);
    dev_t_ d[MAXDEV];
    memcpy(d, s->dev, sizeof(dev_t_) * (size_t)s->ndev);
    qsort(d, (size_t)s->ndev, sizeof d[0], dev_cmp);
    for(int i = 0; i < s->ndev; i++)
        fprintf(f, "dev=%s|%d|%d|%d|%d|%s|%s\n", d[i].addr, d[i].paired, d[i].connected, d[i].trusted,
                d[i].has_rssi ? d[i].rssi : 0, d[i].icon, d[i].name[0] ? d[i].name : d[i].addr);
    if(req_msg) {
        const char *name = "", *addr = "";
        for(int i = 0; i < s->ndev; i++)
            if(!strcmp(s->dev[i].path, req_dev)) { name = s->dev[i].name; addr = s->dev[i].addr; }
        fprintf(f, "req=%s|%06u|%s|%s\n", req_kind, req_pass, addr, name);
    }
    if(s->pl_status[0]) {
        const char *who = "";
        for(int i = 0; i < s->ndev; i++)
            if(!strncmp(s->pl_dev, s->dev[i].path, strlen(s->dev[i].path))) who = s->dev[i].name;
        fprintf(f, "player=%s|%s|%s|%s|%s|%u|%u|%ld\n", s->pl_status, s->pl_title, s->pl_artist, who, s->pl_album,
                s->pl_dur, s->pl_pos, s->pl_pos_at);
    }
    if(s->stream[0]) fprintf(f, "stream=%s\n", s->stream);
    if(last_result[0]) fprintf(f, "result=%s\n", last_result);
    fclose(f);
    rename(tmp, STATE);
}

/* -------------------------------------------------------------- comandos */

static char player_path[128];

static void command(char *line)
{
    line[strcspn(line, "\r\n")] = 0;
    char *arg = strchr(line, ' ');
    if(arg) *arg++ = 0;
    else arg = "";
    char path[128];
    if(!strcmp(line, "on")) {
        int fd = open(FLAG, O_WRONLY | O_CREAT, 0644);
        if(fd >= 0) close(fd);
        reconnect_done = 0;
        powered_at = 0;
        run_service("start", "force");
    } else if(!strcmp(line, "off")) {
        unlink(FLAG);
        run_service("stop", NULL);
    } else if(!adapter[0]) {
        snprintf(last_result, sizeof last_result, "%.32s|erro|desligado", line);
    } else if(!strcmp(line, "visible")) {
        set_bool(adapter, "org.bluez.Adapter1", "Discoverable", atoi(arg));
    } else if(!strcmp(line, "scan")) {
        scan_until = atoi(arg) ? mono_ms() + SCAN_MS : 0;
        call_async(adapter, "org.bluez.Adapter1", atoi(arg) ? "StartDiscovery" : "StopDiscovery", "scan", 5000,
                   DBUS_TYPE_INVALID);
    } else if(!strcmp(line, "accept") || !strcmp(line, "reject")) {
        req_answer(!strcmp(line, "accept"));
    } else if(!strcmp(line, "play") || !strcmp(line, "pause") || !strcmp(line, "next") || !strcmp(line, "prev")) {
        if(player_path[0])
            call_async(player_path, "org.bluez.MediaPlayer1",
                       !strcmp(line, "play") ? "Play" : !strcmp(line, "pause") ? "Pause" :
                       !strcmp(line, "next") ? "Next" : "Previous", "player", 5000, DBUS_TYPE_INVALID);
    } else if(valid_addr(arg)) {
        reconnect_done = 3;                       /* o usuário decide: sem reconexão automática */
        dev_path(path, sizeof path, arg);
        if(!strcmp(line, "pair")) call_async(path, "org.bluez.Device1", "Pair", "pair", 60000, DBUS_TYPE_INVALID);
        else if(!strcmp(line, "connect")) call_async(path, "org.bluez.Device1", "Connect", "connect", 30000, DBUS_TYPE_INVALID);
        else if(!strcmp(line, "disconnect")) call_async(path, "org.bluez.Device1", "Disconnect", "disconnect", 10000, DBUS_TYPE_INVALID);
        else if(!strcmp(line, "forget")) {
            const char *p = path;
            call_async(adapter, "org.bluez.Adapter1", "RemoveDevice", "forget", 10000, DBUS_TYPE_OBJECT_PATH, &p,
                       DBUS_TYPE_INVALID);
        }
    }
    dirty = 1;
}

static void read_fifo(int fd)
{
    static char buf[512];
    static size_t n;
    ssize_t r = read(fd, buf + n, sizeof buf - 1 - n);
    if(r <= 0) return;
    n += (size_t)r;
    buf[n] = 0;
    char *nl;
    while((nl = strchr(buf, '\n'))) {
        *nl = 0;
        command(buf);
        memmove(buf, nl + 1, n - (size_t)(nl + 1 - buf) + 1);
        n -= (size_t)(nl + 1 - buf);
    }
    if(n >= sizeof buf - 1) n = 0;
}

static DBusHandlerResult on_signal(DBusConnection *c, DBusMessage *msg, void *ud)
{
    (void)c; (void)ud;
    if(dbus_message_is_signal(msg, "org.freedesktop.DBus", "NameOwnerChanged")) {
        const char *name, *old, *new_;
        if(dbus_message_get_args(msg, NULL, DBUS_TYPE_STRING, &name, DBUS_TYPE_STRING, &old, DBUS_TYPE_STRING, &new_,
                                 DBUS_TYPE_INVALID) && !strcmp(name, BLUEZ)) {
            agent_ok = 0;                         /* bluetoothd reiniciou: registrar de novo */
            if(*new_) agent_register();
            dirty = 1;
        }
    } else if(dbus_message_get_type(msg) == DBUS_MESSAGE_TYPE_SIGNAL)
        dirty = 1;
    return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

/* O MT6611 sai do reset com a varredura de visibilidade em 2,40 s a cada 2,56 s (94 % do
 * tempo): visível, o áudio A2DP caía de ~41 para ~5 KB/s, com buracos de 2,4 s. Grava o
 * padrão do Bluetooth (janela 11,25 ms, intervalo 2,56 s) ao ligar e a cada vez que fica visível.
 * HCI Write Inquiry Scan Activity (0x0C1E) num soquete HCI cru (docs/bluetooth-audio-
 * performance/08-uart-hci.md). */
static void fix_inquiry_scan(void)
{
    struct { unsigned short family, dev, channel; } a = { 31, 0, 0 };   /* AF_BLUETOOTH, hci0, RAW */
    static const unsigned char cmd[] = { 0x01, 0x1e, 0x0c, 0x04, 0x00, 0x10, 0x12, 0x00 };
    int s = socket(31, SOCK_RAW | SOCK_CLOEXEC, 1);                     /* BTPROTO_HCI */
    if(s < 0) return;
    if(bind(s, (struct sockaddr *)&a, sizeof a) == 0 && write(s, cmd, sizeof cmd) == (ssize_t)sizeof cmd)
        syslog(LOG_INFO, "varredura de visibilidade: 11,25 ms a cada 2,56 s");
    close(s);
}

static void on_sig(int s) { (void)s; quit = 1; }

int main(int argc, char **argv)
{
    int fg = argc > 1 && !strcmp(argv[1], "-f");
    openlog("bfbt-agent", LOG_PID, LOG_DAEMON);
    DBusError e;
    dbus_error_init(&e);
    bus = dbus_bus_get(DBUS_BUS_SYSTEM, &e);
    if(!bus) { fprintf(stderr, "bfbt-agent: D-Bus: %s\n", e.message); return 1; }
    dbus_connection_set_exit_on_disconnect(bus, FALSE);
    static const DBusObjectPathVTable vt = { .message_function = agent_msg };
    dbus_connection_register_object_path(bus, AGENT, &vt, NULL);
    dbus_connection_add_filter(bus, on_signal, NULL, NULL);
    dbus_bus_add_match(bus, "type='signal',sender='org.bluez'", NULL);
    dbus_bus_add_match(bus, "type='signal',interface='org.freedesktop.DBus',member='NameOwnerChanged',arg0='org.bluez'", NULL);

    unlink(FIFO);
    if(mkfifo(FIFO, 0600) < 0 && errno != EEXIST) { perror(FIFO); return 1; }
    /* O_RDWR: o FIFO nunca dá fim de arquivo quando o shell fecha a escrita */
    int ff = open(FIFO, O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if(ff < 0) { perror(FIFO); return 1; }
    if(!fg && daemon(0, 0) < 0) { perror("daemon"); return 1; }
    signal(SIGCHLD, SIG_IGN);
    signal(SIGPIPE, SIG_IGN);
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_sig;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);

    agent_register();
    int dfd = -1;
    dbus_connection_get_unix_fd(bus, &dfd);
    long next_full = 0, last_write = 0;
    state_t st;
    while(!quit && dbus_connection_get_is_connected(bus)) {
        long now = mono_ms();
        if(dirty && now - last_write >= 250) {          /* agrupa rajadas de sinais */
            dirty = 0;
            last_write = now;
            if(!agent_ok) agent_register();
            read_state(&st);
            snprintf(player_path, sizeof player_path, "%s", st.pl_path);
            write_state(&st);
            if(!st.powered) powered_at = 0;
            else if(!powered_at) { powered_at = now; fix_inquiry_scan(); }
            /* a inicialização do adaptador pode desfazer a gravação: repete ao ficar visível,
             * que é quando a varredura passa a rodar */
            static int was_visible;
            if(st.discoverable && !was_visible) fix_inquiry_scan();
            was_visible = st.discoverable;
        }
        /* reconexão aos pareados e confiáveis: 3 s, 15 s e 60 s depois de ligar (o BlueALSA
         * precisa ter registrado o áudio antes, senão "profile-unavailable") */
        static const long when[] = { 3000, 15000, 60000 };
        if(powered_at && reconnect_done < 3 && now - powered_at >= when[reconnect_done]) {
            reconnect_done++;
            for(int i = 0; i < st.ndev; i++)
                if(st.dev[i].paired && st.dev[i].trusted && !st.dev[i].connected)
                    call_async(st.dev[i].path, "org.bluez.Device1", "Connect", "connect", 30000, DBUS_TYPE_INVALID);
            dirty = 1;
        }
        if(scan_until && now >= scan_until) {
            scan_until = 0;
            if(adapter[0]) call_async(adapter, "org.bluez.Adapter1", "StopDiscovery", "scan", 5000, DBUS_TYPE_INVALID);
        }
        if(now >= next_full) { dirty = 1; next_full = now + 10000; }   /* RSSI, marca, segurança */
        struct pollfd p[2] = { { dfd, POLLIN, 0 }, { ff, POLLIN, 0 } };
        int timeout = dirty ? 250 : (int)(next_full - now);
        if(powered_at && reconnect_done < 3 && timeout > 1000) timeout = 1000;
        if(scan_until && timeout > 1000) timeout = 1000;
        if(timeout < 0) timeout = 0;
        if(dbus_connection_get_dispatch_status(bus) == DBUS_DISPATCH_DATA_REMAINS) timeout = 0;
        if(dbus_connection_has_messages_to_send(bus)) p[0].events |= POLLOUT;
        poll(p, 2, timeout);
        if(p[1].revents & POLLIN) read_fifo(ff);
        dbus_connection_read_write(bus, 0);
        while(dbus_connection_dispatch(bus) == DBUS_DISPATCH_DATA_REMAINS) {}
    }
    if(req_msg) req_answer(0);
    unlink(FIFO);
    unlink(STATE);
    return 0;
}
