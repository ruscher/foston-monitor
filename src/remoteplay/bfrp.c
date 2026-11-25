/* bfrp.c — motor do Big Remote Play no FOSTON: cliente GameStream (Sunshine) sobre a libgamestream
 * e a moonlight-common-c do Moonlight Embedded (GPLv3). O shell (src/ui/screens/scr_brp.c) fala com
 * ele por linhas em stdin/stdout, como com o bfweb:
 *
 *   shell -> bfrp                               bfrp -> shell
 *   HOST endereço                               HOST nome|pareado|ocupado|app_atual|versão  ou ERR ...
 *   PAIR                                        PIN nnnn (digite no Sunshine) ... PAIR ok | PAIR erro ...
 *   UNPAIR                                      UNPAIR ok
 *   APPS                                        APP id|nome ... APPS fim
 *   START app largura altura qps kbps video     STAGE nome / STARTED / TERMINATED código
 *   STOP                                        STOPPED
 *   QUITAPP                                     QUITAPP ok
 *   PAD botões lt rt lx ly rx ry                (estado inteiro do controle 1; botões = *_FLAG)
 *   KEY código_vk 1|0 modificadores             teclado (códigos virtuais do Windows, como o Moonlight)
 *   MREL dx dy / MBTN botão 1|0 / SCROLL n      mouse
 *   MABS x y largura altura                     ponteiro do mouse na posição (toque na imagem)
 *   SHOW 1|0                                    desenha ou não o vídeo (algo por cima dele)
 *   RECT x y l a / CLIP n                       nova área (tela cheia) / n linhas de baixo cobertas
 *   ZOOM pct / PAN dx dy l a                    zoom 100..400 % / arrastar a imagem com zoom
 *   (MABS usa o recorte mostrado: sem as faixas pretas do vídeo, coberto, com zoom)
 *   RELEASE                                     solta tudo (botões, eixos, teclas, mouse)
 *   START ... video x y l a                     com vídeo: o retângulo da tela -> VSTATS qps erros ms
 *
 * Modo Somente Controle: START com video=0. A sessão sobe no menor fluxo e os quadros são
 * descartados sem decodificar; a entrada vai pelo canal de controle cifrado do próprio Sunshine
 * (sem agente extra no PC).
 *
 * Segurança: o par de chaves do cliente fica em <dados>/remoteplay (o pareamento por PIN do
 * Sunshine); o motor nunca deixa um botão pressionado no PC: RELEASE ao parar, ao fechar a
 * entrada (o shell saiu ou caiu) e quando a conexão termina.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <Limelight.h>
#include "client.h"
#include "errors.h"
#include "video.h"
#include "audio.h"
#include "relay.h"
#include "proto.h"
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <poll.h>

static SERVER_DATA server;
static bool have_server;
static char key_dir[256] = "/data/bigfoston/remoteplay";
static pthread_mutex_t out_mx = PTHREAD_MUTEX_INITIALIZER;

static void out(const char *fmt, ...)
{
    va_list ap;
#ifdef BFRP_RELAY
    char b[600];
    va_start(ap, fmt);
    int n = vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    if(n < 0) return;
    if(n >= (int)sizeof b) n = (int)sizeof b - 1;
    relay_send('E', b, (uint32_t)n);
    fprintf(stderr, "> %s\n", b);
#else
    pthread_mutex_lock(&out_mx);
    va_start(ap, fmt);
    vfprintf(stdout, fmt, ap);
    va_end(ap);
    fputc('\n', stdout);
    fflush(stdout);
    pthread_mutex_unlock(&out_mx);
#endif
}

/* nomes vindos do host: sem quebras de linha nem o separador '|' (o protocolo é por linhas) */
static void clean(char *d, size_t n, const char *s)
{
    size_t j = 0;
    for(; s && *s && j + 1 < n; s++) d[j++] = (*s == '\n' || *s == '\r' || *s == '|') ? ' ' : *s;
    d[j] = 0;
}

/* ------------------------------------------------------------ estado da entrada */

static volatile bool streaming;
static int pad_buttons, pad_lt, pad_rt, pad_lx, pad_ly, pad_rx, pad_ry;
static unsigned char keys_down[256];
static unsigned char mouse_down[8];

