/* bfbt-cover — capa do álbum da música do celular (AVRCP 1.6 Cover Art: BIP sobre OBEX/L2CAP),
 * sem o obexd (que no Buildroot exige C++ e não cabe na memória interna).
 *
 *   bfbt-cover AA:BB:CC:DD:EE:FF PSM SAIDA.jpg
 *
 * Abre o canal L2CAP (modo ERTM, como o GOEP 2.0 pede) no PSM do serviço de capas do celular
 * (Additional Protocol Descriptor do registro AVRCP Target, OBEX) e faz o OBEX CONNECT com o
 * alvo "BIP AVRCP". Pela especificação, o celular só manda o código da imagem (ImgHandle) nos
 * dados da faixa enquanto essa conexão existe. Depois lê códigos na entrada padrão (um por
 * linha) e, para cada um, faz o OBEX GET da miniatura (x-bt/img-thm, JPEG 200x200) e grava em
 * SAIDA.jpg (arquivo temporário + rename). Responde na saída padrão "ok CODIGO" ou "erro CODIGO".
 * Sai quando a entrada fecha ou o canal cai.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later */
#define _GNU_SOURCE
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define AF_BT 31
#define BTPROTO_L2CAP 0
#define SOL_L2CAP 6
#define L2CAP_OPTIONS 0x01
#define L2CAP_MODE_ERTM 0x03
#define SOL_BLUETOOTH 274
#define BT_SECURITY 4
#define BT_SECURITY_MEDIUM 2

struct sockaddr_l2 { unsigned short family, psm; uint8_t bdaddr[6]; unsigned short cid; uint8_t bdaddr_type; };
struct l2cap_options { uint16_t omtu, imtu, flush_to; uint8_t mode, fcs, max_tx; uint16_t txwin_size; };
struct bt_security { uint8_t level, key_size; };

#define MTU 4096
/* 7163DD54-4A7E-11E2-B47C-0050C2490048: alvo OBEX do serviço de capas do AVRCP */
static const uint8_t BIP_AVRCP[16] = { 0x71, 0x63, 0xdd, 0x54, 0x4a, 0x7e, 0x11, 0xe2,
                                       0xb4, 0x7c, 0x00, 0x50, 0xc2, 0x49, 0x00, 0x48 };
static int sk = -1;
static uint8_t conn_id[4];
static int have_cid;

static int recv_packet(uint8_t *buf, int max, int ms)
{
    struct pollfd p = { sk, POLLIN, 0 };
    if(poll(&p, 1, ms) <= 0) return -1;
    ssize_t r = recv(sk, buf, (size_t)max, 0);
    if(r < 3) return -1;
    int len = buf[1] << 8 | buf[2];
    return len <= r ? len : -1;
}

static int put_hdr_bytes(uint8_t *p, uint8_t id, const uint8_t *data, int n)
{
    p[0] = id;
    p[1] = (uint8_t)((n + 3) >> 8);
    p[2] = (uint8_t)(n + 3);
    memcpy(p + 3, data, (size_t)n);
    return n + 3;
}

static int obex_connect(void)
{
    uint8_t pkt[64], rsp[MTU];
    int n = 7;
    pkt[0] = 0x80;                                     /* CONNECT */
    pkt[3] = 0x10;                                     /* OBEX 1.0 */
    pkt[4] = 0x00;
    pkt[5] = MTU >> 8;
    pkt[6] = MTU & 0xff;
    n += put_hdr_bytes(pkt + n, 0x46, BIP_AVRCP, 16);  /* Target */
    pkt[1] = (uint8_t)(n >> 8);
    pkt[2] = (uint8_t)n;
    if(send(sk, pkt, (size_t)n, 0) != n) return 0;
    int len = recv_packet(rsp, sizeof rsp, 8000);
    if(len < 7 || rsp[0] != 0xa0) {
        fprintf(stderr, "bfbt-cover: CONNECT recusado (0x%02x)\n", len > 0 ? rsp[0] : 0);
        return 0;
    }
    for(int i = 7; i + 1 < len;) {                     /* procura o Connection ID (0xCB) */
        uint8_t id = rsp[i];
        if(id == 0xcb && i + 5 <= len) { memcpy(conn_id, rsp + i + 1, 4); have_cid = 1; i += 5; }
        else if((id & 0xc0) == 0xc0) i += 5;
        else if((id & 0xc0) == 0x80) i += 2;
        else { int hl = rsp[i + 1] << 8 | rsp[i + 2]; if(hl < 3) break; i += hl; }
    }
    return 1;
}

