/* bffm — transmissor FM do FOSTON (KT Micro KT0801A no I2C do MT3351), como o FMC.dll do
 * Windows CE (docs/fm-radio/03-engenharia-reversa-fmc.md).
 *
 *   bffm status            estado (só leitura: GPIO, PWM, clock do I2C; registradores se ligado)
 *   bffm on [MHZ] [fm|ambos]  liga: clock de 32 kHz (PWM3), alimentação (GPIO4), confere PW_OK,
 *                          frequência; "fm" (padrão) desliga o alto-falante, como o CE
 *   bffm saida fm|ambos    só no rádio FM, ou FM e alto-falante juntos (ligado)
 *   bffm tom [SEGUNDOS]    tom de teste (bipes de 440/880 Hz, -12 dBFS, até 30 s) pelo /dev/dsp
 *   bffm freq MHZ          muda a frequência (ligado)
 *   bffm off               desliga o chip, a PWM3 e o clock do I2C
 *   bffm dump              registradores 0x00-0x1F do chip (ligado)
 *
 * Só usa o que o FMC.dll usa, com os mesmos valores:
 *   - I2C1 em 0x8002D000 (layout MT65xx), clock id 19 = PDN0 bit 19, escravo 0x36, TIMING 0x73F;
 *   - PWM3 em 0x8002B000 (+0x90 CON, +0xB8 WAVE_NUM, +0xBC DATA_WIDTH, +0xC0 THRESH, +0x00 bit 2
 *     liga): 58,5 MHz / 1786 = 32 754 Hz, o "cristal" do KT0801A; 0x80001700 bit 10 (pino da
 *     PWM) e OCFG 0x80002600 bit 23 = 0 (sem a saída de clock no pino);
 *   - GPIO4: alimentação do KT0801A (saída, sem pull), 500 ms até PW_OK (reg 0x0F bit 4);
 *   - frequência: CHSEL de 12 bits em passos de 50 kHz (reg0 = CHSEL[8:1], reg1[2:0] =
 *     CHSEL[11:9], reg2 bit 7 = CHSEL[0]); o CE usa passos de 100 kHz e escreve reg0 = 0x20,
 *     reg1 = 0xDB (RFGAIN e PGA do aparelho original; a potência não é alterada aqui).
 * Áudio: o KT0801A recebe o sinal analógico do DAC (o mesmo do alto-falante). O CE, ao ligar o
 * FM, manda waveOutMessage(0x41C, 0) e o driver de som desliga o amplificador do alto-falante
 * (GPIO5); aqui isso é o controle ALSA "Speaker Playback Switch" do mt3351-audio, devolvido ao
 * desligar o FM.
 * Faixa aceita: 88,0-108,0 MHz em passos de 0,1 MHz (radiodifusão FM no Brasil; o chip vai de 76 a
 * 108). Estado em /run/bigfoston-fm (state, freq, ...).
 * SPDX-License-Identifier: GPL-3.0-or-later */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <sound/asound.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/soundcard.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#define STATE "/run/bigfoston-fm"
#define LOCK  "/run/bigfoston-fm.lock"
#define FMIN 880
#define FMAX 1080

static volatile uint32_t *sys, *gpio, *pwm, *i2c;   /* 0x80001000, 0x80002000, 0x8002B000, 0x8002D000 */

static uint32_t rd(volatile uint32_t *b, unsigned off) { return b[off / 4]; }
static void wr(volatile uint32_t *b, unsigned off, uint32_t v) { b[off / 4] = v; }

static void msleep(int ms) { struct timespec t = { ms / 1000, (ms % 1000) * 1000000L }; nanosleep(&t, NULL); }
static long now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1000L + t.tv_nsec / 1000000; }

static int map(void)
{
    int fd = open("/dev/mem", O_RDWR | O_SYNC | O_CLOEXEC);
    if(fd < 0) { perror("/dev/mem"); return -1; }
    void *a = mmap(NULL, 0x1000, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0x80001000);
    void *b = mmap(NULL, 0x1000, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0x80002000);
    void *c = mmap(NULL, 0x1000, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0x8002B000);
    void *d = mmap(NULL, 0x1000, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0x8002D000);
    close(fd);
    if(a == MAP_FAILED || b == MAP_FAILED || c == MAP_FAILED || d == MAP_FAILED) { perror("mmap"); return -1; }
    sys = a; gpio = b; pwm = c; i2c = d;
    return 0;
}

/* ------------------------------------------------------------ I2C1 (como o buses.dll) */