static void send_pad(void)
{
    if(!streaming) return;
    LiSendMultiControllerEvent(0, 1, pad_buttons, (unsigned char)pad_lt, (unsigned char)pad_rt,
                               (short)pad_lx, (short)pad_ly, (short)pad_rx, (short)pad_ry);
}

static void release_all(void)
{
    pad_buttons = pad_lt = pad_rt = pad_lx = pad_ly = pad_rx = pad_ry = 0;
    if(!streaming) { memset(keys_down, 0, sizeof keys_down); memset(mouse_down, 0, sizeof mouse_down); return; }
    send_pad();
    for(int k = 0; k < 256; k++)
        if(keys_down[k]) { LiSendKeyboardEvent((short)(0x8000 | k), KEY_ACTION_UP, 0); keys_down[k] = 0; }
    for(int b = 1; b < 8; b++)
        if(mouse_down[b]) { LiSendMouseButtonEvent(BUTTON_ACTION_RELEASE, b); mouse_down[b] = 0; }
}

static int clamp(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

/* ------------------------------------------------------------ callbacks da sessão */

static bool want_video;
static unsigned long frames_in;

static int dr_setup(int fmt, int w, int h, int rate, void *ctx, int flags)
{
    (void)fmt; (void)ctx; (void)flags;
    out("VIDEO %dx%d %d", w, h, rate);
    if(want_video && video_setup(w, h)) { out("ERR decodificador"); return -1; }
    return 0;
}

static int dr_submit(PDECODE_UNIT du)
{
    frames_in++;
    if(!want_video) return DR_OK;                        /* Somente Controle: descarta */
    return video_submit(du);
}

static int ar_init(int cfg, const POPUS_MULTISTREAM_CONFIGURATION oc, void *ctx, int flags)
{
    (void)ctx; (void)flags;
    if(!want_video) return 0;                            /* Somente Controle: sem som */
    int r = audio_init(cfg, oc);
    out("AUDIO %s %d ch %d Hz", r == 0 ? "ok" : r > 0 ? "ocupado" : "erro", oc->channelCount, oc->sampleRate);
    return r < 0 ? -1 : 0;                               /* alto-falante ocupado: segue sem som */
}

static void ar_sample(char *d, int n) { if(want_video) audio_sample(d, n); }
static void ar_cleanup(void) { audio_close(); }

static void cl_stage_starting(int s) { out("STAGE %s", LiGetStageName(s)); }
static void cl_stage_failed(int s, int err) { out("FAILED %s %d", LiGetStageName(s), err); }
static void cl_started(void) { streaming = true; out("STARTED"); }
static void cl_terminated(int err)
{
    streaming = false;                                   /* o host já solta os controles ao cair */
    memset(keys_down, 0, sizeof keys_down);
    memset(mouse_down, 0, sizeof mouse_down);
    out("TERMINATED %d", err);
}
static void cl_log(const char *fmt, ...)
{
    if(!getenv("BFRP_DEBUG")) return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
}
static void cl_status(int st) { out("STATUS %s", st == CONN_STATUS_POOR ? "ruim" : "boa"); }

/* ------------------------------------------------------------ comandos */

static void cmd_host(const char *addr)
{
    if(have_server) { have_server = false; }
    /* a libgamestream guarda só o ponteiro (serverInfo.address) e o usa depois, inclusive na
     * thread do pareamento: o texto não pode ficar na pilha (24/11/2026: o endereço chegava
     * como "\x??\x??2.168.7.1" e o pareamento falhava com "Could not resolve hostname") */
    static char a[128];
    clean(a, sizeof a, addr);
    int r = gs_init(&server, a, 47989, key_dir, getenv("BFRP_DEBUG") ? 2 : 0, true);
    if(r != GS_OK) { out("ERR host %d %s", r, gs_error ? gs_error : ""); return; }
    have_server = true;
    char name[96];
    clean(name, sizeof name, server.serverInfo.address);
    out("HOST %s|%d|%d|%d|%s", name, server.paired ? 1 : 0, server.currentGame ? 1 : 0, server.currentGame,
        server.serverInfo.serverInfoAppVersion ? server.serverInfo.serverInfoAppVersion : "");
}

static char pair_pin[5];

static void *pair_thread(void *a)
{
    (void)a;
    fprintf(stderr, "bfrp: pareamento com PIN, esperando a aprovação no computador\n");
    int r = gs_pair(&server, pair_pin);
    fprintf(stderr, "bfrp: gs_pair = %d (%s)\n", r, gs_error ? gs_error : "");
    if(r == GS_OK) { server.paired = true; out("PAIR ok"); }
    else out("PAIR erro %d %s", r, gs_error ? gs_error : "");
    return NULL;
}

static void cmd_pair(void)
{
    if(!have_server) { out("ERR sem host"); return; }
    if(server.paired) { out("PAIR ok"); return; }
    unsigned char b[2];
    FILE *f = fopen("/dev/urandom", "rb");
    if(!f || fread(b, 1, 2, f) != 2) { if(f) fclose(f); out("ERR aleatório"); return; }
    fclose(f);
    snprintf(pair_pin, sizeof pair_pin, "%04u", (unsigned)((b[0] << 8 | b[1]) % 10000));
    out("PIN %s", pair_pin);
    pthread_t t;
    pthread_create(&t, NULL, pair_thread, NULL);          /* espera o PIN ser digitado no host */
    pthread_detach(t);
}

static void cmd_apps(void)
{
    if(!have_server || !server.paired) { out("ERR não pareado"); return; }
    PAPP_LIST list = NULL;
    if(gs_applist(&server, &list) != GS_OK) { out("ERR apps %s", gs_error ? gs_error : ""); return; }
    int n = 0;
    for(PAPP_LIST l = list; l && n < 200; l = l->next, n++) {
        char name[128];
        clean(name, sizeof name, l->name);
        out("APP %d|%s", l->id, name);
    }
    while(list) { PAPP_LIST nx = list->next; free(list->name); free(list); list = nx; }
    out("APPS fim");
}

static STREAM_CONFIGURATION cfg;

static void cmd_start(int app, int w, int h, int fps, int kbps, int video, int vx, int vy, int vw, int vh)
{
    if(video && video_open(vx, vy, vw, vh)) { out("ERR framebuffer"); return; }
    if(!have_server || !server.paired) { out("ERR não pareado"); return; }
    if(streaming) { out("ERR já em sessão"); return; }
    LiInitializeStreamConfiguration(&cfg);
    cfg.width = clamp(w, 320, 1920);
    cfg.height = clamp(h, 180, 1080);
    cfg.fps = clamp(fps, 10, 60);
    cfg.bitrate = clamp(kbps, 300, 20000);
    cfg.packetSize = 1024;
    cfg.streamingRemotely = STREAM_CFG_AUTO;
    cfg.audioConfiguration = AUDIO_CONFIGURATION_STEREO;
    cfg.supportedVideoFormats = VIDEO_FORMAT_H264;
    cfg.encryptionFlags = ENCFLG_NONE;
    want_video = video != 0;
#ifdef BFRP_RELAY
    if(want_video) {
        /* o relay decodifica no PC: pede o vídeo bom e rápido do Sunshine (no próprio PC) e reduz
         * ao tamanho do FOSTON; a largura pedida pela tela vira a do quadro enviado */
        relay_set_target(w);
        cfg.width = 1280;
        cfg.height = 720;
        cfg.fps = 60;
        cfg.bitrate = 12000;
        cfg.packetSize = 1392;
    }
#endif
    int r = gs_start_app(&server, &cfg, app, false, false, 1);
    if(r != GS_OK) { out("ERR start %d %s", r, gs_error ? gs_error : ""); return; }

    static DECODER_RENDERER_CALLBACKS dr;
    static AUDIO_RENDERER_CALLBACKS ar;
    static CONNECTION_LISTENER_CALLBACKS cl;
    LiInitializeVideoCallbacks(&dr);
    dr.setup = dr_setup;
    dr.submitDecodeUnit = dr_submit;
    /* com vídeo: a thread de decodificação da moonlight-common-c (fila e descarte dela); sem
     * vídeo: descarta direto na thread de recepção */
    dr.capabilities = want_video ? 0 : CAPABILITY_DIRECT_SUBMIT;
    LiInitializeAudioCallbacks(&ar);
    ar.init = ar_init;
    ar.decodeAndPlaySample = ar_sample;
    ar.cleanup = ar_cleanup;
    ar.capabilities = CAPABILITY_SLOW_OPUS_DECODER;      /* pacotes maiores: menos trabalho no ARM11 */
    LiInitializeConnectionCallbacks(&cl);
    cl.stageStarting = cl_stage_starting;
    cl.stageFailed = cl_stage_failed;
    cl.connectionStarted = cl_started;
    cl.connectionTerminated = cl_terminated;
    cl.logMessage = cl_log;
    cl.connectionStatusUpdate = cl_status;
    r = LiStartConnection(&server.serverInfo, &cfg, &cl, &dr, &ar, NULL, 0, NULL, 0);
    if(r != 0) { streaming = false; out("ERR conexão %d", r); }
}

static void cmd_stop(void)
{
    release_all();
    LiStopConnection();
    streaming = false;
    video_close();
    out("STOPPED");
}

static void command(char *l)
{
    char *a = strchr(l, ' ');
    if(a) *a++ = 0;
    else a = l + strlen(l);
    if(!strcmp(l, "HOST")) cmd_host(a);
    else if(!strcmp(l, "PAIR")) cmd_pair();
    else if(!strcmp(l, "UNPAIR")) { if(have_server) gs_unpair(&server); server.paired = false; out("UNPAIR ok"); }
    else if(!strcmp(l, "APPS")) cmd_apps();
    else if(!strcmp(l, "START")) {
        int app = 0, w = 640, h = 360, fps = 30, kbps = 1000, video = 0, vx = 80, vy = 48, vw = 640, vh = 360, lf = 0;
        sscanf(a, "%d %d %d %d %d %d %d %d %d %d %d", &app, &w, &h, &fps, &kbps, &video, &vx, &vy, &vw, &vh, &lf);
        video_loop_filter(lf);
        cmd_start(app, w, h, fps, kbps, video, vx, vy, vw, vh);
    }
    else if(!strcmp(l, "STOP")) cmd_stop();
    else if(!strcmp(l, "QUITAPP")) { release_all(); if(have_server) gs_quit_app(&server); out("QUITAPP ok"); }
    else if(!strcmp(l, "PAD")) {
        int b, lt, rt, lx, ly, rx, ry;
        if(sscanf(a, "%d %d %d %d %d %d %d", &b, &lt, &rt, &lx, &ly, &rx, &ry) == 7) {
            pad_buttons = b & 0xffff; pad_lt = clamp(lt, 0, 255); pad_rt = clamp(rt, 0, 255);
            pad_lx = clamp(lx, -32768, 32767); pad_ly = clamp(ly, -32768, 32767);
            pad_rx = clamp(rx, -32768, 32767); pad_ry = clamp(ry, -32768, 32767);
            send_pad();
        }
    }
    else if(!strcmp(l, "KEY")) {
        int k, d, m = 0;
        if(sscanf(a, "%d %d %d", &k, &d, &m) >= 2 && k > 0 && k < 256 && streaming) {
            keys_down[k] = d ? 1 : 0;
            LiSendKeyboardEvent((short)(0x8000 | k), d ? KEY_ACTION_DOWN : KEY_ACTION_UP, (char)(m & 0x1f));
        }
    }
    else if(!strcmp(l, "MREL")) {
        int dx, dy;
        if(sscanf(a, "%d %d", &dx, &dy) == 2 && streaming) LiSendMouseMoveEvent((short)clamp(dx, -500, 500), (short)clamp(dy, -500, 500));
    }
    else if(!strcmp(l, "MBTN")) {
        int b, d;
        if(sscanf(a, "%d %d", &b, &d) == 2 && b >= 1 && b <= 5 && streaming) {
            mouse_down[b] = d ? 1 : 0;
            LiSendMouseButtonEvent(d ? BUTTON_ACTION_PRESS : BUTTON_ACTION_RELEASE, b);
        }
    }
    else if(!strcmp(l, "SCROLL")) {
        int n;
        if(sscanf(a, "%d", &n) == 1 && streaming) LiSendScrollEvent((signed char)clamp(n, -10, 10));
    }
    else if(!strcmp(l, "MABS")) {                         /* ponteiro na posição (x, y) de uma área l x a */
        int x, y, w, h, fx, fy, fw, fh;
        if(sscanf(a, "%d %d %d %d", &x, &y, &w, &h) == 4 && w > 0 && h > 0 && streaming) {
            if(want_video && video_map(clamp(x, 0, w - 1), clamp(y, 0, h - 1), w, h, &fx, &fy, &fw, &fh))
                LiSendMousePositionEvent((short)clamp(fx, 0, fw - 1), (short)clamp(fy, 0, fh - 1), (short)fw, (short)fh);
            else
                LiSendMousePositionEvent((short)clamp(x, 0, w - 1), (short)clamp(y, 0, h - 1), (short)w, (short)h);
        }
    }
    else if(!strcmp(l, "ZOOM")) video_zoom(atoi(a));    /* 100..400 % */
    else if(!strcmp(l, "MUTE")) audio_mute(atoi(a));
    else if(!strcmp(l, "PAN")) {                          /* "PAN dx dy l a": arrastar a imagem com zoom */
        int dx, dy, w, h;
        if(sscanf(a, "%d %d %d %d", &dx, &dy, &w, &h) == 4) video_pan(dx, dy, w, h);
    }
    else if(!strcmp(l, "SHOW")) video_show(atoi(a));
    else if(!strcmp(l, "RECT")) {                         /* "RECT x y l a": a área do vídeo mudou (tela cheia) */
        int x, y, w, h;
        if(sscanf(a, "%d %d %d %d", &x, &y, &w, &h) == 4) video_open(x, y, w, h);
    }
    else if(!strcmp(l, "CLIP")) video_clip(atoi(a));     /* linhas de baixo cobertas pela barra */
    else if(!strcmp(l, "RELEASE")) release_all();
    else out("ERR comando");
}

#ifdef BFRP_RELAY
static void serve(int c)
{
    int one = 1, snd = 1 << 20;
    setsockopt(c, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    setsockopt(c, SOL_SOCKET, SO_SNDBUF, &snd, sizeof snd);
    relay_attach(c);
    out("READY");
    proto_rx_t rx = { 0 };
    rx.cap = 64 * 1024;
    rx.buf = malloc(rx.cap);
    time_t last = 0;
    while(rx.buf) {
        struct pollfd p = { c, POLLIN, 0 };
        int pr = poll(&p, 1, 1000);
        if(pr < 0 && errno == EINTR) continue;
        if(pr > 0) {
            if(rx.n == rx.cap) break;
            ssize_t n = read(c, rx.buf + rx.n, rx.cap - rx.n);
            if(n <= 0) break;
            rx.n += (size_t)n;
            char type;
            uint8_t *d;
            uint32_t l;
            size_t used;
            int r;
            while((r = proto_take(&rx, &type, &d, &l, &used)) == 1) {
                if(type == 'Q') relay_ack();
                else if(type == 'R') relay_key();                 /* o FOSTON perdeu a base */
                else if(type == 'K' && l < 500) {
                    char cmd[512];
                    memcpy(cmd, d, l);
                    cmd[l] = 0;
                    command(cmd);
                }
                memmove(rx.buf, rx.buf + used, rx.n - used);
                rx.n -= used;
            }
            if(r < 0) break;
        }
        time_t t = time(NULL);
        if(streaming && want_video && t - last >= 2) {
            unsigned long f, e, dec, sent, ap, al, au;
            unsigned long long us, bytes;
            video_stats(&f, &e, &us);
            relay_stats(&dec, &sent, &bytes);
            audio_stats(&ap, &al, &au);
            double dt = last ? (double)(t - last) : 2.0;
            out("VSTATS %.1f %lu 0", f / dt, e);
            fprintf(stderr, "relay: decodificados %.1f/s, enviados %.1f/s, desenhados %.1f/s, %.0f KB/s, som %lu\n",
                    dec / dt, sent / dt, f / dt, bytes / dt / 1024, ap);
            last = t;
        }
    }
    /* o FOSTON saiu ou a rede USB caiu: nada preso no PC */
    if(streaming) cmd_stop();
    relay_attach(-1);
    free(rx.buf);
}

/* conecta no FOSTON (que escuta enquanto o app Big Remote Play está aberto): o firewall do PC
 * só libera as portas do Sunshine, e uma conexão que sai do PC passa. Tenta a cada 0,5 s. */
static int connect_foston(const char *addr, int port)
{
    int s = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if(s < 0) return -1;
    struct sockaddr_in a = { 0 };
    a.sin_family = AF_INET;
    a.sin_port = htons((uint16_t)port);
    if(inet_pton(AF_INET, addr, &a.sin_addr) != 1) { close(s); return -1; }
    int r = connect(s, (struct sockaddr *)&a, sizeof a);
    if(r < 0 && errno != EINPROGRESS) { close(s); return -1; }
    struct pollfd p = { s, POLLOUT, 0 };
    if(r < 0 && (poll(&p, 1, 400) != 1 || (p.revents & (POLLERR | POLLHUP)))) { close(s); return -1; }
    int err = 0;
    socklen_t el = sizeof err;
    getsockopt(s, SOL_SOCKET, SO_ERROR, &err, &el);
    if(err) { close(s); return -1; }
    fcntl(s, F_SETFL, 0);
    return s;
}

int main(int argc, char **argv)
{
    signal(SIGPIPE, SIG_IGN);
    const char *addr = "192.168.7.2";                    /* o FOSTON pela rede USB */
    for(int i = 1; i + 1 < argc; i++) {
        if(!strcmp(argv[i], "--keys")) snprintf(key_dir, sizeof key_dir, "%s", argv[++i]);
        else if(!strcmp(argv[i], "--foston")) addr = argv[++i];
    }
    mkdir(key_dir, 0700);
    fprintf(stderr, "relay: procurando o FOSTON em %s:%d\n", addr, RELAY_PORT);
    for(;;) {
        int c = connect_foston(addr, RELAY_PORT);
        if(c < 0) { usleep(500000); continue; }
        fprintf(stderr, "relay: FOSTON conectado\n");
        serve(c);
        close(c);
        fprintf(stderr, "relay: FOSTON saiu\n");
    }
}
#else
int main(int argc, char **argv)
{
    signal(SIGPIPE, SIG_IGN);
    const char *relay = NULL;
    for(int i = 1; i + 1 < argc; i++) {
        if(!strcmp(argv[i], "--keys")) snprintf(key_dir, sizeof key_dir, "%s", argv[++i]);
        else if(!strcmp(argv[i], "--relay")) relay = argv[++i];
    }
    /* o relay do PC primeiro (vídeo e som decodificados lá); sem ele, o modo direto */
    if(relay && relay_client_main(relay, RELAY_PORT) == 0) return 0;
    mkdir(key_dir, 0700);
    out("READY");
    char line[512];
    int len = 0;
    for(;;) {
        struct pollfd pf = { 0, POLLIN, 0 };
        int pr = poll(&pf, 1, 2000);
        if(pr < 0 && errno == EINTR) continue;
        if(pr > 0) {
            ssize_t n = read(0, line + len, sizeof line - 1 - (size_t)len);
            if(n <= 0) break;                             /* o shell fechou a entrada */
            len += (int)n;
            line[len] = 0;
            char *s = line, *nl;
            while((nl = strchr(s, '\n'))) {
                *nl = 0;
                if(nl > s && nl[-1] == '\r') nl[-1] = 0;
                if(*s) command(s);
                s = nl + 1;
            }
            len = (int)strlen(s);
            memmove(line, s, (size_t)len + 1);
            if(len >= (int)sizeof line - 1) len = 0;      /* linha grande demais: descarta */
        }
        if(streaming && want_video) {                     /* a cada ~2 s: quadros, erros e tempo */
            static time_t last;
            time_t t = time(NULL);
            if(t - last >= 2) {
                unsigned long f, e;
                unsigned long long us;
                video_stats(&f, &e, &us);
                out("VSTATS %.1f %lu %llu", last ? f / (double)(t - last) : 0.0, e, f ? us / f / 1000 : 0);
                unsigned long ap, al, au;
                audio_stats(&ap, &al, &au);
                out("ASTATS %lu %lu %lu %d", ap, al, au, audio_ok());   /* pacotes, perdidos, som secou, alto-falante */
                last = t;
            }
        }
    }
    /* o shell fechou a entrada (saiu ou caiu): nada fica pressionado no PC */
    if(streaming) cmd_stop();
    return 0;
}
#endif
