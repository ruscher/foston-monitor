/* btprobe.c — liga o chip Bluetooth MediaTek do FOSTON como o mtk_bthuart.dll do CE e
 * confere se ele responde a um HCI Reset na UART2 (/dev/ttyS1).
 *
 *   btprobe read            mostra direção/pull/saída/entrada dos pinos (só leitura)
 *   btprobe on              alimenta e solta o reset (sequência do Radio_HWTurnOn)
 *   btprobe off             corta a alimentação (LDO_EN = 0) e prende o reset
 *   btprobe reset [baud]    abre /dev/ttyS1 (padrão 115200, RTS/CTS), manda HCI Reset
 *                           (01 03 0C 00) e mostra a resposta
 *   btprobe probe [baud]    on + reset; se não responder, off
 *   btprobe cmd HEX...      manda um comando HCI (bytes em hexa, sem o 01) a 115200 e
 *                           mostra o evento de resposta
 *   btprobe visible [s]     nome "BigFoston", visível e conectável por s segundos (padrão
 *                           120): um celular procurando aparelhos deve vê-lo; mostra os
 *                           pedidos de conexão que chegarem
 *   btprobe scan [s]        procura aparelhos (HCI Inquiry com RSSI, ~s segundos, padrão
 *                           10): mostra classe, RSSI e o fim do endereço (-a: inteiro)
 *
 * Pinos (OALIoCtlHalGetBTHWPin do CE): LDO_EN = GPIO 104, RST = GPIO 3, EINT = 11.
 * Registradores do GPIO (os mesmos do bfgps e do driver GIO1: do CE): 0x80002000 +
 * tipo * 0x100 + (pino / 32) * 0x10, tipo 0 direção, 1 pull-up, 2 pull-down, 3 saída,
 * 4 entrada. Só esses endereços são lidos ou escritos.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#define PIN_LDO 104
#define PIN_RST 3
enum { T_DIR = 0, T_PU = 1, T_PD = 2, T_OUT = 3, T_IN = 4 };
static volatile uint32_t *g;

static volatile uint32_t *reg(int type, int pin) { return g + (type * 0x100 + (pin / 32) * 0x10) / 4; }
static int get(int type, int pin) { return (*reg(type, pin) >> (pin % 32)) & 1; }
static void set(int type, int pin, int v)
{
    volatile uint32_t *r = reg(type, pin);
    uint32_t b = 1u << (pin % 32);
    *r = v ? (*r | b) : (*r & ~b);
}

static void ms(int n) { struct timespec t = { n / 1000, (n % 1000) * 1000000L }; nanosleep(&t, NULL); }

static void show(void)
{
    static const int pins[] = { PIN_LDO, PIN_RST, 11 };
    static const char *nm[] = { "LDO_EN", "RST", "EINT" };
    for(int i = 0; i < 3; i++)
        printf("%-6s GPIO %3d: dir=%d pu=%d pd=%d out=%d in=%d\n", nm[i], pins[i], get(T_DIR, pins[i]),
               get(T_PU, pins[i]), get(T_PD, pins[i]), get(T_OUT, pins[i]), get(T_IN, pins[i]));
}

static void hw(int on)
{
    /* Radio_HWTurnOn: os dois pinos como saída, sem pull, em 0 */
    for(int k = 0; k < 2; k++) {
        int p = k ? PIN_RST : PIN_LDO;
        set(T_DIR, p, 1);
        set(T_PU, p, 0);
        set(T_PD, p, 0);
        set(T_OUT, p, 0);
    }
    if(!on) return;
    set(T_OUT, PIN_LDO, 1);
    ms(3);
    set(T_OUT, PIN_RST, 1);
    ms(1000);
}

static speed_t bconst(int b)
{
    switch(b) {
    case 115200: return B115200; case 230400: return B230400; case 460800: return B460800;
    case 921600: return B921600; case 3000000: return B3000000; case 3500000: return B3500000;
    default: return B115200;
    }
}