enum { I_DATA = 0x00, I_SLAVE = 0x04, I_MASK = 0x08, I_STAT = 0x0C, I_CTRL = 0x10, I_TLEN = 0x14,
       I_TRLEN = 0x18, I_TIMING = 0x20, I_START = 0x24, I_FIFO = 0x30, I_FCLR = 0x38, I_HS = 0x48 };
#define KT_ADDR 0x36

static int i2c_clock(int on) { wr(sys, on ? 0x340 : 0x320, 1u << 19); msleep(1); return !(rd(sys, 0x300) >> 19 & 1) == !!on; }

static int i2c_wait(void)
{
    long end = now_ms() + 200;
    while(now_ms() < end) {
        uint32_t s = rd(i2c, I_STAT) & 0xffff;
        if(s & 6) { wr(i2c, I_STAT, 15); return -2; }       /* sem ACK */
        if(s & 1) { wr(i2c, I_STAT, 15); return 0; }
        usleep(200);
    }
    return -1;
}

static void i2c_setup(int trlen, uint32_t tlen, uint32_t ctrl)
{
    wr(i2c, I_STAT, 15);
    wr(i2c, I_MASK, 15);
    wr(i2c, I_FCLR, 1);
    wr(i2c, I_SLAVE, KT_ADDR << 1);
    wr(i2c, I_TRLEN, (uint32_t)trlen);
    wr(i2c, I_TLEN, tlen);
    wr(i2c, I_TIMING, 0x73F);                               /* o que o CE calcula (e deixa) */
    wr(i2c, I_HS, rd(i2c, I_HS) & ~1u);                      /* velocidade normal */
    wr(i2c, I_CTRL, ctrl);
}

static int kt_write(int reg, int val)
{
    i2c_setup(1, 2, 0x30);
    wr(i2c, I_DATA, (uint32_t)reg);
    wr(i2c, I_DATA, (uint32_t)val);
    wr(i2c, I_START, 1);
    return i2c_wait();
}

static int kt_read(int reg)
{
    i2c_setup(2, 1 | 1 << 8, 0x32);                          /* escreve o registrador, lê 1 byte */
    wr(i2c, I_DATA, (uint32_t)reg);
    wr(i2c, I_START, 1);
    int r = i2c_wait();
    if(r) return r;
    if(!((rd(i2c, I_FIFO) >> 4) & 15)) return -3;
    return (int)(rd(i2c, I_DATA) & 0xff);
}

/* ------------------------------------------------------------ PWM3 = 32 kHz do KT0801A */

static void pwm32k(int on)
{
    if(on) {
        wr(sys, 0x700, rd(sys, 0x700) | 1u << 10);
        wr(gpio, 0x600, rd(gpio, 0x600) & ~(1u << 23));
        wr(pwm, 0x90, (rd(pwm, 0x90) | 0x8000) & ~8u);       /* modo antigo, clock do bloco, div 1 */
        wr(pwm, 0xBC, 1786 - 1);
        wr(pwm, 0xC0, 1786 / 2 - 1);
        wr(pwm, 0xB8, 0);
        wr(pwm, 0x00, rd(pwm, 0x00) | 4);
    } else wr(pwm, 0x00, rd(pwm, 0x00) & ~4u);
}

static void power(int on)
{
    wr(gpio, 0x000, rd(gpio, 0x000) | 1u << 4);              /* GPIO4 saída, sem pull (CE) */
    wr(gpio, 0x100, rd(gpio, 0x100) & ~(1u << 4));
    wr(gpio, 0x300, on ? rd(gpio, 0x300) | 1u << 4 : rd(gpio, 0x300) & ~(1u << 4));
}

static int is_on(void) { return rd(gpio, 0x300) >> 4 & 1 && rd(pwm, 0x00) >> 2 & 1; }

/* ------------------------------------------------------------ frequência */

static int set_freq(int f10)                                  /* f10 = MHz x 10 */
{
    int ch = f10 * 2;                                         /* CHSEL em 50 kHz */
    int r1 = kt_read(1), r2 = kt_read(2);
    if(r1 < 0 || r2 < 0) return -1;
    if(kt_write(0, (ch >> 1) & 0xff)) return -1;
    (void)r1;                                                 /* RFGAIN e PGA do CE (0xDB) */
    if(kt_write(1, 0xD8 | ((ch >> 9) & 7))) return -1;
    if(kt_write(2, (r2 & 0x7f) | ((ch & 1) << 7))) return -1;
    return 0;
}

