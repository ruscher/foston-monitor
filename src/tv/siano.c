/* siano.c — sintonizador Siano "Nova B0" (ISDB-T 1seg) do FOSTON no SPI do MT3351 (ver siano.h).
 *
 * Origem: bftv.c (primeira etapa, ver o cabeçalho abaixo).
 *
 * Primeira etapa (docs/tv): trazer o chip à vida em espaço de usuário, pelos mesmos registradores
 * e na mesma ordem do driver do Windows CE (SianoSPI.dll / buses.dll / SmsGenDrv.dll):
 *
 *   bftv on                     liga e reseta o chip (GPIO0 alimentação, GPIO1 reset)
 *   bftv off                    desliga o chip
 *   bftv boot FW.inp            liga, padrão de despertar, msg 710, firmware (660/664)
 *   bftv version                pede a versão (668/669)
 *   bftv tune MHZ [SEG]         modo ISDB-T (578), sintoniza 1seg (776) e mostra estatísticas
 *   bftv dump MHZ SEGUNDOS OUT  sintoniza, filtra o PAT e grava o que chegar (TS) em OUT
 *
 * SPI: controlador 0x8004E000 (bloco "mt65xx" da MediaTek), clock id 36 (PDN1 bit 12), modo 0,
 * MSB primeiro. O CE usa DMA; aqui é FIFO (PIO), em pedaços de 32 bytes com o CS mantido pelo
 * modo pausa. O chip avisa por EINT2, mas o Linux não trata o EINT: o bftv lê por consulta.
 * Só registradores que o CE comprovadamente acessa (docs/tv/02-hardware-sintonizador.md).
 *
 * O firmware (.inp) é da Siano: fica no cartão (tools/bigfoston/voice-extract.py --tvfw).
 * SPDX-License-Identifier: GPL-3.0-or-later */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include "siano.h"

/* ------------------------------------------------------------ registradores */

static volatile uint8_t *gpio, *sys, *spi;   /* 0x80002000, 0x80001000, 0x8004E000 */

static uint32_t rd(volatile uint8_t *b, uint32_t o) { return *(volatile uint32_t *)(b + o); }
static void wr(volatile uint8_t *b, uint32_t o, uint32_t v) { *(volatile uint32_t *)(b + o) = v; }

int map(void)
{
    int fd = open("/dev/mem", O_RDWR | O_SYNC | O_CLOEXEC);
    if(fd < 0) { perror("/dev/mem"); return -1; }
    gpio = mmap(NULL, 0x1000, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0x80002000);
    sys = mmap(NULL, 0x1000, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0x80001000);
    spi = mmap(NULL, 0x1000, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0x8004E000);
    close(fd);
    if(gpio == MAP_FAILED || sys == MAP_FAILED || spi == MAP_FAILED) { perror("mmap"); return -1; }
    return 0;
}

void msleep(int ms) { struct timespec t = { ms / 1000, (ms % 1000) * 1000000L }; nanosleep(&t, NULL); }

long now_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

/* GPIO 0..31: DIR +0x000, DOUT +0x300, DIN +0x400 */
void gpio_dir(int pin, int out) { uint32_t v = rd(gpio, 0); wr(gpio, 0, out ? v | 1u << pin : v & ~(1u << pin)); }
void gpio_set(int pin, int on) { uint32_t v = rd(gpio, 0x300); wr(gpio, 0x300, on ? v | 1u << pin : v & ~(1u << pin)); }

/* ------------------------------------------------------------ energia (CE 0x2942a10) */

void power_on(void)
{
    wr(sys, 0x700, rd(sys, 0x700) & ~0x3u);   /* 0x80001700 bits[1:0] = 0 (CE) */
    gpio_dir(0, 1);
    gpio_dir(1, 1);
    gpio_set(0, 1);                             /* alimentação */
    msleep(10);
    gpio_set(1, 1);
    msleep(60);
    gpio_set(1, 0);                             /* reset */
    msleep(60);
    gpio_set(1, 1);
    msleep(100);
    gpio_dir(2, 0);                             /* interrupção do chip (EINT2) */
    wr(gpio, 0x610, rd(gpio, 0x610) & ~(1u << 2));   /* ICFG bit 2 = 0 (CE) */
}

