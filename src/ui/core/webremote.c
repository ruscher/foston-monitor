/* webremote.c — ver webremote.h.
 *
 * Mensagens nos dois sentidos: tipo (1 byte) + tamanho (u32 LE) + conteúdo.
 *   PC -> FOSTON: H "chave\tnome" | E linha de evento | T x y l a (u16 LE) + zlib(RGB565) | P (fim do quadro)
 *   PC -> FOSTON (downloads): F "id\tnome\ttotal\turl" | G id (u32) + bloco | Z "id\ttamanho"
 *   FOSTON -> PC (som): U código de uso único; o PC abre a porta WEBAUDIO_PORT, manda
 *   "código\n" e depois PCM S16_LE estéreo 32 kHz (webaudio.c), só enquanto a página toca
 *   FOSTON -> PC: A "ok L A [url]" ou "no" | K linha de comando | Q quadro aplicado | q id (bloco gravado)
 * Controle de fluxo: o PC só manda um quadro novo depois do Q do anterior. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "webremote.h"
#include "settings.h"
#include "lodepng.h"
#include "webdl.h"
#include "webaudio.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#define MSG_MAX (1u << 20)

static int lfd = -1, cfd = -1;
static lv_timer_t *tick;
static webremote_cb cb;
static void *cb_ud;
static int fw, fh;
static uint16_t *fb;
static lv_image_dsc_t dsc;
static uint8_t *rbuf;
static size_t rlen, rcap;
static bool authed, pairing;
static char peer_key[80], peer_name[64], start_url[1024];
static int dx0, dy0, dx1, dy1;               /* região alterada desde o último P */

static void emit(const char *l) { if(cb) cb(l, cb_ud); }

static void keys_path(char *out, size_t n)
{
    char d[512];
    data_path(d, sizeof d, "web");
    mkdir(d, 0755);
    snprintf(out, n, "%s/pcs.tsv", d);
}

static bool key_known(const char *key)
{
    char p[600], line[256];
    keys_path(p, sizeof p);
    FILE *f = fopen(p, "r");
    bool ok = false;
    while(f && !ok && fgets(line, sizeof line, f)) {
        line[strcspn(line, "\t\r\n")] = 0;
        ok = !strcmp(line, key);
    }
    if(f) fclose(f);
    return ok;
}

static void key_save(const char *key, const char *name)
{
    char p[600];
    keys_path(p, sizeof p);
    FILE *f = fopen(p, "a");
    if(!f) return;
    fprintf(f, "%s\t%s\n", key, name);
    fflush(f);
    fsync(fileno(f));
    fclose(f);
}

static void raw_send(char t, const void *data, uint32_t n)
{
    if(cfd < 0) return;
    uint8_t h[5] = { (uint8_t)t, (uint8_t)n, (uint8_t)(n >> 8), (uint8_t)(n >> 16), (uint8_t)(n >> 24) };
    const uint8_t *parts[2] = { h, data };
    size_t lens[2] = { 5, n };
    for(int k = 0; k < 2; k++) {
        size_t off = 0;
        while(off < lens[k]) {
            ssize_t w = send(cfd, parts[k] + off, lens[k] - off, MSG_NOSIGNAL);
            if(w < 0 && (errno == EINTR || errno == EAGAIN)) continue;   /* mensagens pequenas */
            if(w <= 0) return;
            off += (size_t)w;
        }
    }
}

static void drop_client(void)
{
    bool was = authed;
    webdl_pc_lost();
    webaudio_set_token(NULL);                   /* derruba a conexão de som junto */
    if(cfd >= 0) close(cfd);
    cfd = -1;
    authed = pairing = false;
    rlen = 0;
    if(was) emit("REMOTE 0");
}

static void accept_ok(void)
{
    char a[1200];
    authed = true;
    pairing = false;
    snprintf(a, sizeof a, "ok %d %d %s", fw, fh, start_url);
    raw_send('A', a, (uint32_t)strlen(a));
    /* código para a conexão de som (só este PC, só esta ligação) */
    uint8_t r[16];
    char tok[40];
    int u = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if(u >= 0 && read(u, r, sizeof r) == (ssize_t)sizeof r) {
        for(int i = 0; i < 16; i++) snprintf(tok + 2 * i, 3, "%02x", r[i]);
        webaudio_set_token(tok);
        raw_send('U', tok, 32);
    }
    if(u >= 0) close(u);
    char m[100];
    snprintf(m, sizeof m, "REMOTE 1 %s", peer_name);
    emit(m);
}

void webremote_pair_answer(bool allow)
{
    if(!pairing || cfd < 0) return;
    if(allow) { key_save(peer_key, peer_name); accept_ok(); }
    else { raw_send('A', "no", 2); drop_client(); }
}

