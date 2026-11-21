/* bigscreen.c — ver bigscreen.h. Mesma lógica do cliente bsmd-ce do Windows
 * CE (device-clients/wince/src/main.c): HELLO_ACK com as capacidades,
 * créditos a cada 8 KB consumidos e em cada PRESENT, NAK em CRC ruim,
 * PRESENTED quando o quadro já está na tela. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE        /* accept4 */
#endif
#include "bigscreen.h"
#include "status.h"
#include "../third_party/fsp/fsp.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/sysinfo.h>
#include <sys/utsname.h>
#include <time.h>
#include <unistd.h>

#define W 800                    /* o painel do FS-707DC */
#define H 480
#define BYE_QUIT 1               /* FSP_BYE: 0 = aguardar o PC, 1 = fechar o cliente */
#define CREDIT_STEP (8u * 1024u)
#define CHUNK (16 * 1024)

static bs_status_t st;
static int lfd = -1, cfd = -1;
static uint16_t *shadow;
static uint8_t *rx, *scratch, *chunk;
static size_t chunk_len, chunk_off;
static fsp_parser parser;
static fsp_box dirty, shown;
static uint32_t consumed, credited, present_id, present_t0;
static bool present_pending;
static uint16_t seq;
static lv_image_dsc_t img;
static bool touch_on = true;
static uint32_t move_ms = 20, last_move;

static uint32_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000u + ts.tv_nsec / 1000000u);
}

const bs_status_t *bs_status(void) { return &st; }
const lv_image_dsc_t *bs_image(void) { return &img; }

lv_area_t bs_last_dirty(void)
{
    lv_area_t a = { shown.x0, shown.y0, shown.x1 - 1, shown.y1 - 1 };
    return a;
}

/* ------------------------------------------------------------- envio */

static void drop_client(void)
{
    if(cfd >= 0) close(cfd);
    cfd = -1;
    st.state = lfd >= 0 ? BS_WAITING : BS_OFF;
    st.host[0] = st.peer[0] = 0;
    present_pending = false;
}

static int write_all(const uint8_t *p, size_t n)
{
    uint32_t t0 = now_ms();
    while(n) {
        ssize_t w = send(cfd, p, n, MSG_NOSIGNAL);
        if(w < 0) {
            if((errno == EINTR || errno == EAGAIN) && now_ms() - t0 < 3000) {
                struct pollfd pfd = { cfd, POLLOUT, 0 };
                poll(&pfd, 1, 100);
                continue;
            }
            return -1;
        }
        p += w;
        n -= (size_t)w;
    }
    return 0;
}

static void send_msg(uint8_t type, const uint8_t *payload, uint32_t len)
{
    uint8_t header[FSP_HEADER];
    if(cfd < 0) return;
    fsp_header(header, type, seq++, payload, len);
    if(write_all(header, FSP_HEADER) || (len && write_all(payload, len))) {
        fprintf(stderr, "bigscreen: falha ao enviar (%s); PC desconectado\n", strerror(errno));
        drop_client();
    }
}

static void send_hello_ack(void)
{
    uint8_t p[512];
    size_t n = 32;
    struct utsname u;
    struct sysinfo si;
    unsigned major = 0, minor = 0, patch = 0;
    uname(&u);
    sscanf(u.release, "%u.%u.%u", &major, &minor, &patch);
    sysinfo(&si);
    memset(p, 0, sizeof p);
    fsp_put16(p, FSP_VERSION);
    fsp_put16(p + 2, W);
    fsp_put16(p + 4, H);
    fsp_put16(p + 6, 16);
    fsp_put32(p + 8, major);               /* o cliente do CE põe aqui a versão do sistema */
    fsp_put32(p + 12, minor);
    fsp_put32(p + 16, patch);
    fsp_put32(p + 20, (uint32_t)((uint64_t)si.totalram * si.mem_unit / 1024));
    fsp_put32(p + 24, (uint32_t)((uint64_t)si.freeram * si.mem_unit / 1024));
    p[28] = status_provider()->backlight_get() >= 0;   /* brilho controlável */
    p[29] = 100;
    p[30] = 1;                             /* toque */
    p[31] = 2;                             /* envia FSP_CREDIT */
#define ADD_TEXT(s) do { size_t l_ = strlen(s); if(n + l_ + 1 < sizeof p) { memcpy(p + n, s, l_ + 1); n += l_ + 1; } } while(0)
    ADD_TEXT("bigfoston-shell " BIGFOSTON_VERSION);
    ADD_TEXT("BigFoston OS");
    ADD_TEXT(u.release);
    ADD_TEXT("tcp:7707");
    ADD_TEXT("");
#undef ADD_TEXT
    send_msg(FSP_HELLO_ACK, p, (uint32_t)n);
}