static int hci_reset(int baud, int rtscts)
{
    int fd = open("/dev/ttyS1", O_RDWR | O_NOCTTY | O_NONBLOCK);
    if(fd < 0) { perror("/dev/ttyS1"); return -1; }
    struct termios t;
    tcgetattr(fd, &t);
    cfmakeraw(&t);
    cfsetispeed(&t, bconst(baud));
    cfsetospeed(&t, bconst(baud));
    t.c_cflag |= CLOCAL | CREAD;
    if(rtscts) t.c_cflag |= CRTSCTS; else t.c_cflag &= ~CRTSCTS;
    tcsetattr(fd, TCSANOW, &t);
    tcflush(fd, TCIOFLUSH);
    static const unsigned char cmd[] = { 0x01, 0x03, 0x0c, 0x00 };
    ssize_t w = write(fd, cmd, sizeof cmd);
    printf("HCI Reset a %d baud, RTS/CTS %s: escritos %zd de 4\n", baud, rtscts ? "sim" : "não", w);
    unsigned char buf[64];
    int n = 0;
    struct pollfd p = { fd, POLLIN, 0 };
    while(n < (int)sizeof buf && poll(&p, 1, 1500) > 0) {
        ssize_t r = read(fd, buf + n, sizeof buf - n);
        if(r <= 0) break;
        n += (int)r;
        if(n >= 7) break;
    }
    printf("resposta (%d bytes):", n);
    for(int i = 0; i < n; i++) printf(" %02x", buf[i]);
    printf("\n");
    close(fd);
    /* comando concluído: 04 0E 04 <n> 03 0C <status> */
    for(int i = 0; i + 6 < n; i++)
        if(buf[i] == 0x04 && buf[i + 1] == 0x0e && buf[i + 4] == 0x03 && buf[i + 5] == 0x0c) return buf[i + 6] == 0 ? 1 : 0;
    return 0;
}

static int hci_cmd(int argc, char **argv)
{
    int fd = open("/dev/ttyS1", O_RDWR | O_NOCTTY | O_NONBLOCK);
    if(fd < 0) { perror("/dev/ttyS1"); return 1; }
    struct termios t;
    tcgetattr(fd, &t);
    cfmakeraw(&t);
    cfsetispeed(&t, B115200);
    cfsetospeed(&t, B115200);
    t.c_cflag |= CLOCAL | CREAD | CRTSCTS;
    tcsetattr(fd, TCSANOW, &t);
    tcflush(fd, TCIOFLUSH);
    unsigned char c[260] = { 0x01 };
    int n = 1;
    for(int i = 0; i < argc && n < (int)sizeof c; i++) c[n++] = (unsigned char)strtoul(argv[i], NULL, 16);
    if(write(fd, c, (size_t)n) != n) { perror("write"); return 1; }
    unsigned char b[300];
    int k = 0;
    struct pollfd p = { fd, POLLIN, 0 };
    while(k < (int)sizeof b && poll(&p, 1, 800) > 0) {
        ssize_t r = read(fd, b + k, sizeof b - k);
        if(r <= 0) break;
        k += (int)r;
        if(k >= 3 && k >= 3 + b[2]) break;
    }
    printf("evento (%d bytes):", k);
    for(int i = 0; i < k; i++) printf(" %02x", b[i]);
    printf("\n");
    close(fd);
    return 0;
}

static int open_uart(void)
{
    int fd = open("/dev/ttyS1", O_RDWR | O_NOCTTY | O_NONBLOCK);
    if(fd < 0) { perror("/dev/ttyS1"); return -1; }
    struct termios t;
    tcgetattr(fd, &t);
    cfmakeraw(&t);
    cfsetispeed(&t, B115200);
    cfsetospeed(&t, B115200);
    t.c_cflag |= CLOCAL | CREAD | CRTSCTS;
    tcsetattr(fd, TCSANOW, &t);
    tcflush(fd, TCIOFLUSH);
    return fd;
}