/* bloco de imagem: valida tudo (vem da rede) antes de tocar na memória */
static void tile(const uint8_t *p, uint32_t n)
{
    if(n < 8 || !fb) return;
    int x = p[0] | p[1] << 8, y = p[2] | p[3] << 8, w = p[4] | p[5] << 8, h = p[6] | p[7] << 8;
    if(w <= 0 || h <= 0 || x + w > fw || y + h > fh) return;
    unsigned char *out = NULL;
    size_t outn = 0;
    LodePNGDecompressSettings s;
    lodepng_decompress_settings_init(&s);
    s.max_output_size = (size_t)w * h * 2;
    if(lodepng_zlib_decompress(&out, &outn, p + 8, n - 8, &s) || outn != (size_t)w * h * 2) { free(out); return; }
    for(int r = 0; r < h; r++) memcpy(fb + (size_t)(y + r) * fw + x, out + (size_t)r * w * 2, (size_t)w * 2);
    free(out);
    if(dx1 <= dx0) { dx0 = x; dy0 = y; dx1 = x + w; dy1 = y + h; }
    else {
        if(x < dx0) dx0 = x;
        if(y < dy0) dy0 = y;
        if(x + w > dx1) dx1 = x + w;
        if(y + h > dy1) dy1 = y + h;
    }
}

static void message(char t, const uint8_t *p, uint32_t n)
{
    if(!authed) {
        if(t != 'H' || pairing) return;
        char s[160];
        snprintf(s, sizeof s, "%.*s", (int)(n < sizeof s - 1 ? n : sizeof s - 1), (const char *)p);
        char *tab = strchr(s, '\t');
        if(tab) *tab = 0;
        size_t kl = strlen(s);
        if(kl < 16 || kl >= sizeof peer_key) { drop_client(); return; }   /* chave de 32 hex */
        memcpy(peer_key, s, kl + 1);
        snprintf(peer_name, sizeof peer_name, "%.*s", (int)sizeof peer_name - 1, tab ? tab + 1 : "PC");
        for(char *c = peer_name; *c; c++) if((unsigned char)*c < 0x20) *c = ' ';
        if(key_known(peer_key)) { accept_ok(); return; }
        pairing = true;
        char m[100];
        snprintf(m, sizeof m, "PAIR %s", peer_name);
        emit(m);
        return;
    }
    if(t == 'E') {
        char line[8192];
        snprintf(line, sizeof line, "%.*s", (int)(n < sizeof line - 1 ? n : sizeof line - 1), (const char *)p);
        if(!strncmp(line, "R ", 2) || !strncmp(line, "REMOTE", 6) || !strncmp(line, "PAIR", 4)) return;  /* só daqui */
        emit(line);
    }
    else if(t == 'T') tile(p, n);
    else if(t == 'F') {                               /* download pelo PC: "id\tnome\ttotal\turl" */
        char s[1600];
        snprintf(s, sizeof s, "%.*s", (int)(n < sizeof s - 1 ? n : sizeof s - 1), (const char *)p);
        char *f[4] = { s, NULL, NULL, NULL };
        for(int k = 1; k < 4; k++) { f[k] = f[k - 1] ? strchr(f[k - 1], '\t') : NULL; if(f[k]) *f[k]++ = 0; }
        if(!f[1]) return;
        int id = atoi(f[0]);
        int local = webdl_pc_begin(id, f[1], f[2] ? strtoll(f[2], NULL, 10) : 0, f[3]);
        if(local < 0) { char c[32]; int l = snprintf(c, sizeof c, "DLCANCEL %d", id); raw_send('K', c, (uint32_t)l); }
    }
    else if(t == 'G' && n >= 4) {                     /* bloco: id (u32) + dados; confirma para vir o próximo */
        int id = (int)(p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24);
        if(webdl_pc_data(id, p + 4, n - 4)) raw_send('q', p, 4);
        else { char c[32]; int l = snprintf(c, sizeof c, "DLCANCEL %d", id); raw_send('K', c, (uint32_t)l); }
    }
    else if(t == 'Z') {                               /* fim: "id\ttamanho" */
        char s[64];
        snprintf(s, sizeof s, "%.*s", (int)(n < sizeof s - 1 ? n : sizeof s - 1), (const char *)p);
        char *tab = strchr(s, '\t');
        if(tab) webdl_pc_end(atoi(s), strtoll(tab + 1, NULL, 10));
    }
    else if(t == 'P') {
        raw_send('Q', NULL, 0);                     /* aplicado: o PC pode mandar o próximo */
        if(dx1 > dx0) {
            char r[64];
            snprintf(r, sizeof r, "R %d %d %d %d", dx0, dy0, dx1, dy1);
            dx0 = dy0 = dx1 = dy1 = 0;
            emit(r);
        }
    }
}