/* GET da miniatura; o corpo vai para f */
static int obex_get_thumbnail(const char *handle, FILE *f)
{
    uint8_t pkt[256], rsp[MTU];
    int n = 3;
    pkt[0] = 0x83;                                      /* GET, final */
    if(have_cid) { pkt[n++] = 0xcb; memcpy(pkt + n, conn_id, 4); n += 4; }
    static const char type[] = "x-bt/img-thm";
    n += put_hdr_bytes(pkt + n, 0x42, (const uint8_t *)type, (int)sizeof type);   /* Type com o \0 */
    uint8_t h16[64];
    int hl = 0;
    for(const char *c = handle; *c && hl < 60; c++) { h16[hl++] = 0; h16[hl++] = (uint8_t)*c; }
    h16[hl++] = 0;
    h16[hl++] = 0;
    n += put_hdr_bytes(pkt + n, 0x30, h16, hl);         /* Img-Handle (Unicode) */
    pkt[1] = (uint8_t)(n >> 8);
    pkt[2] = (uint8_t)n;
    long total = 0;
    for(int round = 0; round < 400; round++) {
        if(send(sk, pkt, (size_t)n, 0) != n) return 0;
        int len = recv_packet(rsp, sizeof rsp, 8000);
        if(len < 3) return 0;
        uint8_t code = rsp[0];
        if(code != 0x90 && code != 0xa0) {
            fprintf(stderr, "bfbt-cover: GET %s recusado (0x%02x)\n", handle, code);
            return 0;
        }
        for(int i = 3; i + 2 < len;) {
            uint8_t id = rsp[i];
            if((id & 0xc0) == 0xc0) { i += 5; continue; }
            if((id & 0xc0) == 0x80) { i += 2; continue; }
            int l = rsp[i + 1] << 8 | rsp[i + 2];
            if(l < 3 || i + l > len) break;
            if(id == 0x48 || id == 0x49) {              /* Body / End of Body */
                fwrite(rsp + i + 3, 1, (size_t)(l - 3), f);
                total += l - 3;
            }
            i += l;
        }
        if(code == 0xa0) return total > 0;
        n = 3;                                          /* próximo pedaço: só o Connection ID */
        pkt[0] = 0x83;
        if(have_cid) { pkt[n++] = 0xcb; memcpy(pkt + n, conn_id, 4); n += 4; }
        pkt[1] = (uint8_t)(n >> 8);
        pkt[2] = (uint8_t)n;
    }
    return 0;
}

int main(int argc, char **argv)
{
    if(argc < 4) { fprintf(stderr, "uso: bfbt-cover ENDERECO PSM SAIDA.jpg\n"); return 2; }
    unsigned v[6];
    if(sscanf(argv[1], "%x:%x:%x:%x:%x:%x", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) != 6) return 2;
    signal(SIGPIPE, SIG_IGN);
    sk = socket(AF_BT, SOCK_SEQPACKET | SOCK_CLOEXEC, BTPROTO_L2CAP);
    if(sk < 0) { perror("socket"); return 1; }
    struct l2cap_options o;
    socklen_t ol = sizeof o;
    if(getsockopt(sk, SOL_L2CAP, L2CAP_OPTIONS, &o, &ol) == 0) {
        o.mode = L2CAP_MODE_ERTM;
        o.imtu = MTU;
        o.omtu = MTU;
        setsockopt(sk, SOL_L2CAP, L2CAP_OPTIONS, &o, sizeof o);
    }
    struct bt_security sec = { BT_SECURITY_MEDIUM, 0 };
    setsockopt(sk, SOL_BLUETOOTH, BT_SECURITY, &sec, sizeof sec);
    struct sockaddr_l2 a;
    memset(&a, 0, sizeof a);
    a.family = AF_BT;
    a.psm = (unsigned short)strtoul(argv[2], NULL, 0);
    for(int i = 0; i < 6; i++) a.bdaddr[i] = (uint8_t)v[5 - i];
    if(connect(sk, (struct sockaddr *)&a, sizeof a) < 0) { perror("bfbt-cover: L2CAP"); return 1; }
    if(!obex_connect()) return 1;
    printf("conectado\n");
    fflush(stdout);
    char line[128], tmp[300];
    snprintf(tmp, sizeof tmp, "%s.tmp", argv[3]);
    while(fgets(line, sizeof line, stdin)) {
        line[strcspn(line, "\r\n ")] = 0;
        if(!line[0]) continue;
        FILE *f = fopen(tmp, "wb");
        if(!f) { printf("erro %s\n", line); fflush(stdout); continue; }
        int ok = obex_get_thumbnail(line, f);
        fclose(f);
        if(ok && rename(tmp, argv[3]) == 0) printf("ok %s\n", line);
        else { unlink(tmp); printf("erro %s\n", line); }
        fflush(stdout);
        if(!ok && errno == ENOTCONN) break;
    }
    close(sk);
    return 0;
}
