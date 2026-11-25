/* bfbt-agent — ponte entre o BlueZ (D-Bus) e o shell do BigFoston, que é estático e não
 * fala D-Bus (docs/hardware-enablement/08-bluetooth-profiles.md).
 *
 *  - agente de pareamento "DisplayYesNo": o código aparece na tela do FOSTON e o usuário
 *    confirma; teclados: o código aparece para digitar no teclado (DisplayPasskey, ou um PIN
 *    sorteado nos antigos); o que o usuário pareia fica confiável (reconecta sozinho) e os
 *    pareamentos vão para o cartão na hora; serviços de aparelhos pareados e confiáveis são
 *    autorizados direto;
 *  - estado em /run/bigfoston-bt (chave=valor, reescrito por rename a cada mudança);
 *  - comandos, um por linha, no FIFO /run/bigfoston-bt.cmd:
 *      on | off                       liga/desliga o Bluetooth (S35bluetooth + marca)
 *      visible 0|1 | scan 0|1         visível por DiscoverableTimeout / procurar aparelhos (20 s)
 *      pair|connect|disconnect|forget AA:BB:CC:DD:EE:FF
 *      accept | reject                responde o pedido de pareamento pendente
 *      play | pause | next | prev     controla a música do celular (AVRCP)
 *      repeat off|singletrack|alltracks | shuffle off|alltracks
 *  - ao ligar, conecta uma vez os aparelhos pareados e confiáveis (como um rádio de carro).
 *
 * SPDX-License-Identifier: GPL-3.0-or-later */
#define _GNU_SOURCE
#include <dbus/dbus.h>
#include <arpa/inet.h>
#include <net/if.h>
#include <netinet/in.h>
#include <strings.h>
#include <sys/wait.h>
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
#include <sys/ioctl.h>
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
    int paired, connected, trusted, rssi, has_rssi, resolved;
    int has_nap, pan_on;                  /* compartilha internet (NAP); Network1 conectado */
    char iface[16];                       /* Network1.Interface (bnep0) */
} dev_t_;

static DBusConnection *bus;
/* internet pelo celular (pan_tick) */
#define INET_OFF "/data/bigfoston/bt-internet-desligada"
#define MAXPAN 4
static struct { char addr[18]; long next_try; int pending; pid_t dhcp; char iface[16]; } pan[MAXPAN];
static volatile sig_atomic_t quit;
static int dirty = 1, agent_ok, reconnect_done;   /* reconnect_done: tentativas feitas */
static long save_soon;                              /* salvar os pareamentos (mono_ms) */
static long hid_fix_until, hid_fix_next, hid_conn_at;  /* DualShock 4 recém-pareado (ds4_fix_all) */
static char hid_conn_path[128];
static long powered_at;
static long scan_until;                             /* busca: encerra sozinha (rouba o rádio do áudio) */
#define SCAN_MS 20000                             /* quando o adaptador apareceu ligado */
static char adapter[64];                 /* /org/bluez/hci0 ou "" */
static char last_result[160];

/* pedido de pareamento esperando o usuário */
static DBusMessage *req_msg;
static char req_kind[16], req_dev[96];
static unsigned req_pass;
static int req_display;                  /* só mostra um código (já respondido): "passkey" */
static char pair_path[128];              /* aparelho do último "pair" pedido pelo usuário */
static char trusted_done[MAXDEV][96];    /* teclados pareados já marcados confiáveis */

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

/* ------------------------------------------- DualShock 4 sem o driver da Sony
 * O kernel não tem o hid-playstation (falta espaço na flash) e o hid-generic usa o descritor
 * HID que o BlueZ guardou do SDP do controle. Nele só o relatório 0x01 (o curto) tem botões e
 * eixos; o 0x11, o que o DualShock 4 manda pelo Bluetooth (78 bytes, com giroscópio e
 * bateria), está declarado como dados do fabricante: o controle conecta e nenhum botão vira
 * evento. Aqui o registro SDP no cache do BlueZ ganha o mesmo descritor com os campos do 0x01
 * declarados no 0x11 (depois dos 2 bytes de cabeçalho dele) e o 0x01 sem campos (o hid-input
 * ignora um uso repetido). O tamanho não muda (431 bytes): nenhum comprimento do SDP mexe.
 * Os botões e eixos saem iguais aos de antes (0x130..0x13d, X Y Z Rz Rx Ry, direcional), então
 * a linha do SDL GameControllerDB continua valendo. O BlueZ lê o cache ao iniciar: corrigido
 * pelo S35bluetooth antes do bluetoothd subir e, depois de parear, com o bluetoothd reiniciado. */
#define BT_LIB "/var/lib/bluetooth"
static const char ds4_old01[] = "85010930093109320935150026FF0075089504810209391500250775049501814205091901290E"
                                "150025017501950E8102750695018101050109330934150026FF00750895028102";
static const char ds4_new01[] = "150026FF007508";
static const char ds4_old11[] = "85110920150026FF007508954D810209219102";
static const char ds4_new11[] = "851109209502810205010930093109320935950481020939250775049501814205091901290E"
                                "25017501950E810275069501810105010933093426FF007508950281020600FF092195428102"
                                "954D09219102A4B4";

/* uma ocorrência só de pat em s (NULL se nenhuma ou mais de uma) */
static char *once(char *s, const char *pat)
{
    char *a = strstr(s, pat);
    return a && !strstr(a + 1, pat) ? a : NULL;
}