void power_off(void)
{
    gpio_set(1, 0);
    msleep(60);
    gpio_set(0, 0);
    wr(sys, 0x324, 1u << 12);                   /* clock do SPI (id 36) desligado */
}

/* ------------------------------------------------------------ SPI (mt65xx, PIO) */

#define SPI_CFG0 0x00
#define SPI_CFG1 0x04
#define SPI_TX   0x10
#define SPI_RX   0x14
#define SPI_CMD  0x18
#define SPI_ST0  0x1C
#define SPI_ST1  0x20
#define CMD_ACT     (1u << 0)
#define CMD_RESUME  (1u << 1)
#define CMD_PAUSE   (1u << 4)
#define CMD_MSB     (3u << 12)

int clk_div = 15;                        /* ClkLow/High: 15 no download, 7 depois (CE) */

void spi_setup(void)
{
    wr(sys, 0x344, 1u << 12);                   /* clock do SPI (id 36) ligado */
    uint32_t pm = rd(sys, 0x700);               /* pinos do SPI: 0x80001700 bits[3:2] = 2 */
    if((pm & 0xC) != 0x8) wr(sys, 0x700, (pm & ~0xCu) | 0x8);
    wr(spi, SPI_CFG0, 10u << 24 | 0u << 16 | (uint32_t)clk_div << 8 | (uint32_t)clk_div);
    wr(spi, SPI_CFG1, (rd(spi, SPI_CFG1) & ~0xFFu) | 9);
    wr(spi, SPI_CMD, CMD_MSB);                  /* modo 0, MSB primeiro, sem DMA, sem IRQ */
}

static unsigned long spi_timeouts;

static int spi_wait(void)
{
    for(long i = 0; i < 200000; i++) {
        uint32_t s = rd(spi, SPI_ST0) & 3;
        if(s) return (int)s;
    }
    spi_timeouts++;
    return 0;
}

/* full duplex de len bytes (múltiplo de 4); CS ativo do começo ao fim (modo pausa) */
static int spi_xfer(const uint8_t *tx, uint8_t *rx, int len)
{
    for(int off = 0; off < len; off += 32) {
        int n = len - off < 32 ? len - off : 32;
        int last = off + n >= len;
        wr(spi, SPI_CFG1, (rd(spi, SPI_CFG1) & 0xFC0000FFu) | (uint32_t)(n - 1) << 16);
        for(int i = 0; i < n; i += 4) {
            uint32_t w = 0;
            if(tx) memcpy(&w, tx + off + i, 4);
            wr(spi, SPI_TX, w);
        }
        uint32_t cmd = CMD_MSB | (last ? 0 : CMD_PAUSE);
        wr(spi, SPI_CMD, cmd);
        wr(spi, SPI_CMD, cmd | (off ? CMD_RESUME : CMD_ACT));
        if(!spi_wait()) return -1;
        for(int i = 0; i < n; i += 4) {
            uint32_t w = rd(spi, SPI_RX);
            if(rx) memcpy(rx + off + i, &w, 4);
        }
    }
    return 0;
}

/* ------------------------------------------------------------ mensagens Siano */

static const uint8_t PRE[4] = { 0xA5, 0x5A, 0xE7, 0x7E };


/* recepção: o que chegar em qualquer transferência passa pelo analisador */
#define MAXMSG 8192
static uint8_t acc[MAXMSG * 2];
static int acc_n;
void (*on_msg)(const hdr_t *h, const uint8_t *d, int n);