void bs_touch(int kind, int x, int y)
{
    uint8_t p[12];
    uint32_t t = now_ms();
    if(st.state != BS_CONNECTED || !touch_on) return;
    if(kind == FSP_TOUCH_MOVE) {
        if(t - last_move < move_ms) return;
        last_move = t;
    }
    p[0] = (uint8_t)kind;
    p[1] = 0;
    fsp_put16(p + 2, (uint16_t)LV_CLAMP(0, x, W - 1));
    fsp_put16(p + 4, (uint16_t)LV_CLAMP(0, y, H - 1));
    fsp_put16(p + 6, 0);
    fsp_put32(p + 8, t);
    send_msg(FSP_TOUCH, p, 12);
}

void bs_presented(uint32_t blit_ms)
{
    uint8_t r[12];
    if(!present_pending) return;
    present_pending = false;
    fsp_put32(r, present_id);
    fsp_put32(r + 4, now_ms() - present_t0);    /* decodificação + espera */
    fsp_put32(r + 8, blit_ms);
    send_msg(FSP_PRESENTED, r, 12);
}

/* ------------------------------------------------------------ início/fim */

bool bs_start(char *err, size_t n)
{
    struct sockaddr_in a;
    int one = 1;
    if(lfd >= 0) return true;
    memset(&st, 0, sizeof st);
    shadow = calloc((size_t)W * H, 2);
    scratch = malloc((size_t)W * H * 2);
    rx = malloc(FSP_HEADER + FSP_MAX_PAYLOAD);
    chunk = malloc(CHUNK);
    if(!shadow || !scratch || !rx || !chunk) {
        snprintf(err, n, "%s", strerror(ENOMEM));
        bs_stop();
        return false;
    }
    lfd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if(lfd < 0) {
        snprintf(err, n, "socket: %s", strerror(errno));
        bs_stop();
        return false;
    }
    setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons(BS_PORT);
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    if(bind(lfd, (struct sockaddr *)&a, sizeof a) || listen(lfd, 1)) {
        snprintf(err, n, "porta %d: %s", BS_PORT, strerror(errno));
        close(lfd);
        lfd = -1;
        bs_stop();
        return false;
    }
    memset(&img, 0, sizeof img);
    img.header.magic = LV_IMAGE_HEADER_MAGIC;
    img.header.cf = LV_COLOR_FORMAT_RGB565;
    img.header.w = W;
    img.header.h = H;
    img.header.stride = W * 2;
    img.data = (const uint8_t *)shadow;
    img.data_size = (uint32_t)W * H * 2;
    st.state = BS_WAITING;
    fprintf(stderr, "bigscreen: aguardando o PC na porta TCP %d\n", BS_PORT);
    return true;
}

void bs_stop(void)
{
    drop_client();
    if(lfd >= 0) close(lfd);
    lfd = -1;
    st.state = BS_OFF;
    free(shadow); free(scratch); free(rx); free(chunk);
    shadow = NULL; scratch = rx = chunk = NULL;
    chunk_len = chunk_off = 0;
    memset(&img, 0, sizeof img);
}

/* ------------------------------------------------------------- recepção */

