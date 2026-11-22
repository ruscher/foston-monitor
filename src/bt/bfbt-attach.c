/* bfbt-attach — liga o Bluetooth MediaTek MT6611 do FOSTON e entrega a UART ao kernel.
 *
 *   bfbt-attach [-f] [-b BAUD]
 *                        liga, confere com HCI Reset, anexa /dev/ttyS1 como hci (H4) e fica
 *                        em segundo plano segurando a porta (-f: em primeiro plano);
 *                        SIGTERM/SIGINT desanexam e cortam a alimentação
 *   bfbt-attach off      só corta a alimentação
 *
 * -b BAUD (230400, 460800, 921600): troca a velocidade como o mtk_bthuart.dll
 * (Radio_ChangeControllerUART): comando 0xFC77 {código, 0x40 = RTS/CTS}, o chip responde
 * "Command Status" na velocidade antiga, o lado do Linux muda a UART, manda o byte 0xFF
 * (acordar) e espera "Command Complete" do opcode 0xFCC0. Os 3,2 Mbaud do CE precisam do modo
 * de alta velocidade da UART da MediaTek, que o driver 8250 daqui não usa.
 *
 * Sequência do mtk_bthuart.dll do Windows CE (docs/hardware-enablement/06): LDO_EN = GPIO 104,
 * RST = GPIO 3, os dois como saída sem pull em 0; LDO_EN = 1; 3 ms; RST = 1; 1 s; HCI Reset a
 * 115200 com RTS/CTS; até 3 tentativas. A ROM do chip funciona sem o patch da MediaTek.
 * Registradores do GPIO: 0x80002000 + tipo * 0x100 + (pino / 32) * 0x10 (os mesmos do bfgps).
 * Faz o papel do btattach do BlueZ (que este sistema estático não tem).
 * SPDX-License-Identifier: GPL-3.0-or-later */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <syslog.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#define PIN_LDO 104
#define PIN_RST 3
#define TTY "/dev/ttyS1"
#ifndef N_HCI
#define N_HCI 15
#endif
#define HCIUARTSETPROTO _IOW('U', 200, int)
#define HCIUARTGETDEVICE _IOR('U', 202, int)
#define HCIUARTSETFLAGS _IOW('U', 203, int)
#define HCI_UART_H4 0

enum { T_DIR = 0, T_PU = 1, T_PD = 2, T_OUT = 3 };
static volatile uint32_t *g;
static volatile sig_atomic_t quit;

static void set(int type, int pin, int v)
{
    volatile uint32_t *r = g + (type * 0x100 + (pin / 32) * 0x10) / 4;
    uint32_t b = 1u << (pin % 32);
    *r = v ? (*r | b) : (*r & ~b);
}

static void ms(int n) { struct timespec t = { n / 1000, (n % 1000) * 1000000L }; nanosleep(&t, NULL); }