/* corrige um arquivo de cache; 1 = corrigido, 0 = nada a fazer, -1 = erro */
static int ds4_fix_file(const char *path)
{
    FILE *f = fopen(path, "r");
    if(!f) return 0;
    static char buf[65536];
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[n] = 0;
    if(n == sizeof buf - 1) return 0;
    char *a = once(buf, ds4_old01), *b = once(buf, ds4_old11);
    if(!a || !b || b < a) return 0;
    /* os dois trechos na mesma linha (o mesmo registro SDP) */
    for(char *c = a; c < b; c++) if(*c == '\n') return 0;
    static char out[65536 + 128];
    size_t k = 0;
    size_t la = strlen(ds4_old01), lb = strlen(ds4_old11);
    memcpy(out + k, buf, (size_t)(a - buf)); k += (size_t)(a - buf);
    memcpy(out + k, ds4_new01, strlen(ds4_new01)); k += strlen(ds4_new01);
    memcpy(out + k, a + la, (size_t)(b - a - (long)la)); k += (size_t)(b - a - (long)la);
    memcpy(out + k, ds4_new11, strlen(ds4_new11)); k += strlen(ds4_new11);
    memcpy(out + k, b + lb, n - (size_t)(b + lb - buf)); k += n - (size_t)(b + lb - buf);
    if(k != n) return -1;                                  /* o tamanho tem de ser o mesmo */
    char tmp[760];
    snprintf(tmp, sizeof tmp, "%s.novo", path);
    FILE *w = fopen(tmp, "w");
    if(!w) return -1;
    int ok = fwrite(out, 1, k, w) == k;
    if(fclose(w) || !ok || rename(tmp, path)) { unlink(tmp); return -1; }
    chmod(path, 0600);
    return 1;
}