/* lê um evento HCI inteiro (04 code len params), espera até ms */
static int read_event(int fd, unsigned char *b, int max, int ms_)
{
    int k = 0, need = 3;
    struct pollfd p = { fd, POLLIN, 0 };
    while(k < need && poll(&p, 1, ms_) > 0) {
        ssize_t r = read(fd, b + k, (size_t)(need - k));
        if(r <= 0) break;
        if(k == 0 && b[0] != 0x04) continue;          /* só eventos */
        k += (int)r;
        if(k >= 3) need = 3 + b[2];
        if(need > max) return -1;
    }
    return k == need ? k : 0;
}

static int scan(int secs, int full)
{
    int fd = open_uart();
    if(fd < 0) return 1;
    unsigned char b[300];
    static const unsigned char mode[] = { 0x01, 0x45, 0x0c, 0x01, 0x01 };   /* Write Inquiry Mode: RSSI */
    if(write(fd, mode, sizeof mode) != sizeof mode) return 1;
    read_event(fd, b, sizeof b, 500);
    int len = secs * 100 / 128;
    if(len < 1) len = 1;
    if(len > 0x30) len = 0x30;
    unsigned char inq[] = { 0x01, 0x01, 0x04, 0x05, 0x33, 0x8b, 0x9e, (unsigned char)len, 0x00 };   /* GIAC */
    if(write(fd, inq, sizeof inq) != sizeof inq) return 1;
    int found = 0, done = 0;
    char seen[64][6];
    while(!done) {
        int n = read_event(fd, b, sizeof b, (secs + 5) * 1000);
        if(n <= 0) break;
        unsigned char ev = b[1];
        if(ev == 0x0f && b[3] != 0) { printf("inquiry recusado: status %02x\n", b[3]); break; }
        if(ev == 0x01) { done = 1; break; }                        /* Inquiry Complete */
        if(ev == 0x02 || ev == 0x22 || ev == 0x2f) {
            int nr = ev == 0x2f ? 1 : b[3];
            for(int i = 0; i < nr; i++) {
                const unsigned char *a, *cod;
                int rssi = 0;
                if(ev == 0x02) { a = b + 4 + 6 * i; cod = b + 4 + 6 * nr + 3 * nr + 3 * i; }
                else if(ev == 0x22) { a = b + 4 + 6 * i; cod = b + 4 + 9 * nr + 3 * i; rssi = (signed char)b[4 + 14 * nr + i]; }
                else { a = b + 4; cod = b + 4 + 6 + 3; rssi = (signed char)b[4 + 6 + 3 + 3 + 2]; }
                int dup = 0;
                for(int k = 0; k < found && k < 64; k++) if(!memcmp(seen[k], a, 6)) dup = 1;
                if(dup) continue;
                if(found < 64) memcpy(seen[found], a, 6);
                found++;
                unsigned major = (cod[1] & 0x1f);
                static const char *mj[] = { "diverso", "computador", "telefone", "rede", "áudio/vídeo", "periférico",
                                            "imagem", "vestível", "brinquedo", "saúde" };
                if(full) printf("  %02x:%02x:%02x:%02x:%02x:%02x", a[5], a[4], a[3], a[2], a[1], a[0]);
                else printf("  ..:..:..:..:%02x:%02x", a[1], a[0]);
                printf("  classe %02x%02x%02x (%s)  RSSI %d dBm\n", cod[2], cod[1], cod[0], major < 10 ? mj[major] : "?", rssi);
            }
        }
    }
    printf("%d aparelho(s) encontrados%s\n", found, done ? "" : " (sem Inquiry Complete)");
    close(fd);
    return 0;
}