static void power(int on)
{
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

static int open_tty(void)
{
    int fd = open(TTY, O_RDWR | O_NOCTTY);
    if(fd < 0) return -1;
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

/* HCI Reset -> 04 0E 04 xx 03 0C 00 */
static int hci_reset(int fd)
{
    static const unsigned char cmd[] = { 0x01, 0x03, 0x0c, 0x00 };
    if(write(fd, cmd, sizeof cmd) != sizeof cmd) return 0;
    unsigned char b[32];
    int n = 0;
    struct pollfd p = { fd, POLLIN, 0 };
    while(n < 7 && poll(&p, 1, 1500) > 0) {
        ssize_t r = read(fd, b + n, sizeof b - (size_t)n);
        if(r <= 0) break;
        n += (int)r;
    }
    for(int i = 0; i + 6 < n; i++)
        if(b[i] == 0x04 && b[i + 1] == 0x0e && b[i + 4] == 0x03 && b[i + 5] == 0x0c) return b[i + 6] == 0;
    return 0;
}

static speed_t bconst(int b)
{
    return b == 230400 ? B230400 : b == 460800 ? B460800 : b == 921600 ? B921600 : B115200;
}

/* lê bytes até achar a sequência want (até ms) */
static int expect(int fd, const unsigned char *want, int n, int ms_)
{
    unsigned char b[64];
    int k = 0;
    struct pollfd p = { fd, POLLIN, 0 };
    while(k < (int)sizeof b && poll(&p, 1, ms_) > 0) {
        ssize_t r = read(fd, b + k, sizeof b - (size_t)k);
        if(r <= 0) break;
        k += (int)r;
        for(int i = 0; i + n <= k; i++) if(!memcmp(b + i, want, (size_t)n)) return 1;
    }
    return 0;
}

static int set_baud(int fd, int baud)
{
    int code = baud == 230400 ? 9 : baud == 460800 ? 0xa : baud == 921600 ? 0xb : 0;
    if(!code) { fprintf(stderr, "bfbt-attach: velocidade %d não suportada\n", baud); return 0; }
    const unsigned char cmd[] = { 0x01, 0x77, 0xfc, 0x02, (unsigned char)code, 0x40 };
    static const unsigned char status[] = { 0x04, 0x0f, 0x04, 0x00, 0x01, 0x77, 0xfc };
    static const unsigned char done[] = { 0x04, 0x0e, 0x04, 0x01, 0xc0, 0xfc, 0x00 };
    if(write(fd, cmd, sizeof cmd) != sizeof cmd || !expect(fd, status, sizeof status, 1000)) {
        fprintf(stderr, "bfbt-attach: o chip não aceitou a troca de velocidade\n");
        return 0;
    }
    tcdrain(fd);
    struct termios t;
    tcgetattr(fd, &t);
    cfsetispeed(&t, bconst(baud));
    cfsetospeed(&t, bconst(baud));
    tcsetattr(fd, TCSANOW, &t);
    ms(10);
    tcflush(fd, TCIFLUSH);
    static const unsigned char wake = 0xff;
    if(write(fd, &wake, 1) != 1 || !expect(fd, done, sizeof done, 1000)) {
        fprintf(stderr, "bfbt-attach: sem resposta a %d baud\n", baud);
        return 0;
    }
    return 1;
}

static void on_sig(int s) { (void)s; quit = 1; }

int main(int argc, char **argv)
{
    int fg = 0, baud = 115200;
    for(int i = 1; i < argc; i++) {
        if(!strcmp(argv[i], "-f")) fg = 1;
        else if(!strcmp(argv[i], "-b") && i + 1 < argc) baud = atoi(argv[++i]);
    }
    int mfd = open("/dev/mem", O_RDWR | O_SYNC);
    if(mfd < 0) { perror("/dev/mem"); return 1; }
    void *m = mmap(NULL, 0x1000, PROT_READ | PROT_WRITE, MAP_SHARED, mfd, 0x80002000);
    if(m == MAP_FAILED) { perror("mmap"); return 1; }
    g = m;
    if(argc > 1 && !strcmp(argv[1], "off")) { power(0); return 0; }
    openlog("bfbt-attach", LOG_PID, LOG_DAEMON);

    int fd = -1, ok = 0;
    for(int tr = 0; tr < 3 && !ok; tr++) {
        power(1);
        if(fd >= 0) close(fd);
        fd = open_tty();
        if(fd < 0) { perror(TTY); power(0); return 1; }
        ok = hci_reset(fd);
    }
    if(!ok) {
        fprintf(stderr, "bfbt-attach: o chip não respondeu ao HCI Reset\n");
        syslog(LOG_ERR, "o chip não respondeu ao HCI Reset");
        close(fd);
        power(0);
        return 1;
    }
    if(baud != 115200 && (!set_baud(fd, baud) || !hci_reset(fd))) {
        syslog(LOG_ERR, "troca para %d baud falhou", baud);
        close(fd);
        power(0);
        return 1;
    }
    int ld = N_HCI;
    if(ioctl(fd, TIOCSETD, &ld) < 0) { perror("TIOCSETD N_HCI (kernel sem CONFIG_BT_HCIUART?)"); power(0); return 1; }
    if(ioctl(fd, HCIUARTSETFLAGS, 0) < 0 || ioctl(fd, HCIUARTSETPROTO, HCI_UART_H4) < 0) {
        perror("HCIUARTSETPROTO");
        power(0);
        return 1;
    }
    int dev = ioctl(fd, HCIUARTGETDEVICE, 0);
    printf("bfbt-attach: MT6611 anexado como hci%d (%s, %d baud, H4)\n", dev, TTY, baud);
    syslog(LOG_INFO, "MT6611 anexado como hci%d", dev);
    fflush(stdout);
    if(!fg && daemon(0, 0) < 0) { perror("daemon"); return 1; }
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_sig;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);
    while(!quit) pause();
    ld = N_TTY;
    ioctl(fd, TIOCSETD, &ld);
    close(fd);
    power(0);
    syslog(LOG_INFO, "Bluetooth desligado");
    return 0;
}