static void parse(const uint8_t *b, int n)
{
    if(acc_n + n > (int)sizeof acc) acc_n = 0;
    memcpy(acc + acc_n, b, (size_t)n);
    acc_n += n;
    for(;;) {
        int i = 0;
        while(i + 4 <= acc_n && memcmp(acc + i, PRE, 4)) i++;
        if(i + 4 > acc_n) { int keep = acc_n < 3 ? acc_n : 3; memmove(acc, acc + acc_n - keep, (size_t)keep); acc_n = keep; return; }
        if(i) { memmove(acc, acc + i, (size_t)(acc_n - i)); acc_n -= i; }
        if(acc_n < 12) return;
        hdr_t h;
        memcpy(&h, acc + 4, 8);
        if(h.len < 8 || h.len > MAXMSG) { memmove(acc, acc + 4, (size_t)(acc_n - 4)); acc_n -= 4; continue; }
        if(acc_n < 4 + h.len) return;
        const uint8_t *d = acc + 12;
        int dn = h.len - 8;
        if(h.flags & 4) { int pad = (h.flags >> 8) & 3; d += pad; dn -= pad; }
        if(on_msg) on_msg(&h, d, dn);
        int used = 4 + h.len;
        memmove(acc, acc + used, (size_t)(acc_n - used));
        acc_n -= used;
    }
}

int send_msg(uint16_t type, uint8_t src, const void *data, int dn)
{
    static uint8_t tx[1024], rx[1024];
    int total = 4 + 8 + dn;
    int padded = (total + 255) & ~255;
    if(padded > (int)sizeof tx) return -1;
    memset(tx, 0, (size_t)padded);
    memcpy(tx, PRE, 4);
    hdr_t h = { type, src, 11, (uint16_t)(8 + dn), 0 };
    memcpy(tx + 4, &h, 8);
    if(dn) memcpy(tx + 12, data, (size_t)dn);
    if(spi_xfer(tx, rx, padded)) return -1;
    parse(rx, padded);
    return 0;
}

/* uma leitura de 256 bytes (sem dados nossos) */
int poll_read(void)
{
    static uint8_t rx[256];
    if(spi_xfer(NULL, rx, 256)) return -1;
    parse(rx, 256);
    return 0;
}

/* espera uma resposta do tipo want (lendo por consulta) */
uint16_t got_type;
uint8_t got_data[512];
int got_n;
unsigned long n_msgs;

void store(const hdr_t *h, const uint8_t *d, int n)
{
    n_msgs++;
    got_type = h->type;
    got_n = n < (int)sizeof got_data ? n : (int)sizeof got_data;
    memcpy(got_data, d, (size_t)got_n);
    if(getenv("BFTV_TRACE")) fprintf(stderr, "  <- msg %u de %u para %u, %d bytes\n", h->type, h->src, h->dst, n);
}

int wait_msg(uint16_t want, int ms)
{
    long end = now_ms() + ms;
    got_type = 0;
    while(now_ms() < end) {
        if(got_type == want) return 0;
        if(poll_read()) return -1;
        if(got_type == want) return 0;
        msleep(2);
    }
    return -1;
}

/* ------------------------------------------------------------ firmware (CE 0x295193c) */

int fw_download(const char *path)
{
    FILE *f = fopen(path, "rb");
    if(!f) { perror(path); return -1; }
    uint32_t h[3];
    if(fread(h, 4, 3, f) != 3) { fclose(f); return -1; }
    uint32_t len = h[1], addr = h[2], entry = h[2];
    uint8_t *fw = malloc(len);
    if(!fw || fread(fw, 1, len, f) != len) { fclose(f); free(fw); fprintf(stderr, "%s: curto\n", path); return -1; }
    fclose(f);
    printf("firmware: %u bytes, endereço 0x%x\n", len, addr);
    /* padrão de despertar (sem preâmbulo) e linha de interrupção (710) */
    static const uint8_t wake[8] = { 0x00, 0x00, 0xDE, 0xC1, 0xA5, 0x51, 0xF1, 0xED };
    uint8_t rx[8];
    spi_xfer(wake, rx, 8);
    uint32_t il[3] = { 0, 19, 20 };
    send_msg(710, 35, il, sizeof il);
    on_msg = store;
    for(uint32_t off = 0; off < len; off += 240) {
        uint32_t n = len - off < 240 ? len - off : 240;
        uint8_t m[4 + 240];
        uint32_t a = addr + off;
        memcpy(m, &a, 4);
        memcpy(m + 4, fw + off, n);
        msleep(5);
        if(send_msg(660, 35, m, (int)(4 + n)) || wait_msg(661, 2000)) {
            fprintf(stderr, "download: sem resposta 661 em %u/%u (spi timeouts %lu, msgs %lu)\n", off, len, spi_timeouts, n_msgs);
            free(fw);
            return -1;
        }
        if(off % 4800 == 0) { printf("  %u/%u\n", off, len); fflush(stdout); }
    }
    free(fw);
    uint32_t trig[5] = { entry, 3, 0x200, 0, 1 };
    if(send_msg(664, 35, trig, sizeof trig) || wait_msg(665, 5000)) { fprintf(stderr, "sem 665 (partida)\n"); return -1; }
    msleep(150);
    clk_div = 7;                                /* SPI mais rápido depois do download (CE) */
    spi_setup();
    msleep(200);
    printf("firmware carregado e iniciado\n");
    return 0;
}