static void poll_cb(lv_timer_t *t)
{
    (void)t;
    if(cfd < 0 && lfd >= 0) {
        struct sockaddr_in a;
        socklen_t al = sizeof a;
        int c = accept4(lfd, (struct sockaddr *)&a, &al, SOCK_NONBLOCK | SOCK_CLOEXEC);
        if(c >= 0) {
            int one = 1;
            setsockopt(c, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
            cfd = c;
            rlen = 0;
            authed = pairing = false;
        }
    }
    if(cfd < 0) return;
    uint32_t t0 = lv_tick_get();
    while(cfd >= 0 && lv_tick_elaps(t0) < 40) {            /* não segura a interface */
        if(rcap - rlen < 65536) {
            size_t nc = rcap ? rcap * 2 : 262144;
            if(nc > MSG_MAX + 65536 + 5) nc = MSG_MAX + 65536 + 5;
            if(nc <= rcap) { drop_client(); return; }
            uint8_t *nb = realloc(rbuf, nc);
            if(!nb) { drop_client(); return; }
            rbuf = nb;
            rcap = nc;
        }
        ssize_t n = recv(cfd, rbuf + rlen, rcap - rlen, 0);
        if(n == 0) { drop_client(); return; }
        if(n < 0) { if(errno != EAGAIN && errno != EINTR) drop_client(); break; }
        rlen += (size_t)n;
        size_t off = 0;
        while(rlen - off >= 5) {
            uint32_t len = rbuf[off + 1] | rbuf[off + 2] << 8 | rbuf[off + 3] << 16 | (uint32_t)rbuf[off + 4] << 24;
            if(len > MSG_MAX) { drop_client(); return; }
            if(rlen - off < 5 + len) break;
            message((char)rbuf[off], rbuf + off + 5, len);
            if(cfd < 0) return;
            off += 5 + len;
        }
        memmove(rbuf, rbuf + off, rlen - off);
        rlen -= off;
    }
}

static void pc_cancel(int pc_id)
{
    char c[32];
    int l = snprintf(c, sizeof c, "DLCANCEL %d", pc_id);
    raw_send('K', c, (uint32_t)l);
}

bool webremote_listen(int width, int height, webremote_cb ecb, void *ud)
{
    webdl_set_pc_cancel(pc_cancel);
    webremote_stop();
    cb = ecb;
    cb_ud = ud;
    fw = width;
    fh = height;
    fb = malloc((size_t)fw * fh * 2);
    if(!fb) return false;
    memset(fb, 0xff, (size_t)fw * fh * 2);
    memset(&dsc, 0, sizeof dsc);
    dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    dsc.header.cf = LV_COLOR_FORMAT_RGB565;
    dsc.header.w = (uint32_t)fw;
    dsc.header.h = (uint32_t)fh;
    dsc.header.stride = (uint32_t)fw * 2;
    dsc.data = (const uint8_t *)fb;
    dsc.data_size = (uint32_t)((size_t)fw * fh * 2);

    lfd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if(lfd < 0) return false;
    int one = 1;
    setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(WEBREMOTE_PORT) };
    /* só a rede USB (o PC na outra ponta do cabo); no simulador, só o próprio PC */
    const char *addr = getenv("BIGFOSTON_WEB_REMOTE_ADDR");
    inet_pton(AF_INET, addr && *addr ? addr : "192.168.7.2", &a.sin_addr);
    if(bind(lfd, (struct sockaddr *)&a, sizeof a)) {
        inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
        if(bind(lfd, (struct sockaddr *)&a, sizeof a)) { close(lfd); lfd = -1; return false; }
    }
    if(listen(lfd, 1)) { close(lfd); lfd = -1; return false; }
    tick = lv_timer_create(poll_cb, 15, NULL);
    webaudio_listen();                  /* sem a porta de som: o modo completo segue mudo */
    return true;
}

void webremote_stop(void)
{
    if(cfd >= 0) raw_send('K', "QUIT", 4);
    cb = NULL;                          /* sem "REMOTE 0" depois de fechar o app */
    drop_client();
    webaudio_close();
    if(tick) { lv_timer_delete(tick); tick = NULL; }
    if(lfd >= 0) close(lfd);
    lfd = -1;
    if(fb) { lv_image_cache_drop(&dsc); free(fb); fb = NULL; }
    free(rbuf);
    rbuf = NULL;
    rlen = rcap = 0;
}

bool webremote_active(void) { return cfd >= 0 && authed; }
const lv_image_dsc_t *webremote_image(void) { return fb ? &dsc : NULL; }
void webremote_set_start_url(const char *url) { snprintf(start_url, sizeof start_url, "%s", url ? url : ""); }

void webremote_send(const char *fmt, ...)
{
    if(!webremote_active()) return;
    char line[8192];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    if(n < 0) return;
    if(n >= (int)sizeof line) n = sizeof line - 1;
    for(int i = 0; i < n; i++) if(line[i] == '\n' || line[i] == '\r') line[i] = ' ';
    raw_send('K', line, (uint32_t)n);
}