/* todos os caches de todos os adaptadores; devolve quantos foram corrigidos */
static int ds4_fix_all(const char *root)
{
    int fixed = 0;
    DIR *ad = opendir(root);
    if(!ad) return 0;
    struct dirent *e;
    while((e = readdir(ad))) {
        if(e->d_name[0] == '.') continue;
        char cdir[400];
        snprintf(cdir, sizeof cdir, "%s/%s/cache", root, e->d_name);
        DIR *cd = opendir(cdir);
        if(!cd) continue;
        struct dirent *c;
        while((c = readdir(cd))) {
            if(c->d_name[0] == '.' || strstr(c->d_name, ".novo")) continue;
            char p[700];
            snprintf(p, sizeof p, "%s/%s", cdir, c->d_name);
            int r = ds4_fix_file(p);
            if(r > 0) { fixed++; syslog(LOG_INFO, "DualShock 4: descritor HID corrigido (%s)", c->d_name); }
            else if(r < 0) syslog(LOG_ERR, "DualShock 4: não foi possível corrigir %s", c->d_name);
        }
        closedir(cd);
    }
    closedir(ad);
    return fixed;
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

static void req_clear(void);
static int inet_off(const char *addr);
static void inet_set(const char *addr, int on);
static int pan_slot(const char *addr, int make);
static void iface_ip(const char *ifc, char *out, size_t n);
static unsigned dev_class(const char *path);
static void call_async(const char *path, const char *iface, const char *method, const char *what, int timeout_ms,
                       int first_type, ...);
static void set_bool(const char *path, const char *iface, const char *prop, int v);

static void call_done(DBusPendingCall *pc, void *ud)
{
    const char *what = ud;
    DBusMessage *r = dbus_pending_call_steal_reply(pc);
    if(!strcmp(what, "reconnect")) {             /* automática: não apaga o resultado do usuário */
        if(r) dbus_message_unref(r);
        dbus_pending_call_unref(pc);
        dirty = 1;
        return;
    }
    int failed = r && dbus_message_get_type(r) == DBUS_MESSAGE_TYPE_ERROR;
    if(!strcmp(what, "pair") || !strcmp(what, "forget")) {
        if(!strcmp(what, "pair") && !failed && pair_path[0]) {
            /* pareado pelo usuário: confiável (reconecta e é autorizado sem perguntar) */
            set_bool(pair_path, "org.bluez.Device1", "Trusted", 1);
            /* periférico (teclado, mouse, controle de jogo): o Pair só cria o vínculo; o
             * canal HID abre com o Connect. O controle do PlayStation espera o computador
             * conectar logo depois de parear e, se ninguém conecta, desliga ("Pareado",
             * nunca "Conectado"). Teclado que já conectou sozinho: AlreadyConnected, sem efeito */
            if(((dev_class(pair_path) >> 8) & 0x1f) == 0x05) {
                call_async(pair_path, "org.bluez.Device1", "Connect", "reconnect", 30000, DBUS_TYPE_INVALID);
                /* DualShock 4: o BlueZ grava o SDP no cache logo depois; corrigir o descritor */
                hid_fix_until = mono_ms() + 15000;
                hid_fix_next = mono_ms() + 1500;
                snprintf(hid_conn_path, sizeof hid_conn_path, "%s", pair_path);
            }
        }
        if(!failed) save_soon = mono_ms() + 1500;     /* pareamentos para o cartão */
        if(req_display) req_clear();                   /* o código já não vale */
    }
    if(failed) {
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
    if(strcmp(what, "reconnect")) snprintf(last_result, sizeof last_result, "%s|andamento|", what);
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

/* Properties.Set de uma string, sem esperar; o resultado vai para "result=" (what) */
static void set_string_async(const char *path, const char *iface, const char *prop, const char *v, const char *what)
{
    DBusMessage *m = dbus_message_new_method_call(BLUEZ, path, "org.freedesktop.DBus.Properties", "Set");
    if(!m) return;
    DBusMessageIter it, var;
    dbus_message_iter_init_append(m, &it);
    dbus_message_iter_append_basic(&it, DBUS_TYPE_STRING, &iface);
    dbus_message_iter_append_basic(&it, DBUS_TYPE_STRING, &prop);
    dbus_message_iter_open_container(&it, DBUS_TYPE_VARIANT, "s", &var);
    dbus_message_iter_append_basic(&var, DBUS_TYPE_STRING, &v);
    dbus_message_iter_close_container(&it, &var);
    DBusPendingCall *pc = NULL;
    if(dbus_connection_send_with_reply(bus, m, &pc, 5000) && pc)
        dbus_pending_call_set_notify(pc, call_done, (void *)what, NULL);
    dbus_message_unref(m);
    snprintf(last_result, sizeof last_result, "%s|andamento|", what);
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
    req_display = 0;
    req_kind[0] = req_dev[0] = 0;
    dirty = 1;
}

static void req_answer(int yes)
{
    if(req_display) {                              /* código na tela: só dá para cancelar */
        if(!yes && req_dev[0]) call_async(req_dev, "org.bluez.Device1", "CancelPairing", "cancel", 5000, DBUS_TYPE_INVALID);
        req_clear();
        return;
    }
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

/* mostra um código para digitar no aparelho (teclado); a resposta ao BlueZ já foi */
static void disp_set(const char *dev, unsigned pass)
{
    if(req_msg) req_answer(0);
    req_display = 1;
    snprintf(req_kind, sizeof req_kind, "passkey");
    snprintf(req_dev, sizeof req_dev, "%s", dev);
    req_pass = pass;
    dirty = 1;
}

static unsigned random6(void)
{
    unsigned v = 0;
    int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if(fd >= 0) { if(read(fd, &v, sizeof v) != (ssize_t)sizeof v) v = 0; close(fd); }
    if(!v) v = (unsigned)mono_ms() * 2654435761u;
    return v % 1000000u;
}

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
    } else if(!strcmp(mem, "DisplayPasskey")) {
        /* teclado: o FOSTON mostra, o usuário digita no teclado e aperta Enter (chamado de
         * novo a cada tecla, com o mesmo código) */
        dbus_uint16_t entered = 0;
        if(dbus_message_get_args(msg, NULL, DBUS_TYPE_OBJECT_PATH, &dev, DBUS_TYPE_UINT32, &pass,
                                 DBUS_TYPE_UINT16, &entered, DBUS_TYPE_INVALID)) {
            if(!req_display || req_pass != pass || strcmp(req_dev, dev)) {
                syslog(LOG_INFO, "pareamento: código para digitar no aparelho");
                disp_set(dev, pass);
            }
        }
        reply_simple(msg, NULL);
    } else if(!strcmp(mem, "DisplayPinCode")) {
        const char *pin = NULL;
        if(dbus_message_get_args(msg, NULL, DBUS_TYPE_OBJECT_PATH, &dev, DBUS_TYPE_STRING, &pin, DBUS_TYPE_INVALID) && pin)
            disp_set(dev, (unsigned)strtoul(pin, NULL, 10));
        reply_simple(msg, NULL);
    } else if(!strcmp(mem, "RequestPinCode") &&
              dbus_message_get_args(msg, NULL, DBUS_TYPE_OBJECT_PATH, &dev, DBUS_TYPE_INVALID)) {
        /* aparelho antigo (antes do Bluetooth 2.1): teclado digita um PIN sorteado e mostrado
         * aqui; fones e caixas de som usam o PIN fixo de fábrica, 0000 */
        unsigned cls = dev_class(dev);
        int kbd = ((cls >> 8) & 0x1f) == 0x05 && (cls & 0x40);
        char pin[8];
        if(kbd) { unsigned v = random6(); snprintf(pin, sizeof pin, "%06u", v); disp_set(dev, v); }
        else snprintf(pin, sizeof pin, "0000");
        const char *pp = pin;
        DBusMessage *r = dbus_message_new_method_return(msg);
        if(r) {
            dbus_message_append_args(r, DBUS_TYPE_STRING, &pp, DBUS_TYPE_INVALID);
            dbus_connection_send(bus, r, NULL);
            dbus_message_unref(r);
        }
    } else if(!strcmp(mem, "Release")) {
        reply_simple(msg, NULL);
    } else {
        /* RequestPasskey: o aparelho mostra um código para digitar aqui (o FOSTON não tem
         * teclado de pareamento; não acontece com DisplayYesNo) */
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

/* Device1.Class (0 = desconhecida) */
static unsigned dev_class(const char *path)
{
    const char *iface = "org.bluez.Device1", *prop = "Class";
    DBusMessage *m = dbus_message_new_method_call(BLUEZ, path, "org.freedesktop.DBus.Properties", "Get");
    if(!m) return 0;
    dbus_message_append_args(m, DBUS_TYPE_STRING, &iface, DBUS_TYPE_STRING, &prop, DBUS_TYPE_INVALID);
    DBusMessage *r = dbus_connection_send_with_reply_and_block(bus, m, 2000, NULL);
    dbus_message_unref(m);
    if(!r) return 0;
    dbus_uint32_t v = 0;
    DBusMessageIter it, var;
    if(dbus_message_iter_init(r, &it) && dbus_message_iter_get_arg_type(&it) == DBUS_TYPE_VARIANT) {
        dbus_message_iter_recurse(&it, &var);
        if(dbus_message_iter_get_arg_type(&var) == DBUS_TYPE_UINT32) dbus_message_iter_get_basic(&var, &v);
    }
    dbus_message_unref(r);
    return v;
}

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
    char pl_repeat[16], pl_shuffle[16];   /* AVRCP: off/singletrack/alltracks/group; "" sem suporte */
    char pl_genre[48];
    unsigned pl_trackno, pl_ntracks;
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
        is_pl = !strcmp(iface, "org.bluez.MediaPlayer1"), is_tr = !strcmp(iface, "org.bluez.MediaTransport1"),
        is_net = !strcmp(iface, "org.bluez.Network1");
    if(!is_ad && !is_dev && !is_pl && !is_tr && !is_net) return;
    dev_t_ *d = NULL;
    if(is_ad) {
        if(adapter[0] && strcmp(adapter, path)) return;
        s->present = 1;
        snprintf(adapter, sizeof adapter, "%s", path);
    }
    if(is_pl) { snprintf(s->pl_path, sizeof s->pl_path, "%s", path); s->pl_pos_at = mono_ms(); }
    if(is_dev || is_net) {                     /* Device1 e Network1 no mesmo objeto, em qualquer ordem */
        for(int i = 0; i < s->ndev && !d; i++) if(!strcmp(s->dev[i].path, path)) d = &s->dev[i];
        if(!d) {
            if(s->ndev >= MAXDEV) return;
            d = &s->dev[s->ndev++];
            memset(d, 0, sizeof *d);
            snprintf(d->path, sizeof d->path, "%s", path);
        }
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
        } else if(is_net) {
            if(!strcmp(k, "Connected")) var_bool(&var, &d->pan_on);
            else if(!strcmp(k, "Interface")) var_str(&var, d->iface, sizeof d->iface);
        } else if(is_dev) {
            if(!strcmp(k, "UUIDs") && dbus_message_iter_get_arg_type(&var) == DBUS_TYPE_ARRAY) {
                DBusMessageIter ue;
                dbus_message_iter_recurse(&var, &ue);
                for(; dbus_message_iter_get_arg_type(&ue) == DBUS_TYPE_STRING; dbus_message_iter_next(&ue)) {
                    const char *u;
                    dbus_message_iter_get_basic(&ue, &u);
                    if(!strncasecmp(u, "00001116-", 9)) d->has_nap = 1;     /* NAP: compartilha a rede */
                }
            }
            else if(!strcmp(k, "Address")) var_str(&var, d->addr, sizeof d->addr);
            else if(!strcmp(k, "Alias")) var_str(&var, d->name, sizeof d->name);
            else if(!strcmp(k, "Icon")) var_str(&var, d->icon, sizeof d->icon);
            else if(!strcmp(k, "Paired")) var_bool(&var, &d->paired);
            else if(!strcmp(k, "Connected")) var_bool(&var, &d->connected);
            else if(!strcmp(k, "Trusted")) var_bool(&var, &d->trusted);
            else if(!strcmp(k, "ServicesResolved")) var_bool(&var, &d->resolved);
            else if(!strcmp(k, "RSSI")) { dbus_int16_t r = 0; var_get(&var, DBUS_TYPE_INT16, &r); d->rssi = r; d->has_rssi = 1; }
        } else if(is_tr) {
            char st[12] = "";
            if(!strcmp(k, "State")) var_str(&var, st, sizeof st);
            /* um transporte ativo vale mais que outro parado (dois aparelhos) */
            if(st[0] && (strcmp(s->stream, "active") || !strcmp(st, "active"))) snprintf(s->stream, sizeof s->stream, "%s", st);
        } else {
            if(!strcmp(k, "Status")) var_str(&var, s->pl_status, sizeof s->pl_status);
            else if(!strcmp(k, "Position")) var_get(&var, DBUS_TYPE_UINT32, &s->pl_pos);
            else if(!strcmp(k, "Repeat")) var_str(&var, s->pl_repeat, sizeof s->pl_repeat);
            else if(!strcmp(k, "Shuffle")) var_str(&var, s->pl_shuffle, sizeof s->pl_shuffle);
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
                    else if(!strcmp(tk, "Genre")) var_str(&tv, s->pl_genre, sizeof s->pl_genre);
                    else if(!strcmp(tk, "TrackNumber")) var_get(&tv, DBUS_TYPE_UINT32, &s->pl_trackno);
                    else if(!strcmp(tk, "NumberOfTracks")) var_get(&tv, DBUS_TYPE_UINT32, &s->pl_ntracks);
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
    for(int i = 0; i < s->ndev; i++) {               /* "pan=endereço|quer|conectado|ip" */
        if(!s->dev[i].has_nap || !s->dev[i].paired) continue;
        char ip[24] = "";
        if(s->dev[i].pan_on && s->dev[i].iface[0]) iface_ip(s->dev[i].iface, ip, sizeof ip);
        fprintf(f, "pan=%s|%d|%d|%s\n", s->dev[i].addr, !inet_off(s->dev[i].addr), s->dev[i].pan_on, ip);
    }
    if(req_msg || req_display) {
        const char *name = "", *addr = "";
        for(int i = 0; i < s->ndev; i++)
            if(!strcmp(s->dev[i].path, req_dev)) { name = s->dev[i].name; addr = s->dev[i].addr; }
        fprintf(f, "req=%s|%06u|%s|%s\n", req_kind, req_pass, addr, name);
    }
    if(s->pl_status[0]) {
        const char *who = "";
        for(int i = 0; i < s->ndev; i++)
            if(!strncmp(s->pl_dev, s->dev[i].path, strlen(s->dev[i].path))) who = s->dev[i].name;
        fprintf(f, "player=%s|%s|%s|%s|%s|%u|%u|%ld|%s|%s|%s|%u|%u\n", s->pl_status, s->pl_title, s->pl_artist, who,
                s->pl_album, s->pl_dur, s->pl_pos, s->pl_pos_at, s->pl_repeat, s->pl_shuffle, s->pl_genre,
                s->pl_trackno, s->pl_ntracks);
    }
    if(s->stream[0]) fprintf(f, "stream=%s\n", s->stream);
    if(last_result[0]) fprintf(f, "result=%s\n", last_result);
    fclose(f);
    rename(tmp, STATE);
}

/* Teclados (e mouses) pareados mas não confiáveis: o BlueZ recusa a reconexão quando acordam
 * e o agente perguntaria a cada vez. Pareados aqui pelo usuário: ficam confiáveis (uma vez por
 * aparelho; quem desmarcar por fora não é remarcado nesta execução). O código mostrado para
 * digitar some quando o aparelho fica pareado. */
static void trust_inputs(const state_t *s)
{
    for(int i = 0; i < s->ndev; i++) {
        const dev_t_ *d = &s->dev[i];
        if(req_display && d->paired && !strcmp(d->path, req_dev)) { req_clear(); save_soon = mono_ms() + 1500; }
        if(!d->paired || d->trusted || strncmp(d->icon, "input-", 6)) continue;
        int seen = 0, free_ = -1;
        for(int k = 0; k < MAXDEV; k++) {
            if(!strcmp(trusted_done[k], d->path)) seen = 1;
            else if(free_ < 0 && !trusted_done[k][0]) free_ = k;
        }
        if(seen) continue;
        if(free_ >= 0) snprintf(trusted_done[free_], sizeof trusted_done[free_], "%s", d->path);
        syslog(LOG_INFO, "teclado/mouse/controle pareado marcado como confiável");
        set_bool(d->path, "org.bluez.Device1", "Trusted", 1);
        save_soon = mono_ms() + 1500;
        dirty = 1;
    }
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
    } else if(!strcmp(line, "internet")) {            /* "internet ENDEREÇO 1|0" */
        char a[18] = "";
        int on = 1;
        if(sscanf(arg, "%17s %d", a, &on) >= 1 && valid_addr(a)) {
            inet_set(a, on);
            int k = pan_slot(a, 0);
            if(k >= 0) pan[k].next_try = 0;             /* tenta já */
        }
    } else if((!strcmp(line, "repeat") || !strcmp(line, "shuffle")) && player_path[0]) {
        /* AVRCP: repeat off|singletrack|alltracks, shuffle off|alltracks */
        static const char *ok[] = { "off", "singletrack", "alltracks", "group", NULL };
        for(int i = 0; ok[i]; i++)
            if(!strcmp(arg, ok[i]))
                set_string_async(player_path, "org.bluez.MediaPlayer1", !strcmp(line, "repeat") ? "Repeat" : "Shuffle", arg,
                                 !strcmp(line, "repeat") ? "repeat" : "shuffle");
    } else if(valid_addr(arg)) {
        reconnect_done = 3;                       /* o usuário decide: sem reconexão automática */
        dev_path(path, sizeof path, arg);
        if(!strcmp(line, "pair")) {
            snprintf(pair_path, sizeof pair_path, "%s", path);
            call_async(path, "org.bluez.Device1", "Pair", "pair", 60000, DBUS_TYPE_INVALID);
        }
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

/* Ajustes do MT6611 gravados a cada vez que o adaptador liga (a inicialização do BlueZ não os
 * faz), por comandos HCI num soquete cru (docs/bluetooth-integration/03-bluez-stack.md):
 *  - Write Inquiry Scan Activity (0x0C1E): o chip sai do reset varrendo a visibilidade 2,40 s a
 *    cada 2,56 s; visível, o A2DP caía de ~41 para ~5 KB/s. Padrão do Bluetooth: 11,25 ms;
 *  - Write Default Link Policy Settings (0x080F) = só sniff, sem troca de papel: quando o
 *    celular pedia para virar mestre no meio da abertura do áudio, o chip perdia a resposta do
 *    AVDTP Discover e a conexão falhava (POCO X4: 0/5 conexões; sem a troca: 5/5). */
static void hci_raw(const unsigned char *cmd, size_t n, const char *what)
{
    struct { unsigned short family, dev, channel; } a = { 31, 0, 0 };   /* AF_BLUETOOTH, hci0, RAW */
    int s = socket(31, SOCK_RAW | SOCK_CLOEXEC, 1);                     /* BTPROTO_HCI */
    if(s < 0) return;
    if(bind(s, (struct sockaddr *)&a, sizeof a) == 0 && write(s, cmd, n) == (ssize_t)n)
        syslog(LOG_INFO, "%s", what);
    close(s);
}

static void fix_inquiry_scan(void)
{
    static const unsigned char scan[] = { 0x01, 0x1e, 0x0c, 0x04, 0x00, 0x10, 0x12, 0x00 };
    static const unsigned char policy[] = { 0x01, 0x0f, 0x08, 0x02, 0x04, 0x00 };
    hci_raw(scan, sizeof scan, "varredura de visibilidade: 11,25 ms a cada 2,56 s");
    hci_raw(policy, sizeof policy, "politica de enlace: sem troca de papel");
}

/* Com a conexão pronta, o celular passa a ser o mestre do enlace: o mestre controla o AFH (fugir
 * dos canais com interferência) e o MT6611 faz isso mal. Com o FOSTON mestre, a qualidade do enlace
 * caía de 255 para 8 com Wi-Fi por perto, o celular usava pacotes DH5 sem EDR e o áudio chegava a
 * 18,7 KB/s (59 % do tempo real: cortes e trechos pulados); com o celular mestre: 41,9 KB/s,
 * qualidade 255 (22/11/2026). A troca é pedida só depois dos perfis conectados, porque durante a
 * abertura do áudio o chip perdia a resposta do AVDTP (política padrão sem troca de papel). */
#define HCIGETCONNINFO _IOR('H', 213, int)
struct conn_info { uint16_t handle; uint8_t bdaddr[6], type, out; uint16_t state; uint32_t link_mode; };
struct conn_info_req { uint8_t bdaddr[6], type; struct conn_info info; };

static void become_peripheral(const char *addr)
{
    unsigned v[6];
    if(sscanf(addr, "%x:%x:%x:%x:%x:%x", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) != 6) return;
    struct { unsigned short family, dev, channel; } a = { 31, 0, 0 };
    int s = socket(31, SOCK_RAW | SOCK_CLOEXEC, 1);
    if(s < 0) return;
    struct conn_info_req r;
    memset(&r, 0, sizeof r);
    for(int i = 0; i < 6; i++) r.bdaddr[i] = (uint8_t)v[5 - i];
    r.type = 1;                                          /* ACL */
    if(bind(s, (struct sockaddr *)&a, sizeof a) == 0 && ioctl(s, HCIGETCONNINFO, &r) == 0) {
        if(!(r.info.link_mode & 0x0001)) {               /* HCI_LM_MASTER: já é escravo */
            close(s);
            return;
        }
        uint16_t h = r.info.handle;
        unsigned char pol[] = { 0x01, 0x0d, 0x08, 0x04, (uint8_t)h, (uint8_t)(h >> 8), 0x05, 0x00 };
        unsigned char sw[] = { 0x01, 0x0b, 0x08, 0x07, r.bdaddr[0], r.bdaddr[1], r.bdaddr[2], r.bdaddr[3],
                               r.bdaddr[4], r.bdaddr[5], 0x01 };
        if(write(s, pol, sizeof pol) == (ssize_t)sizeof pol && write(s, sw, sizeof sw) == (ssize_t)sizeof sw)
            syslog(LOG_INFO, "troca de papel pedida: o celular passa a mestre do enlace");
    }
    close(s);
}

/* Teclado e controle de jogo em tempo real (medido em 24/11/2026 com o DualShock 4):
 *  - o controle pede o modo sniff quando fica parado e, com o MT6611, o enlace em sniff chega
 *    a ~3 relatórios/s, com buracos de até 2 s: o atraso "oscila" (bom ativo, ruim em sniff).
 *    A política do enlace sem sniff (0x080D = 0) faz o chip recusar o pedido, e Exit Sniff
 *    (0x0804) tira o enlace do sniff se já entrou;
 *  - ativo, o Tpoll padrão (40 slots, 25 ms) limitava a ~38 relatórios/s. QoS Setup (0x0807)
 *    "garantido", latência 1250 us: ~62 relatórios/s, 99% em menos de 28 ms.
 * Uma vez por conexão, 1 s depois de conectado. Gasta mais bateria do teclado/controle. */
static void hid_link_fast(const char *addr)
{
    unsigned v[6];
    if(sscanf(addr, "%x:%x:%x:%x:%x:%x", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) != 6) return;
    struct { unsigned short family, dev, channel; } a = { 31, 0, 0 };
    int s = socket(31, SOCK_RAW | SOCK_CLOEXEC, 1);
    if(s < 0) return;
    struct conn_info_req r;
    memset(&r, 0, sizeof r);
    for(int i = 0; i < 6; i++) r.bdaddr[i] = (uint8_t)v[5 - i];
    r.type = 1;                                          /* ACL */
    if(bind(s, (struct sockaddr *)&a, sizeof a) == 0 && ioctl(s, HCIGETCONNINFO, &r) == 0) {
        uint16_t h = r.info.handle;
        uint8_t hl = (uint8_t)h, hh = (uint8_t)(h >> 8);
        unsigned char pol[] = { 0x01, 0x0d, 0x08, 0x04, hl, hh, 0x00, 0x00 };
        unsigned char ex[] = { 0x01, 0x04, 0x08, 0x02, hl, hh };
        unsigned char qos[] = { 0x01, 0x07, 0x08, 0x14, hl, hh, 0x00, 0x02,
                                0x10, 0x27, 0x00, 0x00,      /* taxa: 10000 B/s */
                                0x00, 0x00, 0x00, 0x00,      /* pico: sem limite */
                                0xe2, 0x04, 0x00, 0x00,      /* latência: 1250 us */
                                0xff, 0xff, 0xff, 0xff };    /* variação: qualquer */
        int ok = write(s, pol, sizeof pol) == (ssize_t)sizeof pol;
        usleep(20000);
        ok &= write(s, ex, sizeof ex) == (ssize_t)sizeof ex;   /* fora do sniff dá "não permitido": tudo bem */
        usleep(20000);
        ok &= write(s, qos, sizeof qos) == (ssize_t)sizeof qos;
        if(ok) syslog(LOG_INFO, "teclado/controle: enlace sem sniff e com latência baixa");
    }
    close(s);
}

#define MAXHL 4
static struct { char addr[18]; long since; int done; } hl[MAXHL];

static void hid_link_tick(const state_t *st, long now)
{
    for(int k = 0; k < MAXHL; k++) {                     /* esquece quem desconectou */
        if(!hl[k].addr[0]) continue;
        int still = 0;
        for(int i = 0; i < st->ndev; i++)
            if(!strcmp(st->dev[i].addr, hl[k].addr) && st->dev[i].connected) still = 1;
        if(!still) hl[k].addr[0] = 0;
    }
    for(int i = 0; i < st->ndev; i++) {
        const dev_t_ *d = &st->dev[i];
        if(!d->connected || strncmp(d->icon, "input-", 6)) continue;
        int k, freek = -1;
        for(k = 0; k < MAXHL && strcmp(hl[k].addr, d->addr); k++)
            if(!hl[k].addr[0] && freek < 0) freek = k;
        if(k == MAXHL) {
            if(freek < 0) continue;
            k = freek;
            snprintf(hl[k].addr, sizeof hl[k].addr, "%s", d->addr);
            hl[k].since = now;
            hl[k].done = 0;
        }
        if(!hl[k].done && now - hl[k].since >= 1000) {
            hl[k].done = 1;
            hid_link_fast(d->addr);
        }
    }
}

/* um pedido por conexão, 2 s depois de os perfis estarem conectados */
#define MAXRS 8
static struct { char addr[18]; long since; int done; } rs[MAXRS];

static void role_switch_tick(const state_t *st, long now)
{
    for(int k = 0; k < MAXRS; k++) {                     /* esquece quem desconectou */
        if(!rs[k].addr[0]) continue;
        int still = 0;
        for(int i = 0; i < st->ndev; i++)
            if(!strcmp(st->dev[i].addr, rs[k].addr) && st->dev[i].connected && st->dev[i].resolved) still = 1;
        if(!still) rs[k].addr[0] = 0;
    }
    for(int i = 0; i < st->ndev; i++) {
        const dev_t_ *d = &st->dev[i];
        if(!d->connected || !d->resolved) continue;
        /* teclado, mouse e controle: o FOSTON segue mestre. A troca serve ao áudio (AFH); com
         * vários aparelhos de entrada mestres o FOSTON viraria escravo de várias piconets */
        if(!strncmp(d->icon, "input-", 6)) continue;
        int k, freek = -1;
        for(k = 0; k < MAXRS && strcmp(rs[k].addr, d->addr); k++)
            if(!rs[k].addr[0] && freek < 0) freek = k;
        if(k == MAXRS) {
            if(freek < 0) continue;
            k = freek;
            snprintf(rs[k].addr, sizeof rs[k].addr, "%s", d->addr);
            rs[k].since = now;
            rs[k].done = 0;
        }
        /* 2 s depois de conectado e, depois, a cada 30 s: o celular às vezes desfaz a troca
         * (24/11/2026: FOSTON mestre de novo, qualidade 3/255 e a música sem som). Se já é
         * escravo, become_peripheral não faz nada */
        if(now - rs[k].since >= 2000 && (!rs[k].done || now - rs[k].since >= 30000)) {
            rs[k].done = 1;
            rs[k].since = now - 2000;
            become_peripheral(d->addr);
        }
    }
}

/* ------------------------------------------------ internet pelo celular (Bluetooth PAN)
 * Celular com o "Tethering Bluetooth" ligado oferece o NAP: Network1.Connect("nap") cria a
 * bnep0 e o udhcpc pega endereço, rota (métrica 0: ganha da rota do PC, 200) e DNS (o
 * default.script marca as linhas "# bnep0" e as tira ao desconectar). Ligado por padrão para
 * todo aparelho pareado e conectado que anuncia NAP; desligado à mão fica em INET_OFF.
 * Tenta de novo a cada 30 s enquanto o aparelho está conectado (o tethering pode ser ligado
 * depois). Antes ninguém chamava o Network1: o celular conectava e a internet não vinha. */

static int inet_off(const char *addr)
{
    FILE *f = fopen(INET_OFF, "r");
    if(!f) return 0;
    char l[64];
    int off = 0;
    while(!off && fgets(l, sizeof l, f)) if(!strncmp(l, addr, 17)) off = 1;
    fclose(f);
    return off;
}

static void inet_set(const char *addr, int on)
{
    char tmp[300];
    snprintf(tmp, sizeof tmp, "%s.novo", INET_OFF);
    FILE *in = fopen(INET_OFF, "r"), *out = fopen(tmp, "w");
    if(!out) { if(in) fclose(in); return; }
    char l[64];
    while(in && fgets(l, sizeof l, in)) if(strncmp(l, addr, 17)) fputs(l, out);
    if(in) fclose(in);
    if(!on) fprintf(out, "%s\n", addr);
    if(fclose(out) == 0) rename(tmp, INET_OFF);
}

static int pan_slot(const char *addr, int make)
{
    int fr = -1;
    for(int k = 0; k < MAXPAN; k++) {
        if(!strcmp(pan[k].addr, addr)) return k;
        if(!pan[k].addr[0] && fr < 0) fr = k;
    }
    if(!make || fr < 0) return -1;
    memset(&pan[fr], 0, sizeof pan[fr]);
    snprintf(pan[fr].addr, sizeof pan[fr].addr, "%s", addr);
    return fr;
}

static void pan_done(DBusPendingCall *pc, void *ud)
{
    int k = (int)(intptr_t)ud;
    DBusMessage *r = dbus_pending_call_steal_reply(pc);
    pan[k].pending = 0;
    if(r && dbus_message_get_type(r) == DBUS_MESSAGE_TYPE_ERROR) {
        const char *msg = NULL;
        dbus_message_get_args(r, NULL, DBUS_TYPE_STRING, &msg, DBUS_TYPE_INVALID);
        syslog(LOG_INFO, "internet pelo celular: %s %s (o tethering Bluetooth está ligado no celular?)",
               dbus_message_get_error_name(r), msg ? msg : "");
    } else if(r) {
        const char *ifc = NULL;
        if(dbus_message_get_args(r, NULL, DBUS_TYPE_STRING, &ifc, DBUS_TYPE_INVALID) && ifc)
            snprintf(pan[k].iface, sizeof pan[k].iface, "%s", ifc);
        syslog(LOG_INFO, "internet pelo celular: %s conectada", pan[k].iface);
    }
    if(r) dbus_message_unref(r);
    dbus_pending_call_unref(pc);
    dirty = 1;
}

static void pan_dhcp_stop(int k)
{
    if(pan[k].dhcp > 0) {
        kill(pan[k].dhcp, SIGTERM);                       /* -R: devolve o endereço e o script limpa */
        pan[k].dhcp = 0;
    }
}

static void pan_tick(const state_t *st, long now)
{
    for(int i = 0; i < st->ndev; i++) {
        const dev_t_ *d = &st->dev[i];
        if(!d->addr[0] || !d->has_nap) continue;
        int want = d->paired && d->connected && !inet_off(d->addr);
        int k = pan_slot(d->addr, want || d->pan_on);
        if(k < 0) continue;
        if(d->pan_on && d->iface[0]) snprintf(pan[k].iface, sizeof pan[k].iface, "%s", d->iface);
        if(want && !d->pan_on && !pan[k].pending && now >= pan[k].next_try) {
            DBusMessage *m = dbus_message_new_method_call(BLUEZ, d->path, "org.bluez.Network1", "Connect");
            const char *nap = "nap";
            DBusPendingCall *pc = NULL;
            if(m) {
                dbus_message_append_args(m, DBUS_TYPE_STRING, &nap, DBUS_TYPE_INVALID);
                if(dbus_connection_send_with_reply(bus, m, &pc, 20000) && pc) {
                    dbus_pending_call_set_notify(pc, pan_done, (void *)(intptr_t)k, NULL);
                    pan[k].pending = 1;
                }
                dbus_message_unref(m);
            }
            pan[k].next_try = now + 30000;
        }
        if(d->pan_on && want && pan[k].iface[0] && pan[k].dhcp <= 0) {
            pid_t p = fork();
            if(p == 0) {
                setsid();
                int nul = open("/dev/null", O_RDWR);
                if(nul >= 0) { dup2(nul, 0); dup2(nul, 1); dup2(nul, 2); }
                execl("/sbin/udhcpc", "udhcpc", "-i", pan[k].iface, "-f", "-R", "-t", "8", "-T", "3", "-A", "10",
                      "-s", "/usr/sbin/bigfoston-pan-dhcp", (char *)NULL);   /* DNS do celular primeiro */
                _exit(127);
            }
            if(p > 0) {
                pan[k].dhcp = p;
                syslog(LOG_INFO, "internet pelo celular: pedindo endereço na %s", pan[k].iface);
            }
        }
        if((!d->pan_on || !want) && pan[k].dhcp > 0) pan_dhcp_stop(k);
        if(!want && d->pan_on)                          /* desligado à mão: solta a rede */
            call_async(d->path, "org.bluez.Network1", "Disconnect", "reconnect", 10000, DBUS_TYPE_INVALID);
        if(!want && !d->pan_on && !pan[k].pending) pan[k].addr[0] = 0;
    }
}

/* o endereço IPv4 da interface ("" se não há) */
static void iface_ip(const char *ifc, char *out, size_t n)
{
    out[0] = 0;
    int s = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if(s < 0) return;
    struct ifreq r;
    memset(&r, 0, sizeof r);
    snprintf(r.ifr_name, sizeof r.ifr_name, "%s", ifc);
    if(ioctl(s, SIOCGIFADDR, &r) == 0)
        inet_ntop(AF_INET, &((struct sockaddr_in *)&r.ifr_addr)->sin_addr, out, (socklen_t)n);
    close(s);
}

/* pareamentos novos ou trocados (inclusive os que o celular começa) vão para o cartão: a
 * chave nova só existia na RAM até o próximo "desligar o Bluetooth" e, se o aparelho fosse
 * desligado sem isso, voltava a antiga do cartão e o celular pedia o código de novo */
static long newest_info(void)
{
    long newest = 0;
    DIR *ad = opendir("/var/lib/bluetooth");
    if(!ad) return 0;
    struct dirent *a;
    while((a = readdir(ad))) {
        if(a->d_name[0] == '.') continue;
        char dp[300];
        snprintf(dp, sizeof dp, "/var/lib/bluetooth/%s", a->d_name);
        DIR *dd = opendir(dp);
        if(!dd) continue;
        struct dirent *e;
        while((e = readdir(dd))) {
            if(e->d_name[0] == '.' || strlen(e->d_name) != 17) continue;
            char ip[400];
            struct stat sb;
            snprintf(ip, sizeof ip, "%s/%s/info", dp, e->d_name);
            if(!stat(ip, &sb) && sb.st_mtime > newest) newest = sb.st_mtime;
        }
        closedir(dd);
    }
    closedir(ad);
    return newest;
}

static void on_sig(int s) { (void)s; quit = 1; }

int main(int argc, char **argv)
{
    int fg = argc > 1 && !strcmp(argv[1], "-f");
    openlog("bfbt-agent", LOG_PID, LOG_DAEMON);
    /* S35bluetooth, antes do bluetoothd: corrige os caches (e testes: outra pasta) */
    if(argc > 1 && !strcmp(argv[1], "--fix-hid")) {
        int n = ds4_fix_all(argc > 2 ? argv[2] : BT_LIB);
        printf("%d\n", n);
        return 0;
    }
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
            trust_inputs(&st);
            snprintf(player_path, sizeof player_path, "%s", st.pl_path);
            write_state(&st);
            role_switch_tick(&st, now);
            hid_link_tick(&st, now);
            pan_tick(&st, now);
            /* chave de pareamento nova ou trocada: para o cartão (a primeira leitura só marca) */
            static long info_seen = -1;
            long ni = newest_info();
            if(info_seen >= 0 && ni > info_seen && !save_soon) save_soon = now + 2000;
            if(ni > info_seen) info_seen = ni;
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
            /* com teclado ou controle conectado, não procura ninguém: enquanto o MT6611 chama
             * um aparelho ausente (Create Connection, até ~5 s de page) o enlace do controle
             * fica sem relatório nenhum (medido: 0/s por 5 s, o "controle travou e depois
             * mexeu sozinho"). O celular ainda pode conectar sozinho (quem chama é ele). */
            int hid_on = 0;
            for(int i = 0; i < st.ndev; i++)
                if(st.dev[i].connected && !strncmp(st.dev[i].icon, "input-", 6)) hid_on = 1;
            if(hid_on) syslog(LOG_INFO, "reconexão automática adiada: teclado/controle conectado");
            for(int i = 0; i < st.ndev && !hid_on; i++)
                /* teclados e mouses reconectam sozinhos (o BlueZ cuida dos que pedem ao
                 * computador): aqui só atrasariam o celular com uma espera de ~5 s */
                if(st.dev[i].paired && st.dev[i].trusted && !st.dev[i].connected && strncmp(st.dev[i].icon, "input-", 6))
                    call_async(st.dev[i].path, "org.bluez.Device1", "Connect", "reconnect", 30000, DBUS_TYPE_INVALID);
            dirty = 1;
        }
        if(hid_fix_until && now >= hid_fix_next) {
            if(ds4_fix_all(BT_LIB) > 0) {
                /* o bluetoothd só lê o cache ao iniciar: reinicia e conecta de novo */
                hid_fix_until = 0;
                syslog(LOG_INFO, "DualShock 4 pareado: bluetoothd reiniciado com o descritor corrigido");
                pid_t p = fork();
                if(p == 0) {
                    setsid();
                    execl("/etc/init.d/S40bluetoothd", "S40bluetoothd", "restart", (char *)NULL);
                    _exit(127);
                }
                save_soon = now + 8000;
                hid_conn_at = now + 6000;
            } else if(now >= hid_fix_until) hid_fix_until = 0;
            else hid_fix_next = now + 1500;
        }
        if(hid_conn_at && now >= hid_conn_at && adapter[0]) {
            hid_conn_at = 0;
            call_async(hid_conn_path, "org.bluez.Device1", "Connect", "reconnect", 30000, DBUS_TYPE_INVALID);
        }
        if(save_soon && now >= save_soon) { save_soon = 0; run_service("save", NULL); }
        if(scan_until && now >= scan_until) {
            scan_until = 0;
            if(adapter[0]) call_async(adapter, "org.bluez.Adapter1", "StopDiscovery", "scan", 5000, DBUS_TYPE_INVALID);
        }
        if(now >= next_full) { dirty = 1; next_full = now + 10000; }   /* RSSI, marca, segurança */
        struct pollfd p[2] = { { dfd, POLLIN, 0 }, { ff, POLLIN, 0 } };
        int timeout = dirty ? 250 : (int)(next_full - now);
        if(powered_at && reconnect_done < 3 && timeout > 1000) timeout = 1000;
        if(scan_until && timeout > 1000) timeout = 1000;
        if(save_soon && timeout > 500) timeout = 500;
        if((hid_fix_until || hid_conn_at) && timeout > 500) timeout = 500;
        for(int k = 0; k < MAXRS; k++)
            if(rs[k].addr[0] && !rs[k].done) { dirty = 1; if(timeout > 500) timeout = 500; }
        for(int k = 0; k < MAXHL; k++)
            if(hl[k].addr[0] && !hl[k].done) { dirty = 1; if(timeout > 500) timeout = 500; }
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