/* ------------------------------------------------------------ comandos */

int version(void)
{
    on_msg = store;
    if(send_msg(668, 150, NULL, 0) || wait_msg(669, 2000)) { fprintf(stderr, "sem resposta de versão (669)\n"); return -1; }
    /* SmsVersionRes: ChipModel(u16) Step(u8) MetalFix(u8) FirmwareId(u8) SupportedProtocols(u8) Major Minor Patch FieldPatch RomMajor RomMinor RomPatch RomFieldPatch TextLabel[34] */
    if(got_n >= 8) {
        uint16_t chip;
        memcpy(&chip, got_data, 2);
        printf("chip 0x%04x passo %u, firmware id %u, versão %u.%u.%u, ROM %u.%u.%u\n", chip, got_data[2], got_data[4],
               got_data[6], got_data[7], got_data[8], got_data[10], got_data[11], got_data[12]);
        if(got_n >= 48) { char lab[35]; memcpy(lab, got_data + 14, 34); lab[34] = 0; printf("rótulo: %s\n", lab); }
    }
    return 0;
}

int set_mode_tune(double mhz, int seg)
{
    on_msg = store;
    uint32_t mode = 5;                           /* DEVICE_MODE_ISDBT */
    if(send_msg(578, 151, &mode, 4) || wait_msg(579, 3000)) fprintf(stderr, "aviso: sem 579 (modo)\n");
    uint32_t t[4] = { (uint32_t)(mhz * 1e6 + 0.5), 4 /* 1seg */, 12000000, (uint32_t)seg };
    if(send_msg(776, 150, t, sizeof t) || wait_msg(777, 3000)) { fprintf(stderr, "sem 777 (sintonia)\n"); return -1; }
    printf("sintonizado em %.3f MHz (1seg, segmento %d)\n", mhz, seg);
    return 0;
}

siano_stats_t last_stats;

/* resposta 654: 1 palavra de status + struct sms_isdbt_stats (smscoreapi.h do Linux) */
/* quem chama escolhe on_msg: store, ou um tratador que passe a store o que não for TS (o bftvd
 * tocando; trocar por store perdia o TS que chegava durante a espera, uma falha por segundo) */
int stats(void)
{
    if(!on_msg) on_msg = store;
    if(send_msg(653, 150, NULL, 0) || wait_msg(654, 2000)) { last_stats.valid = 0; return -1; }
    int32_t v[17] = { 0 };
    int n = got_n - 4 < (int)sizeof v ? got_n - 4 : (int)sizeof v;
    if(n > 0) memcpy(v, got_data + 4, (size_t)n);
    last_stats.valid = 1;
    last_stats.rf_locked = v[2];
    last_stats.demod_locked = v[3];
    last_stats.snr = v[5];
    last_stats.rssi = v[6];
    last_stats.in_band_pwr = v[7];
    last_stats.carrier_offset = v[8];
    last_stats.frequency = (uint32_t)v[9];
    return 0;
}

