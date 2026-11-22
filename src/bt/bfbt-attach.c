/* bfbt-attach — liga o Bluetooth MediaTek MT6611 do FOSTON e entrega a UART ao kernel.
 *
 *   bfbt-attach [-f] [-b BAUD]
 *                        liga, confere com HCI Reset, anexa /dev/ttyS1 como hci (H4) e fica
 *                        em segundo plano segurando a porta (-f: em primeiro plano);
 *                        SIGTERM/SIGINT desanexam e cortam a alimentação
 *   bfbt-attach off      só corta a alimentação
 *
 * Endereço Bluetooth: o chip sai de fábrica com 00:00:46:66:11:01 (igual em todo MT6611).
 * Como o mtk_bthuart.dll (Radio_CmdMTKSetBDAddr), o endereço é gravado com o comando 0xFC1A
 * (6 bytes, o menos significativo primeiro). Ele vem de BDADDR_FILE no cartão (o mesmo
 * endereço que o Windows CE gerou, copiado do backup do aparelho, ou um gerado aqui e salvo
 * lá na primeira vez): é um identificador do aparelho e não fica no firmware.
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
    ms(100);                                      /* desligado de verdade antes de religar */
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

/* HCI Reset -> 04 0E 04 xx 03 0C 00; lê até achar a resposta (sobras de um evento anterior
 * não podem cortá-la) */
static int hci_reset(int fd)
{
    static const unsigned char cmd[] = { 0x01, 0x03, 0x0c, 0x00 };
    tcflush(fd, TCIFLUSH);
    if(write(fd, cmd, sizeof cmd) != sizeof cmd) return 0;
    unsigned char b[64];
    int n = 0;
    struct pollfd p = { fd, POLLIN, 0 };
    while(n < (int)sizeof b && poll(&p, 1, 1500) > 0) {
        ssize_t r = read(fd, b + n, sizeof b - (size_t)n);
        if(r <= 0) break;
        n += (int)r;
        for(int i = 0; i + 6 < n; i++)
            if(b[i] == 0x04 && b[i + 1] == 0x0e && b[i + 4] == 0x03 && b[i + 5] == 0x0c) return b[i + 6] == 0;
    }
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

#define BDADDR_FILE "/mnt/sd/BigFoston/bt/endereco"

/* "AA:BB:CC:DD:EE:FF" do arquivo, ou um novo (salvo se o cartão aceitar gravação) */
static int get_bdaddr(unsigned char a[6])
{
    unsigned v[6];
    FILE *f = fopen(BDADDR_FILE, "r");
    if(f) {
        int n = fscanf(f, "%x:%x:%x:%x:%x:%x", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]);
        fclose(f);
        if(n == 6) { for(int i = 0; i < 6; i++) a[i] = (unsigned char)v[i]; return 1; }
    }
    int r = open("/dev/urandom", O_RDONLY);
    if(r < 0 || read(r, a, 6) != 6) { if(r >= 0) close(r); return 0; }
    close(r);
    a[0] = 0x02;                                   /* administrado localmente, como o da USB */
    if(system("mkdir -p /mnt/sd/BigFoston/bt 2>/dev/null") == 0 && (f = fopen(BDADDR_FILE, "w"))) {
        fprintf(f, "%02X:%02X:%02X:%02X:%02X:%02X\n", a[0], a[1], a[2], a[3], a[4], a[5]);
        fclose(f);
    }
    return 1;
}

static int set_bdaddr(int fd)
{
    unsigned char a[6];
    if(!get_bdaddr(a)) return 0;
    unsigned char cmd[10] = { 0x01, 0x1a, 0xfc, 0x06 };
    for(int i = 0; i < 6; i++) cmd[4 + i] = a[5 - i];        /* o menos significativo primeiro */
    static const unsigned char done[] = { 0x04, 0x0e, 0x04, 0x01, 0x1a, 0xfc, 0x00 };
    if(write(fd, cmd, sizeof cmd) != sizeof cmd || !expect(fd, done, sizeof done, 1000)) return 0;
    return 1;
}

/* Como o mtk_bthuart.dll (Radio_CmdMTKDisableEsco): máscara de recursos LMP 0xFC1E sem eSCO.
 * Sem ela o kernel e o celular negociam eSCO, que este chip aceita mas nunca completa (o canal
 * de voz da ligação não abre); com SCO simples a voz vai pelos pinos PCM ao DSP. */
static int disable_esco(int fd)
{
    static const unsigned char cmd[] = { 0x01, 0x1e, 0xfc, 0x08, 0xff, 0xff, 0x8d, 0x7e, 0x98, 0x1f, 0x79, 0x83 };
    static const unsigned char done[] = { 0x04, 0x0e, 0x04, 0x01, 0x1e, 0xfc, 0x00 };
    return write(fd, cmd, sizeof cmd) == sizeof cmd && expect(fd, done, sizeof done, 1000);
}