static int send_cmd(int fd, const unsigned char *c, int n, unsigned char *b)
{
    if(write(fd, c, (size_t)n) != n) return -1;
    int k = read_event(fd, b, 300, 1000);
    return k > 6 && b[1] == 0x0e ? b[6] : -1;          /* status do Command Complete */
}

static int visible(int secs)
{
    int fd = open_uart();
    if(fd < 0) return 1;
    unsigned char b[300], name[4 + 248] = { 0x01, 0x13, 0x0c, 248 };
    memcpy(name + 4, "BigFoston", 9);
    static const unsigned char cod[] = { 0x01, 0x24, 0x0c, 0x03, 0x08, 0x04, 0x20 };   /* áudio: viva-voz */
    static const unsigned char scan_on[] = { 0x01, 0x1a, 0x0c, 0x01, 0x03 };
    static const unsigned char scan_off[] = { 0x01, 0x1a, 0x0c, 0x01, 0x00 };
    printf("nome: status %d\n", send_cmd(fd, name, sizeof name, b));
    printf("classe: status %d\n", send_cmd(fd, cod, sizeof cod, b));
    printf("visível: status %d — procure \"BigFoston\" no celular (%d s)\n", send_cmd(fd, scan_on, sizeof scan_on, b), secs);
    fflush(stdout);
    time_t end = time(NULL) + secs;
    while(time(NULL) < end) {
        int n = read_event(fd, b, sizeof b, 1000);
        if(n <= 0) continue;
        if(b[1] == 0x04) {                                  /* Connection Request */
            printf("pedido de conexão: classe %02x%02x%02x, tipo de enlace %d\n", b[11], b[10], b[9], b[12]);
            /* recusa (não há pilha para atender): Reject Connection Request, motivo 0x0d */
            unsigned char rej[] = { 0x01, 0x0a, 0x04, 0x07, b[3], b[4], b[5], b[6], b[7], b[8], 0x0d };
            if(write(fd, rej, sizeof rej) != (ssize_t)sizeof rej) break;
        } else printf("evento %02x (%d bytes)\n", b[1], n);
        fflush(stdout);
    }
    send_cmd(fd, scan_off, sizeof scan_off, b);
    close(fd);
    return 0;
}

int main(int argc, char **argv)
{
    if(argc < 2) { fprintf(stderr, "uso: %s read|on|off|reset [baud]|probe [baud]\n", argv[0]); return 2; }
    int fd = open("/dev/mem", O_RDWR | O_SYNC);
    if(fd < 0) { perror("/dev/mem"); return 1; }
    void *m = mmap(NULL, 0x1000, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0x80002000);
    if(m == MAP_FAILED) { perror("mmap"); return 1; }
    g = m;
    int baud = argc > 2 ? atoi(argv[2]) : 115200;
    if(!strcmp(argv[1], "read")) show();
    else if(!strcmp(argv[1], "on")) { hw(1); show(); }
    else if(!strcmp(argv[1], "off")) { hw(0); show(); }
    else if(!strcmp(argv[1], "reset")) return hci_reset(baud, 1) == 1 ? 0 : 1;
    else if(!strcmp(argv[1], "cmd")) return hci_cmd(argc - 2, argv + 2);
    else if(!strcmp(argv[1], "visible")) return visible(argc > 2 ? atoi(argv[2]) : 120);
    else if(!strcmp(argv[1], "scan")) return scan(argc > 2 && argv[2][0] != '-' ? atoi(argv[2]) : 10, argc > 2 && !strcmp(argv[argc - 1], "-a"));
    else if(!strcmp(argv[1], "probe")) {
        hw(1);
        show();
        int ok = hci_reset(baud, 1) == 1 || hci_reset(baud, 0) == 1;
        printf(ok ? "o chip respondeu: HCI Reset concluído\n" : "sem resposta: desligando\n");
        if(!ok) { hw(0); show(); }
        return ok ? 0 : 1;
    } else { fprintf(stderr, "comando desconhecido\n"); return 2; }
    return 0;
}