static int get_freq(void)
{
    int a = kt_read(0), b = kt_read(1), c = kt_read(2);
    if(a < 0 || b < 0 || c < 0) return -1;
    int ch = (b & 7) << 9 | a << 1 | (c >> 7 & 1);
    return ch / 2;
}

/* ------------------------------------------------------------ alto-falante (ALSA) */

static int speaker(int on)
{
    int fd = open("/dev/snd/controlC0", O_RDWR | O_CLOEXEC);
    if(fd < 0) return -1;
    struct snd_ctl_elem_value v;
    memset(&v, 0, sizeof v);
    v.id.iface = SNDRV_CTL_ELEM_IFACE_MIXER;
    snprintf((char *)v.id.name, sizeof v.id.name, "Speaker Playback Switch");
    v.value.integer.value[0] = on;
    int r = ioctl(fd, SNDRV_CTL_IOCTL_ELEM_WRITE, &v);
    close(fd);
    return r < 0 ? -1 : 0;
}

/* estado anterior (para o modo de saída e o "desde") */
static int st_mute, st_since;
static void read_prev(void)
{
    FILE *f = fopen(STATE, "r");
    if(!f) return;
    char l[128];
    while(fgets(l, sizeof l, f)) {
        if(!strncmp(l, "speaker_off=", 12)) st_mute = atoi(l + 12);
        else if(!strncmp(l, "since=", 6)) st_since = atoi(l + 6);
    }
    fclose(f);
}

static void write_state(const char *st, int f10, const char *err)
{
    FILE *f = fopen(STATE ".tmp", "w");
    if(!f) return;
    fprintf(f, "state=%s\nfreq=%d.%d\nchip=KT0801A\nspeaker_off=%d\n", st, f10 / 10, f10 % 10, st_mute);
    if(err && *err) fprintf(f, "error=%s\n", err);
    fprintf(f, "since=%ld\n", !strcmp(st, "transmitindo") && st_since ? (long)st_since : (long)time(NULL));
    fclose(f);
    rename(STATE ".tmp", STATE);
}

/* ------------------------------------------------------------ tom de teste */

static volatile sig_atomic_t stop_tone;
static void on_term(int s) { (void)s; stop_tone = 1; }

static int tone(int secs)
{
    if(secs < 1) secs = 1;
    if(secs > 30) secs = 30;
    int fd = -1;
    for(int i = 0; i < 20 && fd < 0; i++) {
        fd = open("/dev/dsp", O_WRONLY | O_CLOEXEC);
        if(fd < 0 && errno == EBUSY) msleep(100); else break;
    }
    if(fd < 0) { printf("erro dsp\n"); return 1; }
    int fmt = AFMT_S16_LE, ch = 1, sp = 32000;
    ioctl(fd, SNDCTL_DSP_SETFMT, &fmt);
    ioctl(fd, SNDCTL_DSP_CHANNELS, &ch);
    ioctl(fd, SNDCTL_DSP_SPEED, &sp);
    signal(SIGTERM, on_term);
    signal(SIGINT, on_term);
    /* onda triangular por tabela (sem ponto flutuante): 0,5 s de 440 Hz, 0,5 s de 880 Hz, repete */
    static int16_t tab[64];
    for(int i = 0; i < 64; i++) {                        /* -12 dBFS ~ 8200 */
        int x = i < 32 ? i : 64 - i;
        int y = (x * 4096 / 16) - 4096;                  /* -4096..4096 */
        tab[i] = (int16_t)(y * 2);
    }
    int16_t buf[1600];
    long total = (long)secs * 32000, n = 0;
    unsigned ph = 0;
    while(n < total && !stop_tone) {
        int hz = (n / 16000) % 2 ? 880 : 440;
        unsigned step = (unsigned)hz * 64 * 65536u / 32000;
        for(int i = 0; i < 1600; i++) { buf[i] = tab[(ph >> 16) & 63]; ph += step; }
        if(write(fd, buf, sizeof buf) < 0 && errno != EINTR) break;
        n += 1600;
    }
    close(fd);
    printf("ok\n");
    return 0;
}

static int parse_f10(const char *s)
{
    double v = atof(s);
    int f = (int)(v * 10 + 0.5);
    if(f < FMIN || f > FMAX) { fprintf(stderr, "frequência fora de 88,0-108,0 MHz\n"); return -1; }
    return f;
}