/* Patch da ROM do MT6611, como o mtk_bthuart.dll (Radio_WritePatch): o CE lê a versão do chip
 * (0xFC D1 {04 00 00 80}) e manda o patch certo em pedaços de até 240 bytes pelo 0xFCC4
 * {estágio 0 primeiro / 1 meio / 2 último, tamanho, 0, 0, 0, dados}. O patch é da MediaTek:
 * vem do backup do próprio aparelho (tools/bigfoston/voice-extract.py --patch) para o cartão.
 * Sem ele o chip faz dados e música, mas a voz da ligação não sai pelos pinos PCM. */
#define PATCH_FILE "/mnt/sd/BigFoston/bt/mt6611-patch.bin"

static int vendor_cmd(int fd, unsigned ocf, const unsigned char *p, int n, unsigned char *ret, int retn)
{
    unsigned char cmd[260] = { 0x01, (unsigned char)ocf, 0xfc, (unsigned char)n };
    memcpy(cmd + 4, p, (size_t)n);
    if(write(fd, cmd, (size_t)n + 4) != n + 4) return 0;
    unsigned char b[128];
    int k = 0;
    struct pollfd pf = { fd, POLLIN, 0 };
    while(k < (int)sizeof b && poll(&pf, 1, 2000) > 0) {
        ssize_t r = read(fd, b + k, sizeof b - (size_t)k);
        if(r <= 0) break;
        k += (int)r;
        for(int i = 0; i + 6 < k; i++)
            if(b[i] == 0x04 && b[i + 1] == 0x0e && b[i + 4] == (unsigned char)ocf && b[i + 5] == 0xfc) {
                int plen = b[i + 2], need = i + 3 + plen;
                if(k < need) break;                          /* resto ainda chegando */
                for(int j = 0; j < retn && 6 + j < 3 + plen; j++) ret[j] = b[i + 6 + j];
                return 1;
            }
    }
    return 0;
}

static int write_patch(int fd)
{
    static const unsigned char rd[] = { 0x04, 0x00, 0x00, 0x80 };
    unsigned char ver[3] = { 0xff, 0, 0 };
    if(!vendor_cmd(fd, 0xd1, rd, sizeof rd, ver, 3) || ver[0] != 0) return -1;
    if(ver[1] != 3 || ver[2] != 0x8b) {
        syslog(LOG_WARNING, "MT6611 versão %d/0x%02x: patch não conhecido, segue sem", ver[1], ver[2]);
        return 0;
    }
    FILE *f = fopen(PATCH_FILE, "rb");
    if(!f) { syslog(LOG_INFO, "sem %s: segue sem o patch (sem voz nas ligações)", PATCH_FILE); return 0; }
    static unsigned char data[0x4000];
    size_t n = fread(data, 1, sizeof data, f);
    fclose(f);
    if(n != 0x1cdc) { syslog(LOG_WARNING, "%s com tamanho errado (%zu)", PATCH_FILE, n); return 0; }
    for(size_t off = 0; off < n;) {
        size_t len = n - off > 240 ? 240 : n - off;
        unsigned char p[245];
        p[0] = n - off > 240 ? (off ? 1 : 0) : 2;
        p[1] = (unsigned char)len;
        p[2] = p[3] = p[4] = 0;
        memcpy(p + 5, data + off, len);
        unsigned char st = 0xff;
        if(!vendor_cmd(fd, 0xc4, p, (int)len + 5, &st, 1) || st != 0) {
            syslog(LOG_ERR, "patch do MT6611 recusado em %zu (0x%02x)", off, st);
            return -1;
        }
        off += len;
    }
    syslog(LOG_INFO, "patch do MT6611 carregado (%zu bytes)", n);
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

    /* liga, HCI Reset, endereço e troca de velocidade; se algo falhar, corta a alimentação e
     * repete tudo (a troca de velocidade falha às vezes logo depois de ligar) */
    int fd = -1, ok = 0;
    for(int tr = 0; tr < 3 && !ok; tr++) {
        if(fd >= 0) close(fd);
        power(1);
        fd = open_tty();
        if(fd < 0) { perror(TTY); power(0); return 1; }
        if(!hci_reset(fd)) { fprintf(stderr, "bfbt-attach: sem resposta ao HCI Reset (tentativa %d)\n", tr + 1); continue; }
        if(!set_bdaddr(fd)) syslog(LOG_WARNING, "endereço Bluetooth não gravado (fica o de fábrica)");
        if(baud != 115200) {
            if(!set_baud(fd, baud)) { fprintf(stderr, "bfbt-attach: troca para %d baud falhou (tentativa %d)\n", baud, tr + 1); continue; }
            if(!hci_reset(fd)) { fprintf(stderr, "bfbt-attach: sem resposta ao HCI Reset a %d baud (tentativa %d)\n", baud, tr + 1); continue; }
        }
        if(write_patch(fd) < 0) {           /* na última tentativa segue sem o patch */
            fprintf(stderr, "bfbt-attach: patch falhou (tentativa %d)\n", tr + 1);
            if(tr < 2) continue;
            if(!hci_reset(fd)) continue;
        }
        if(!disable_esco(fd)) syslog(LOG_WARNING, "máscara sem eSCO (0xFC1E) não aceita");
        ok = 1;
    }
    if(!ok) {
        fprintf(stderr, "bfbt-attach: o chip Bluetooth não respondeu\n");
        syslog(LOG_ERR, "o chip não respondeu (%d baud)", baud);
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