static void accept_client(void)
{
    struct sockaddr_in a;
    socklen_t al = sizeof a;
    int one = 1;
    int fd = accept4(lfd, (struct sockaddr *)&a, &al, SOCK_NONBLOCK | SOCK_CLOEXEC);
    if(fd < 0) return;
    if(cfd >= 0) {                     /* um PC por vez: a conexão nova assume */
        fprintf(stderr, "bigscreen: nova conexão substitui a anterior\n");
        drop_client();
    }
    cfd = fd;
    setsockopt(cfd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    inet_ntop(AF_INET, &a.sin_addr, st.peer, sizeof st.peer);
    fsp_parser_init(&parser, rx, FSP_HEADER + FSP_MAX_PAYLOAD);
    chunk_len = chunk_off = 0;
    consumed = credited = 0;
    seq = 0;
    fprintf(stderr, "bigscreen: conexão de %s\n", st.peer);
}

/* trata uma mensagem; devolve true num PRESENT */
static bool handle(const fsp_msg *m)
{
    uint8_t r[12];
    switch(m->type) {
        case FSP_HELLO: {
            size_t hl = m->len > 4 ? m->len - 4 : 0;
            if(hl >= sizeof st.host) hl = sizeof st.host - 1;
            memcpy(st.host, m->payload + 4, hl);
            st.host[hl] = 0;
            st.state = BS_CONNECTED;
            st.frames = 0;
            st.quit_requested = false;
            dirty.x0 = dirty.x1 = dirty.y0 = dirty.y1 = 0;
            touch_on = true;
            move_ms = 20;
            send_hello_ack();
            fprintf(stderr, "bigscreen: HELLO de \"%s\"\n", st.host);
            break;
        }
        case FSP_PING:
            if(m->len >= 4) {
                memcpy(r, m->payload, 4);
                fsp_put32(r + 4, now_ms());
                send_msg(FSP_PONG, r, 8);
            }
            break;
        case FSP_RECT:
        case FSP_FILL: {
            fsp_surface s = { shadow, W, H, W };
            int e = fsp_apply(&s, m->type, m->payload, m->len, scratch, (size_t)W * H * 2, &dirty);
            if(e) {
                fsp_put16(r, m->seq);
                r[2] = (uint8_t)e;
                send_msg(FSP_NAK, r, 3);
            }
            break;
        }
        case FSP_PRESENT:
            if(m->len < 4) break;
            present_id = fsp_get32(m->payload);
            present_t0 = now_ms();
            present_pending = true;
            shown = dirty;
            dirty.x0 = dirty.x1 = dirty.y0 = dirty.y1 = 0;
            st.frames++;
            return true;
        case FSP_BRIGHTNESS:
            if(m->len >= 1 && status_provider()->backlight_get() >= 0)
                status_provider()->backlight_set(LV_CLAMP(5, m->payload[0], 100));
            break;
        case FSP_TOUCHCFG:
            if(m->len >= 1) touch_on = m->payload[0] != 0;
            if(m->len >= 2 && m->payload[1]) move_ms = m->payload[1];
            break;
        case FSP_CALIBRATE:
            break;                      /* a calibração do BigFoston fica em Ajustes */
        case FSP_BYE:
            if(m->len >= 1 && m->payload[0] == BYE_QUIT) st.quit_requested = true;
            fprintf(stderr, "bigscreen: BYE (%s)\n", st.quit_requested ? "fechar" : "aguardar");
            drop_client();
            return false;
        default:
            fsp_put16(r, m->seq);
            r[2] = FSP_NAK_UNKNOWN;
            send_msg(FSP_NAK, r, 3);
            break;
    }
    return false;
}

bool bs_poll(void)
{
    if(lfd < 0) return false;
    if(present_pending) return false;     /* espera a tela mostrar o quadro anterior */
    accept_client();
    if(cfd < 0) return false;
    uint32_t t0 = now_ms();
    for(;;) {
        fsp_msg m;
        int r = fsp_next(&parser, &m);
        if(r == FSP_NEED_MORE) {
            if(chunk_off < chunk_len) {
                chunk_off += fsp_feed(&parser, chunk + chunk_off, chunk_len - chunk_off);
                continue;
            }
            if(now_ms() - t0 > 40) return false;   /* devolve a vez ao LVGL (toque) */
            ssize_t got = recv(cfd, chunk, CHUNK, 0);
            if(got == 0 || (got < 0 && errno != EAGAIN && errno != EINTR)) {
                fprintf(stderr, "bigscreen: PC desconectado\n");
                drop_client();
                return false;
            }
            if(got < 0) return false;
            st.bytes += (uint32_t)got;
            chunk_len = (size_t)got;
            chunk_off = 0;
            continue;
        }
        uint8_t rr[4];
        if(r == FSP_BAD_CRC) {
            uint8_t n[3];
            fsp_put16(n, m.seq);
            n[2] = FSP_NAK_CRC;
            send_msg(FSP_NAK, n, 3);
            continue;
        }
        if(m.type == FSP_HELLO) consumed = credited = 0;
        else consumed += FSP_HEADER + m.len;
        bool presented = handle(&m);
        if(cfd < 0) return false;          /* BYE ou erro de envio */
        fsp_consume(&parser);
        if(consumed - credited >= CREDIT_STEP || (m.type == FSP_PRESENT && consumed != credited)) {
            credited = consumed;
            fsp_put32(rr, consumed);
            send_msg(FSP_CREDIT, rr, 4);
        }
        if(presented) {
            lv_image_cache_drop(&img);
            return true;
        }
    }
}