static int cmd_on(int f10, int mute)
{
    st_since = 0;
    write_state("ligando", f10, NULL);
    if(!i2c_clock(1)) { power(0); write_state("erro", f10, "clock do I2C"); printf("erro clock\n"); return 1; }
    pwm32k(1);
    power(1);
    int ok = 0;
    for(int i = 0; i < 3 && !ok; i++) {                       /* CE: 500 ms e PW_OK, até 3 vezes */
        msleep(500);
        int s = kt_read(0x0F);
        if(s >= 0 && (s & 0x10)) ok = 1;
        else if(s < 0) fprintf(stderr, "reg 0x0F: erro %d\n", s);
    }
    if(!ok) {
        power(0); pwm32k(0); i2c_clock(0);
        write_state("erro", f10, "chip sem PW_OK");
        printf("erro chip\n");
        return 1;
    }
    if(set_freq(f10) || get_freq() != f10) {
        power(0); pwm32k(0); i2c_clock(0);
        write_state("erro", f10, "frequência não confirmada");
        printf("erro frequencia\n");
        return 1;
    }
    st_mute = mute && speaker(0) == 0;                     /* "só no rádio": sem alto-falante */
    write_state("transmitindo", f10, NULL);
    printf("ok %d.%d\n", f10 / 10, f10 % 10);
    return 0;
}

static int cmd_off(void)
{
    power(0);
    pwm32k(0);
    i2c_clock(0);
    speaker(1);                                             /* o alto-falante volta sempre */
    st_mute = 0;
    write_state("desligado", 0, NULL);
    printf("ok\n");
    return 0;
}

int main(int argc, char **argv)
{
    if(argc < 2) {
        fprintf(stderr, "uso: bffm status | on [MHZ] | freq MHZ | off | dump\n");
        return 2;
    }
    if(map()) return 1;
    int lk = open(LOCK, O_RDWR | O_CREAT | O_CLOEXEC, 0600);   /* um comando por vez */
    if(lk >= 0) flock(lk, LOCK_EX);
    const char *c = argv[1];
    if(!strcmp(c, "status")) {
        int on = is_on();
        printf("gpio4=%u pwm3=%u i2c_clock=%u pwm_width=%u pwm_thresh=%u\n", rd(gpio, 0x300) >> 4 & 1,
               rd(pwm, 0) >> 2 & 1, !(rd(sys, 0x300) >> 19 & 1), rd(pwm, 0xBC), rd(pwm, 0xC0));
        if(on && !(rd(sys, 0x300) >> 19 & 1)) {
            int f = get_freq(), s = kt_read(0x0F);
            printf("freq=%d.%d pw_ok=%d\n", f / 10, f % 10, s >= 0 ? (s >> 4 & 1) : -1);
        }
        return 0;
    }
    read_prev();
    if(!strcmp(c, "on")) {
        int f = parse_f10(argc > 2 ? argv[2] : "88.1");
        return f < 0 ? 2 : cmd_on(f, !(argc > 3 && !strcmp(argv[3], "ambos")));
    }
    if(!strcmp(c, "saida") && argc > 2) {
        if(!is_on()) { printf("erro desligado\n"); return 1; }
        int mute = strcmp(argv[2], "ambos") != 0;
        if(speaker(!mute)) { printf("erro alsa\n"); return 1; }
        st_mute = mute;
        int f = get_freq();
        write_state("transmitindo", f, NULL);
        printf("ok\n");
        return 0;
    }
    if(!strcmp(c, "tom")) { flock(lk, LOCK_UN); return tone(argc > 2 ? atoi(argv[2]) : 8); }
    if(!strcmp(c, "freq") && argc > 2) {
        int f = parse_f10(argv[2]);
        if(f < 0) return 2;
        if(!is_on()) { printf("erro desligado\n"); return 1; }
        if(set_freq(f) || get_freq() != f) { printf("erro frequencia\n"); return 1; }
        write_state("transmitindo", f, NULL);
        printf("ok %d.%d\n", f / 10, f % 10);
        return 0;
    }
    if(!strcmp(c, "off")) return cmd_off();
    if(!strcmp(c, "dump")) {
        if(!is_on()) { printf("erro desligado\n"); return 1; }
        for(int r = 0; r < 0x20; r++) {
            int v = kt_read(r);
            printf("%02x=%s%02x%s", r, v < 0 ? "-" : "", v < 0 ? -v : v, r % 8 == 7 ? "\n" : " ");
        }
        return 0;
    }
    fprintf(stderr, "comando desconhecido\n");
    return 2;
}
