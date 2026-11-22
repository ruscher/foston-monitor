/* bfvoice — voz da ligação Bluetooth no FOSTON pelo DSP do MT3351 (docs/phone/02).
 *
 * O MT6611 entrega a voz (SCO) pelos pinos PCM ao DSP do MT3351; o DSP roda um programa que
 * o driver de áudio do Windows CE (wavedev_drv.dll) copia da sua própria seção de código.
 * O bfvoice repete, registrador por registrador, o que o CE faz (análise estática em
 * docs/phone/02-audio-da-chamada.md):
 *
 *   bfvoice status           mostra o DSP, a caixa postal e o front-end (só lê)
 *   bfvoice boot [TEXT.bin]   copia o programa do DSP e faz o aperto de mão (CE 0x3C181E4);
 *                            só é preciso se a caixa postal não estiver em 0x5555 (o CE já
 *                            inicia o DSP no boot e o Linux o encontra pronto)
 *   bfvoice start [TEXT.bin] voz da ligação: alto-falante e microfone (CE MM_GSMPCM_2WAY_START)
 *   bfvoice stop             volta ao normal (CE MM_GSMPCM_2WAY_CLOSE)
 *   bfvoice run [TEXT.bin]   start, atende o DSP até SIGTERM/SIGINT e faz o stop (é o que o
 *                            bfphone usa: no modo 2 vias o ARM leva cada bloco de 20 ms de uma
 *                            área da memória do DSP para outra — CE IST 0x3C0CEA4/0x3C1997C)
 *
 * TEXT.bin (padrão /mnt/sd/BigFoston/voz/wavedev-text.bin): a seção de código do wavedev_drv.dll do próprio aparelho (VA 0x03BD1000), tirada
 * do backup pelo tools/bigfoston/voice-extract.py. É da MediaTek: fica só no cartão do usuário.
 *
 * Só toca em registradores que o driver do CE comprovadamente acessa, e toda espera pelo DSP
 * tem tempo-limite (no CE algumas são infinitas).
 * SPDX-License-Identifier: GPL-3.0-or-later */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define TEXT_VA   0x03BD1000u
#define TEXT_SIZE 313466u
#define TEXT_PATH "/mnt/sd/BigFoston/voz/wavedev-text.bin"

/* ---------------------------------------------------------------- janelas */

typedef struct { uint32_t phys, size; volatile uint8_t *v; } win_t;
enum { W_PDN, W_DSP, W_MBOX, W_PM, W_XM, W_YM, W_AFE, W_ANA, W_ADMA, W_GPIO, NWIN };
static win_t win[NWIN] = {
    [W_PDN]  = { 0x80001000, 0x1000 },   /* clocks: 0x300 estado, +0x24 desliga, +0x44 liga (banco 1) */
    [W_DSP]  = { 0x8004F000, 0x1000 },   /* controle do DSP (o CE mapeia 0x50) */
    [W_MBOX] = { 0xA0000000, 0x1000 },   /* caixa postal (o CE mapeia 0x1A0) */
    [W_PM]   = { 0xA2000000, 0x20000 },  /* programa do DSP, bancos 0 e 1 (32 bits) */
    [W_XM]   = { 0xA2100000, 0x8000 },   /* dados X (16 bits) */
    [W_YM]   = { 0xA2200000, 0x8000 },   /* dados Y (16 bits) */
    [W_AFE]  = { 0x8004D000, 0x1000 },   /* front-end de voz (o CE mapeia 0x15C) */
    [W_ANA]  = { 0x80061000, 0x1000 },   /* analógico */
    [W_ADMA] = { 0x8004B000, 0x1000 },   /* DMA de áudio */
    [W_GPIO] = { 0x80002000, 0x1000 },   /* GPIO 5 = amplificador */
};

static int map_all(void)
{
    int fd = open("/dev/mem", O_RDWR | O_SYNC | O_CLOEXEC);
    if(fd < 0) { perror("/dev/mem"); return -1; }
    for(int i = 0; i < NWIN; i++) {
        void *p = mmap(NULL, win[i].size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, win[i].phys);
        if(p == MAP_FAILED) { fprintf(stderr, "mmap 0x%08x: %s\n", win[i].phys, strerror(errno)); return -1; }
        win[i].v = p;
    }
    close(fd);
    return 0;
}

static uint32_t r32(int w, uint32_t o) { return *(volatile uint32_t *)(win[w].v + o); }
static void w32(int w, uint32_t o, uint32_t v) { *(volatile uint32_t *)(win[w].v + o) = v; }
static uint16_t r16(int w, uint32_t o) { return *(volatile uint16_t *)(win[w].v + o); }
static void w16(int w, uint32_t o, uint16_t v) { *(volatile uint16_t *)(win[w].v + o) = v; }
/* RMW de 32 bits como os objetos do CE: (velho & ~máscara) | valor */
static void m32(int w, uint32_t o, uint32_t mask, uint32_t v) { w32(w, o, (r32(w, o) & ~mask) | v); }
static void lo16(int w, uint32_t o, uint16_t v) { m32(w, o, 0xffff, v); }
static void m16(int w, uint32_t o, uint16_t mask, uint16_t v) { w16(w, o, (uint16_t)((r16(w, o) & ~mask) | v)); }

static void stall_us(long us) { struct timespec t = { 0, us * 1000 }; nanosleep(&t, NULL); }

static long now_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

/* espera a palavra da caixa postal valer v (o CE espera para sempre; aqui até ms) */
static int mbox_wait(uint32_t off, uint16_t v, int ms)
{
    long end = now_ms() + ms;
    for(;;) {
        uint16_t got = r16(W_MBOX, off);
        if(got == v) return 0;
        if(now_ms() > end) { fprintf(stderr, "DSP: caixa +0x%02x = 0x%04x, esperado 0x%04x\n", off, got, v); return -1; }
        stall_us(100);
    }
}

/* ------------------------------------------------------------- clocks */

#define PDN1_STA 0x304
#define PDN1_SET 0x324          /* desliga */
#define PDN1_CLR 0x344          /* liga */
/* ids 37 e 43 conferidos no aparelho (docs/audio.md); 42 pela mesma regra (não conferido).
 * O id 45 (DSP) NÃO é o bit 21: com o DSP iniciado pelo CE o bit 21 continua 1 — o bfvoice
 * não mexe no clock do DSP. */
#define CLK(id) (1u << ((id) - 24))

static void clk_on(uint32_t bits) { w32(W_PDN, PDN1_CLR, bits); }
static void clk_off(uint32_t bits) { w32(W_PDN, PDN1_SET, bits); }

/* -------------------------------------------------------------- imagem */

static uint8_t *text;

static const uint8_t *at(uint32_t va, uint32_t n)
{
    if(va < TEXT_VA || va + n > TEXT_VA + TEXT_SIZE) { fprintf(stderr, "VA 0x%08x fora da imagem\n", va); exit(1); }
    return text + (va - TEXT_VA);
}

static int load_text(const char *path)
{
    FILE *f = fopen(path, "rb");
    if(!f) { perror(path); return -1; }
    text = malloc(TEXT_SIZE);
    size_t n = text ? fread(text, 1, TEXT_SIZE, f) : 0;
    int extra = fgetc(f);
    fclose(f);
    if(n != TEXT_SIZE || extra != EOF) { fprintf(stderr, "%s: tamanho errado (esperado %u)\n", path, TEXT_SIZE); return -1; }
    /* conferência mínima: a primeira palavra dos dois bancos do programa */
    uint32_t a, b;
    memcpy(&a, at(0x03BF3560, 4), 4);
    memcpy(&b, at(0x03BE3560, 4), 4);
    if(a != 0x00545715 || b != 0x00545715) { fprintf(stderr, "%s: não é a seção do wavedev_drv analisado\n", path); return -1; }
    return 0;
}

/* cópia de halfwords da imagem para uma memória de 16 bits do DSP */
static void put16(int w, uint32_t off, uint32_t va, uint32_t bytes)
{
    const uint8_t *s = at(va, bytes);
    for(uint32_t i = 0; i < bytes; i += 2) w16(w, off + i, (uint16_t)(s[i] | s[i + 1] << 8));
}

/* -------------------------------------------------------------- etapas */

static void show(void)
{
    uint32_t pdn = r32(W_PDN, PDN1_STA);
    printf("PDN1 0x%08x: voz(42) %s, áudio(37/43) %s\n", pdn, pdn & CLK(42) ? "desl" : "lig",
           pdn & (CLK(37) | CLK(43)) ? "desl" : "lig");
    printf("DSP ctl: +00 %08x +04 %08x +0c %08x +10 %08x +28 %08x +48 %08x\n", r32(W_DSP, 0), r32(W_DSP, 4),
           r32(W_DSP, 0xc), r32(W_DSP, 0x10), r32(W_DSP, 0x28), r32(W_DSP, 0x48));
    printf("caixa: +00 %04x +02 %04x +22 %04x +50 %04x +94 %04x +a6 %04x +aa %04x +e0 %04x +f6 %04x\n",
           r16(W_MBOX, 0), r16(W_MBOX, 2), r16(W_MBOX, 0x22), r16(W_MBOX, 0x50), r16(W_MBOX, 0x94),
           r16(W_MBOX, 0xa6), r16(W_MBOX, 0xaa), r16(W_MBOX, 0xe0), r16(W_MBOX, 0xf6));
    printf("AFE: +000 %04x +00c %04x +014 %04x +018 %04x +100 %04x +104 %04x\n", r32(W_AFE, 0) & 0xffff,
               r32(W_AFE, 0xc) & 0xffff, r32(W_AFE, 0x14) & 0xffff, r32(W_AFE, 0x18) & 0xffff,
               r32(W_AFE, 0x100) & 0xffff, r32(W_AFE, 0x104) & 0xffff);
}

/* CE 0x3C17384 + 0x3C181E4 + 0x3C19B04 */
static int boot(void)
{
    w32(W_DSP, 0x28, 1);
    w32(W_DSP, 0x48, 0xBFFF);
    w32(W_DSP, 0, 0xC); w32(W_DSP, 0, 0xD); w32(W_DSP, 0, 0xE); w32(W_DSP, 0, 0xF);
    /* programa: bancos 0 e 1 a partir da palavra 1; a palavra 0 por último */
    static const uint32_t pm_src[2] = { 0x03BF3560, 0x03BE3560 };
    for(int b = 0; b < 2; b++) {
        const uint8_t *s = at(pm_src[b], 0x10000);
        for(uint32_t i = 4; i < 0x10000; i += 4) {
            uint32_t v;
            memcpy(&v, s + i, 4);
            w32(W_PM, b * 0x10000 + i, v);
        }
    }
    put16(W_XM, 0, 0x03BDB560, 0x8000);
    put16(W_YM, 0, 0x03BD3560, 0x8000);
    /* conferência por leitura de volta (amostra) */
    for(int b = 0; b < 2; b++)
        for(uint32_t i = 4; i < 0x10000; i += 0x1004 & ~3u) {
            uint32_t v;
            memcpy(&v, at(pm_src[b], 0x10000) + i, 4);
            if((r32(W_PM, b * 0x10000 + i) & 0xffffff) != (v & 0xffffff)) {
                fprintf(stderr, "DSP: programa não confere no banco %d +0x%x (0x%08x != 0x%08x)\n", b, i,
                        r32(W_PM, b * 0x10000 + i), v);
                return -1;
            }
        }
    uint32_t w0;
    memcpy(&w0, at(pm_src[0], 4), 4); w32(W_PM, 0, w0);
    memcpy(&w0, at(pm_src[1], 4), 4); w32(W_PM, 0x10000, w0);
    w32(W_DSP, 0, 0xC); w32(W_DSP, 0, 0xD);
    /* aperto de mão (CE 0x3C17F18): 22 blocos de parâmetros vazios (tag 0x5A00) */
    if(mbox_wait(0x00, 0x5555, 2000)) return -1;
    w16(W_MBOX, 0x00, 0x6666);
    for(int i = 0; i < 14; i++) {
        w16(W_MBOX, 0x24 + 8 * i, 0x5A00); w16(W_MBOX, 0x26 + 8 * i, 0);
        w16(W_MBOX, 0x28 + 8 * i, 0); w16(W_MBOX, 0x2a + 8 * i, 0);
    }
    w16(W_MBOX, 0x02, 0x6666);
    if(mbox_wait(0x22, 0x1111, 2000)) return -1;
    for(int i = 0; i < 8; i++) {
        w16(W_MBOX, 0x24 + 8 * i, 0x5A00); w16(W_MBOX, 0x26 + 8 * i, 0);
        w16(W_MBOX, 0x28 + 8 * i, 0); w16(W_MBOX, 0x2a + 8 * i, 0);
    }
    w16(W_MBOX, 0x22, 0x2222);
    if(mbox_wait(0x00, 0x5555, 2000)) return -1;
    /* tabelas fixas da voz (CE 0x3C19B04) */
    put16(W_XM, 0x7EEE, 0x03C03968, 0x5A);
    put16(W_YM, 0x6A80, 0x03C039C8, 0xE4);
    put16(W_XM, 0x7F48, 0x03C038C8, 0x3E);
    put16(W_YM, 0x643A, 0x03C03850, 0x28);
    printf("DSP iniciado\n");
    return 0;
}

/* parâmetros da voz (registro WavformDriver do aparelho; docs/phone/02) */
static const uint16_t param_block[20] = {
    0x0000, 0xDAB0, 0x7918, 0x4000, 0x0000, 0x0010, 0x7FFF, 0x0000,      /* Common_8 */
    0x0000, 0x0000, 0x0000, 0x0000,                                      /* Common_4 */
    0x0060, 0x00E0, 0x1844, 0x081E, 0xC007, 0x001F, 0x0190, 0x00AC,      /* LoudSp_Mode */
};
#define MIC_VOL 0x98            /* Microphone_Volume */
#define SPK_GAIN 0x77           /* Speech_Volume 0x95 -> T[0xC0] = 0x70 -> 0x77 */

static uint16_t saved_1204, saved_gain;

static void guard_snapshot(void);

/* CE MM_GSMPCM_2WAY_START (0x3C0833C, 0x3C0C5A8, 0x3C1A100) */
static int start(void)
{
    if(r16(W_MBOX, 0) != 0x5555) {
        fprintf(stderr, "DSP não iniciado (bfvoice boot)\n");
        return -1;
    }
    clk_on(CLK(42) | CLK(43) | CLK(37));
    stall_us(100);
    saved_gain = r16(W_ANA, 0x200);
    /* 0x3C0E1F4, 0x3C0E0A0 */
    m16(W_ADMA, 0x204, 0x000C, 0x000C);
    m16(W_ANA, 0x200, 0x0300, 0x0300);
    w16(W_ADMA, 0x200, 0x0001);
    /* 0x3C18878: parâmetros */
    w16(W_MBOX, 0xF4, 1);
    w32(W_DSP, 0x0C, (r32(W_DSP, 0x0C) & 0xFFFF) | 0xFF);
    for(int i = 0; i < 20; i++) w16(W_YM, 0x7F2C + 2 * i, param_block[i]);
    w16(W_XM, 0x7E94, 0x7FFF);
    for(int i = 1; i < 45; i++) w16(W_XM, 0x7E94 + 2 * i, 0);
    w16(W_MBOX, 0x108, 0x0400); w16(W_MBOX, 0x10A, 0xFFF7);
    w16(W_MBOX, 0x22, 0x0001); w16(W_MBOX, 0x24, 0x000A); w16(W_MBOX, 0x26, 0x00D0); w16(W_MBOX, 0x28, 0x0038);
    w16(W_MBOX, 0x30, 0x0E3D); w16(W_MBOX, 0x32, 0xC053); w16(W_MBOX, 0x34, 0x002D); w16(W_MBOX, 0x38, 0x002D);
    w16(W_MBOX, 0x3A, 0x1000); w16(W_MBOX, 0x3C, 0x0000);
    put16(W_XM, 0x7D68, 0x03C03AB0, 0xF0);
    /* 0x3C18DE0: front-end de voz e microfone */
    lo16(W_AFE, 0x018, 0x0000);
    lo16(W_AFE, 0x00C, 0x0098);
    w16(W_MBOX, 0x106, 0x002B);
    lo16(W_AFE, 0x108, 0xA4B5); lo16(W_AFE, 0x10C, 0x2DD5); lo16(W_AFE, 0x110, 0x1018);
    lo16(W_AFE, 0x114, 0x3478); lo16(W_AFE, 0x118, 0x40FD);
    lo16(W_AFE, 0x104, 0x2CC8);
    lo16(W_AFE, 0x100, (MIC_VOL >> 2) << 4 | 6);
    lo16(W_AFE, 0x100, (uint16_t)(r32(W_AFE, 0x100) | 1));
    lo16(W_ANA, 0x114, 0x0000);
    /* 0x3C18A34(2, 3): modo 2 vias */
    w16(W_MBOX, 0xA6, 2);
    w16(W_MBOX, 0x50, 4);
    if(mbox_wait(0xAA, 0, 500)) return -1;
    w16(W_MBOX, 0xAA, 1);
    w16(W_MBOX, 0xF6, 1);
    w16(W_MBOX, 0xE0, 1);
    /* 0x3C190E0: liga o front-end */
    lo16(W_AFE, 0x014, 0x0018);
    lo16(W_AFE, 0x000, 0);
    lo16(W_AFE, 0x00C, (uint16_t)(r32(W_AFE, 0x00C) | 0x0201));
    lo16(W_AFE, 0x000, 1);
    stall_us(250);
    /* 0x3C19334 __speech_AnaOn */
    saved_1204 = (uint16_t)r32(W_ANA, 0x204);
    lo16(W_ANA, 0x114, 0); lo16(W_ANA, 0x104, 0); lo16(W_ANA, 0x108, 0); lo16(W_ANA, 0x10C, 0);
    lo16(W_ANA, 0x114, 0x8000);
    lo16(W_ANA, 0x104, (MIC_VOL & 0xFC) << 6);
    m32(W_ANA, 0x200, 0xFF, SPK_GAIN);
    lo16(W_ANA, 0x108, 0x0080);
    lo16(W_ANA, 0x10C, 0x8000);
    lo16(W_ANA, 0x204, 0x1240);
    lo16(W_ANA, 0x110, 0x003C);
    stall_us(250);
    m32(W_ANA, 0x208, 0xFF, 0x1F);
    stall_us(250);
    lo16(W_ANA, 0x10C, 0);
    lo16(W_ANA, 0x204, 0x1240);
    /* microfone: com 0x80061108 = 0x80 (valor do modo de ligação do CE) o ADC do microfone fica
     * desligado e o DSP recebe zeros. Os valores da gravação de fábrica do CE (0x3C1A980)
     * ligam o microfone — medido numa ligação real em 22/11/2026: o nível vai de ~60 a ~31000
     * e a outra pessoa passa a ouvir. */
    lo16(W_ANA, 0x104, 0x2A00);
    lo16(W_ANA, 0x10C, 0x6220);
    lo16(W_ANA, 0x108, 0x0000);
    /* liga a voz */
    w16(W_MBOX, 0x94, 0x0110);
    w16(W_YM, 0x7F2C, 0);
    w32(W_DSP, 0x04, 0x838);
    /* amplificador do alto-falante (GPIO 5) */
    m32(W_GPIO, 0x300, 1u << 5, 1u << 5);
    guard_snapshot();
    /* para o stop: ganho e caminho de antes */
    FILE *f = fopen("/run/bfvoice.saved", "w");
    if(f) { fprintf(f, "%u %u\n", saved_gain, saved_1204); fclose(f); }
    printf("voz ligada\n");
    return 0;
}

/* CE MM_GSMPCM_2WAY_CLOSE (0x3C1A1BC, 0x3C19530, 0x3C18F7C, 0x3C0E24C, 0x3C0E0FC) */
static int stop(void)
{
    unsigned g = 0x00, p = 0x1120;
    FILE *f = fopen("/run/bfvoice.saved", "r");
    if(f) { if(fscanf(f, "%u %u", &g, &p) != 2) { g = 0; p = 0x1120; } fclose(f); unlink("/run/bfvoice.saved"); }
    int rc = 0;
    if(r16(W_MBOX, 0) == 0x5555) {
        w16(W_MBOX, 0x94, 0);
        w16(W_YM, 0x7F2C, param_block[0]);
        static const uint16_t offs[3] = { 0xAA, 0xF6, 0xE0 };
        for(int i = 0; i < 3; i++) {
            if(mbox_wait(offs[i], 2, 500)) { rc = -1; continue; }
            w16(W_MBOX, offs[i], 3);
            if(mbox_wait(offs[i], 0, 500)) rc = -1;
        }
        w32(W_DSP, 0x04, 0x839);
    }
    m32(W_GPIO, 0x300, 1u << 5, 0);
    /* __speech_AnaOff */
    lo16(W_ANA, 0x10C, 0x8000);
    lo16(W_ANA, 0x204, 0x1120);
    stall_us(250);
    lo16(W_ANA, 0x110, 0x000C);
    lo16(W_ANA, 0x208, 0x001F);
    stall_us(250);
    lo16(W_ANA, 0x110, 0);
    m32(W_ANA, 0x200, 0xFF, g & 0xFF);
    lo16(W_ANA, 0x114, 0); lo16(W_ANA, 0x104, 0); lo16(W_ANA, 0x108, 0); lo16(W_ANA, 0x10C, 0);
    /* 0x3C18F7C: front-end parado */
    lo16(W_AFE, 0x018, 0x0000);
    lo16(W_AFE, 0x00C, 0x0098);
    lo16(W_AFE, 0x104, 0x0CC8);
    lo16(W_AFE, 0x100, (MIC_VOL >> 2) << 4 | 6);
    lo16(W_ANA, 0x114, 0x8000);
    /* 0x3C0E24C, 0x3C0E0FC */
    m16(W_ANA, 0x200, 0x0300, 0);
    m16(W_ADMA, 0x204, 0x000C, 0);
    w16(W_ADMA, 0x200, 0x0001);
    (void)p;
    clk_off(CLK(42) | CLK(43) | CLK(37));
    printf(rc ? "voz desligada (o DSP não respondeu a tudo)\n" : "voz desligada\n");
    return rc;
}

/* Pedido do DSP (CE IST, bit 0x20 de +0x10): caixa +0x0A == 0x14. O ARM lê o bloco de
 * Y[+0x9A*2] (tamanho +0x9C*2), grava em Y[+0x9A*2 + 0x280] (tamanho +0x9E*2) e responde
 * +0x0A = 0. */
static volatile sig_atomic_t quit;
static void on_sig(int s) { (void)s; quit = 1; }
static unsigned long served, bad;

/* A voz do celular (PCM do MT6611 pelo DAI) chega ao DSP no anel Y+0x7400 (2 x 160 amostras,
 * medido em 22/11/2026: só muda com o canal de voz Bluetooth aberto). O bloco que o CE copia
 * (Y[+0x9A*2]) fica em ruído neste modo, então a ponte leva o anel ao lado do alto-falante,
 * alternando as metades — a versão aprovada pelo usuário numa ligação real. */
#define BT_RX_RING 0x7400
/* ganho da voz do celular em %: /data/bigfoston/voz-ganho (padrão 170; o usuário achou 100 baixo
 * e 130 ainda baixo com o volume no máximo) */
#define RX_GAIN_FILE "/data/bigfoston/voz-ganho"
static int rx_gain = 250;               /* aprovado pelo usuário numa ligação real */

/* Controle durante a ligação: /run/bfvoice.ctl ("mute=0|1 gain=N rec=CAMINHO|-"), escrito
 * pelo bfphone e relido a cada 200 ms. */
#define CTL_FILE "/run/bfvoice.ctl"
static int muted;
static FILE *rec;
static char rec_path[200];
static uint32_t rec_bytes;

static void wav_header(FILE *f, uint32_t data)
{
    uint8_t h[44] = { 'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'A', 'V', 'E', 'f', 'm', 't', ' ', 16, 0, 0, 0, 1, 0, 1, 0,
                      0x40, 0x1f, 0, 0, 0x80, 0x3e, 0, 0, 2, 0, 16, 0, 'd', 'a', 't', 'a', 0, 0, 0, 0 };
    uint32_t riff = data + 36;
    memcpy(h + 4, &riff, 4);
    memcpy(h + 40, &data, 4);
    fseek(f, 0, SEEK_SET);
    fwrite(h, 1, sizeof h, f);
    fseek(f, 0, SEEK_END);
}

static void rec_stop(void)
{
    if(!rec) return;
    wav_header(rec, rec_bytes);
    fclose(rec);
    rec = NULL;
    sync();
}

static void set_mute(int m)
{
    if(m == muted) return;
    muted = m;
    lo16(W_ANA, 0x108, m ? 0x0080 : 0x0000);     /* 0x80 desliga o ADC do microfone (medido) */
}

/* O driver ALSA divide clocks, amplificador e analógico com a voz: ao fechar o som (por exemplo
 * a música pausada pela ligação) ele desliga os clocks 37/43 e o amplificador, e a ligação fica
 * muda. Durante a ligação o bfvoice confere e restaura (a cada 200 ms). */
static unsigned long fixes;
static char fixed_what[160];

/* "foto" dos registradores de áudio tirada no fim do start; guard() restaura o que mudar
 * (o ALSA, ao abrir/fechar o som, mexe em quase todos: medido ao juntar duas chamadas) */
static const struct { int w; uint32_t o; uint32_t mask; } watch[] = {
    { W_ANA, 0x104, 0xffff }, { W_ANA, 0x10C, 0xffff }, { W_ANA, 0x110, 0xffff }, { W_ANA, 0x114, 0xffff },
    { W_ANA, 0x200, 0x03ff }, { W_ANA, 0x204, 0xffff }, { W_ANA, 0x208, 0x00ff }, { W_ANA, 0x20C, 0xffff },
    { W_ADMA, 0x200, 0xffff }, { W_ADMA, 0x204, 0x000C },
    { W_AFE, 0x000, 0xffff }, { W_AFE, 0x00C, 0xffff }, { W_AFE, 0x014, 0xffff }, { W_AFE, 0x018, 0xffff },
    { W_AFE, 0x100, 0xffff }, { W_AFE, 0x104, 0xffff },
};
#define NWATCH (int)(sizeof watch / sizeof watch[0])
static uint32_t snap[NWATCH];

static void guard_snapshot(void)
{
    for(int i = 0; i < NWATCH; i++) snap[i] = r32(watch[i].w, watch[i].o) & watch[i].mask;
}

static void note_fix(const char *what)
{
    fixes++;
    if(!strstr(fixed_what, what) && strlen(fixed_what) + strlen(what) + 2 < sizeof fixed_what) {
        strcat(fixed_what, " ");
        strcat(fixed_what, what);
    }
}

static void guard(void)
{
    uint32_t need = CLK(42) | CLK(43) | CLK(37);
    if(r32(W_PDN, PDN1_STA) & need) { clk_on(need); note_fix("clocks"); }
    if((r32(W_ANA, 0x108) & 0xffff) != (muted ? 0x80u : 0u)) { lo16(W_ANA, 0x108, muted ? 0x80 : 0); note_fix("mic"); }
    if(!(r32(W_GPIO, 0x300) & (1u << 5))) { m32(W_GPIO, 0x300, 1u << 5, 1u << 5); note_fix("amp"); }
    for(int i = 0; i < NWATCH; i++) {
        uint32_t m = watch[i].mask, v = r32(watch[i].w, watch[i].o);
        if((v & m) != snap[i]) {
            w32(watch[i].w, watch[i].o, (v & ~m) | snap[i]);
            char n[16];
            snprintf(n, sizeof n, "%s%03X", watch[i].w == W_ANA ? "A" : watch[i].w == W_ADMA ? "D" : "F", watch[i].o);
            note_fix(n);
        }
    }
}

/* níveis (pico) da voz do celular e do microfone, para o log */
static int lvl_rx, lvl_ul;

static void ctl_read(void)
{
    FILE *f = fopen(CTL_FILE, "r");
    if(!f) return;
    char l[300] = "";
    if(!fgets(l, sizeof l, f)) l[0] = 0;
    fclose(f);
    int m = 0, g = rx_gain;
    char r[220] = "-";
    char *p;
    if((p = strstr(l, "mute="))) m = atoi(p + 5);
    if((p = strstr(l, "gain="))) { g = atoi(p + 5); if(g < 25 || g > 400) g = rx_gain; }
    if((p = strstr(l, "rec="))) { sscanf(p + 4, "%219s", r); }
    set_mute(m);
    rx_gain = g;
    if(r[0] == '-' && rec) rec_stop();
    else if(r[0] == '/' && (!rec || strcmp(r, rec_path))) {
        rec_stop();
        umask(077);
        rec = fopen(r, "wb");
        if(rec) { snprintf(rec_path, sizeof rec_path, "%s", r); rec_bytes = 0; wav_header(rec, 0); }
    }
}
static unsigned ring_n;

static void serve_once(void)
{
    if(r16(W_MBOX, 0x0A) != 0x14) return;
    uint32_t off = (uint32_t)r16(W_MBOX, 0x9A) * 2, wr = (uint32_t)r16(W_MBOX, 0x9E) * 2;
    if(wr > 0x140 || off + 0x280 + wr > 0x8000) {
        bad++;
    } else {
        uint32_t src = BT_RX_RING + (ring_n++ & 1) * 0x140;
        for(uint32_t i = 0; i < wr; i += 16) {            /* amostra dos níveis */
            int a = abs((int16_t)r16(W_YM, src + i)), b = abs((int16_t)r16(W_YM, off + i));
            if(a > lvl_rx) lvl_rx = a;
            if(b > lvl_ul) lvl_ul = b;
        }
        if(rec) {                                 /* gravação: voz do celular + microfone, 8 kHz */
            int16_t mix[160];
            int n = (int)(wr / 2);
            for(int i = 0; i < n; i++) {
                int a = (int16_t)r16(W_YM, src + 2 * i), b = muted ? 0 : (int16_t)r16(W_YM, off + 2 * i);
                int v = (a + b) / 2;
                mix[i] = (int16_t)v;
            }
            if(fwrite(mix, 2, (size_t)n, rec) == (size_t)n) rec_bytes += (uint32_t)n * 2;
        }
        for(uint32_t i = 0; i < wr; i += 2) {
            int v = (int16_t)r16(W_YM, src + i) * rx_gain / 100;
            w16(W_YM, off + 0x280 + i, (uint16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v));
        }
        served++;
    }
    w16(W_MBOX, 0x0A, 0);
}

static int run(void)
{
    FILE *gf = fopen(RX_GAIN_FILE, "r");
    if(gf) { int g; if(fscanf(gf, "%d", &g) == 1 && g >= 25 && g <= 400) rx_gain = g; fclose(gf); }
    signal(SIGTERM, on_sig);
    signal(SIGINT, on_sig);
    signal(SIGHUP, on_sig);
    if(start()) { stop(); return 1; }
    long last = now_ms(), lastctl = 0;
    while(!quit) {
        serve_once();
        stall_us(1000);
        long t = now_ms();
        if(t - lastctl >= 200) { lastctl = t; ctl_read(); guard(); }
        if(t - last >= 10000) {
            last = t;
            fprintf(stderr, "bfvoice: %lu blocos, %lu inválidos, celular %d, microfone %d%s, ganho %d%%, restaurações %lu%s\n",
                    served, bad, lvl_rx, lvl_ul, muted ? " (mudo)" : "", rx_gain, fixes, fixed_what);
            lvl_rx = lvl_ul = 0;
            fixed_what[0] = 0;
        }
    }
    fprintf(stderr, "bfvoice: %lu blocos atendidos, %lu inválidos\n", served, bad);
    rec_stop();
    return stop() ? 1 : 0;
}

int main(int argc, char **argv)
{
    if(argc < 2) {
        fprintf(stderr, "uso: bfvoice status|boot [TEXT.bin]|start [TEXT.bin]|stop\n");
        return 2;
    }
    if(map_all()) return 1;
    const char *c = argv[1];
    if(!strcmp(c, "status")) { show(); return 0; }
    const char *img = argc > 2 ? argv[2] : TEXT_PATH;
    if(!strcmp(c, "boot")) {
        if(load_text(img)) return 1;
        int rc = boot();
        show();
        return rc ? 1 : 0;
    }
    if(!strcmp(c, "start")) {
        if(load_text(img)) return 1;
        if(start()) { stop(); return 1; }     /* falhou no meio: volta ao normal */
        return 0;
    }
    if(!strcmp(c, "stop")) return stop() ? 1 : 0;
    if(!strcmp(c, "run")) {
        if(load_text(img)) return 1;
        setvbuf(stderr, NULL, _IOLBF, 0);
        return run();
    }
    if(!strcmp(c, "phase")) {                     /* só lê: qual metade do anel muda logo após o pedido */
        int h0 = 0, h1 = 0, tries = 0;
        while(tries < 60) {
            if(r16(W_MBOX, 0x0A) != 0x14) { stall_us(200); continue; }
            uint16_t a0[8], a1[8];
            for(int i = 0; i < 8; i++) { a0[i] = r16(W_YM, 0x7400 + 0x40 * i); a1[i] = r16(W_YM, 0x7540 + 0x40 * i); }
            stall_us(8000);
            int c0 = 0, c1 = 0;
            for(int i = 0; i < 8; i++) { c0 += a0[i] != r16(W_YM, 0x7400 + 0x40 * i); c1 += a1[i] != r16(W_YM, 0x7540 + 0x40 * i); }
            if(c0 > c1) h0++; else if(c1 > c0) h1++;
            tries++;
            while(r16(W_MBOX, 0x0A) == 0x14) stall_us(200);
        }
        printf("depois do pedido muda: metade 0 em %d, metade 1 em %d de 60\n", h0, h1);
        return 0;
    }
    if(!strcmp(c, "bridge")) {                    /* teste: alto-falante <- buffer Y[SRC] (anel de 2 blocos) */
        uint32_t src = argc > 2 ? (uint32_t)strtoul(argv[2], NULL, 0) : 0x7400;
        int secs = argc > 3 ? atoi(argv[3]) : 10, swap = argc > 4 ? atoi(argv[4]) : 0, shift = argc > 5 ? atoi(argv[5]) : 0;
        unsigned phase = argc > 6 ? (unsigned)atoi(argv[6]) : 0;
        signal(SIGTERM, on_sig); signal(SIGINT, on_sig);
        long end = now_ms() + secs * 1000;
        unsigned n = 0;
        while(!quit && now_ms() < end) {
            if(r16(W_MBOX, 0x0A) == 0x14) {
                uint32_t off = (uint32_t)r16(W_MBOX, 0x9A) * 2, wr = (uint32_t)r16(W_MBOX, 0x9E) * 2;
                /* phase 0/1: metades alternadas; 2: amostras pares das 320; 3: ímpares; 4: só a metade 1 */
                uint32_t s0 = src + ((n++ + phase) & 1) * 0x140;
                for(uint32_t i = 0; i < wr && i < 0x140 && off + 0x280 + i < 0x8000 && s0 + i < 0x8000; i += 2) {
                    uint32_t at = phase == 2 ? src + i * 2 : phase == 3 ? src + i * 2 + 2 : phase == 4 ? src + 0x140 + i : s0 + i;
                    uint16_t v = r16(W_YM, at);
                    if(swap) v = (uint16_t)(v >> 8 | v << 8);
                    int x = (int16_t)v;
                    x = shift >= 0 ? x >> shift : x << -shift;
                    w16(W_YM, off + 0x280 + i, (uint16_t)x);
                }
                w16(W_MBOX, 0x0A, 0);
            }
            stall_us(1000);
        }
        printf("%u blocos\n", n);
        return 0;
    }
    if(!strcmp(c, "mrec")) {                      /* só lê: pico por segundo de cada pedaço de 32 amostras (X e Y) */
        int secs = argc > 2 ? atoi(argv[2]) : 60;
        enum { CH = 32, NC = 0x8000 / CH };
        static int pk[NC];
        setvbuf(stdout, NULL, _IOFBF, 1 << 16);
        for(int t = 0; t < secs; t++) {
            memset(pk, 0, sizeof pk);
            for(int k = 0; k < 20; k++) {
                for(int i = 0; i < 0x8000; i++) {
                    int v = (int16_t)(i < 0x4000 ? r16(W_XM, i * 2) : r16(W_YM, (i - 0x4000) * 2));
                    if(abs(v) > pk[i / CH]) pk[i / CH] = abs(v);
                }
                stall_us(50000);
            }
            printf("%d", t);
            for(int c = 0; c < NC; c++) printf(" %d", pk[c]);
            printf("\n");
            fflush(stdout);
        }
        return 0;
    }
    if(!strcmp(c, "scan")) {                      /* procura voz na memória de dados X e Y do DSP */
        int secs = argc > 2 ? atoi(argv[2]) : 10;
        enum { CH = 64, NX = 0x8000 / 2 / CH, NC = NX * 2 };   /* pedaços de 64 amostras, X e Y */
        static int16_t prev[0x8000];
        static long act[NC]; static int peak[NC];
        for(int i = 0; i < 0x4000; i++) { prev[i] = (int16_t)r16(W_XM, i * 2); prev[0x4000 + i] = (int16_t)r16(W_YM, i * 2); }
        for(int k = 0; k < secs * 10; k++) {
            stall_us(100000);
            for(int i = 0; i < 0x8000; i++) {
                int16_t v = (int16_t)(i < 0x4000 ? r16(W_XM, i * 2) : r16(W_YM, (i - 0x4000) * 2));
                int c = i / CH;
                if(v != prev[i]) { act[c]++; if(abs(v) > peak[c]) peak[c] = abs(v); }
                prev[i] = v;
            }
        }
        for(int c = 0; c < NC; c++)
            if(act[c] > secs * 10 && peak[c] > 300)
                printf("%s +0x%04x  mudancas %6ld  pico %5d\n", c < NX ? "X" : "Y", (c % NX) * CH * 2, act[c], peak[c]);
        printf("fim\n");
        return 0;
    }
    if(!strcmp(c, "peek")) {                      /* só lê: pico/média do bloco de origem, 10x por segundo */
        int secs = argc > 2 ? atoi(argv[2]) : 10;
        setvbuf(stdout, NULL, _IOLBF, 0);
        for(int k = 0; k < secs * 10; k++) {
            uint32_t off = (uint32_t)r16(W_MBOX, 0x9A) * 2;
            int mx = 0; long sum = 0; int zc = 0, prev = 0;
            for(uint32_t i = 0; i < 0x140 && off + i < 0x8000; i += 2) {
                int a = (int16_t)r16(W_YM, off + i);
                if(abs(a) > mx) mx = abs(a);
                sum += abs(a);
                if((a < 0) != (prev < 0)) zc++;
                prev = a;
            }
            printf("pico %5d media %5ld cruzamentos %3d\n", mx, sum / 160, zc);
            stall_us(100000);
        }
        return 0;
    }
    if(!strcmp(c, "tone")) {                      /* start + tom de 1 kHz no lado do alto-falante */
        if(load_text(img)) return 1;
        signal(SIGTERM, on_sig); signal(SIGINT, on_sig);
        if(start()) { stop(); return 1; }
        long end = now_ms() + (argc > 3 ? atol(argv[3]) : 6) * 1000;
        unsigned ph = 0;
        static const int16_t sine8[8] = { 0, 5792, 8191, 5792, 0, -5792, -8191, -5792 };   /* 1 kHz a 8 kHz */
        while(!quit && now_ms() < end) {
            if(r16(W_MBOX, 0x0A) == 0x14) {
                uint32_t off = (uint32_t)r16(W_MBOX, 0x9A) * 2, wr = (uint32_t)r16(W_MBOX, 0x9E) * 2;
                for(uint32_t i = 0; i < wr && off + 0x280 + i < 0x8000; i += 2)
                    w16(W_YM, off + 0x280 + i, (uint16_t)sine8[ph++ & 7]);
                w16(W_MBOX, 0x0A, 0);
                served++;
            }
            stall_us(1000);
        }
        printf("%lu blocos com tom\n", served);
        return stop() ? 1 : 0;
    }
    if(!strcmp(c, "meter")) {                     /* atendimento + pico por segundo dos dois blocos */
        signal(SIGTERM, on_sig); signal(SIGINT, on_sig);
        long end = now_ms() + (argc > 2 ? atol(argv[2]) : 30) * 1000, sec = now_ms() + 1000;
        int m1 = 0, m2 = 0;
        setvbuf(stdout, NULL, _IOLBF, 0);
        while(!quit && now_ms() < end) {
            if(r16(W_MBOX, 0x0A) == 0x14) {
                uint32_t off = (uint32_t)r16(W_MBOX, 0x9A) * 2;
                for(uint32_t i = 0; i < 0x140 && off + 0x280 + i < 0x8000; i += 2) {
                    int a = (int16_t)r16(W_YM, off + i), b = (int16_t)r16(W_YM, off + 0x280 + i);
                    if(abs(a) > m1) m1 = abs(a);
                    if(abs(b) > m2) m2 = abs(b);
                }
            }
            serve_once();
            stall_us(1000);
            if(now_ms() >= sec) { printf("%ld origem %5d destino %5d\n", (end - now_ms()) / 1000, m1, m2); m1 = m2 = 0; sec += 1000; }
        }
        return 0;
    }
    if(!strcmp(c, "serve")) {                     /* só o atendimento (diagnóstico) */
        signal(SIGTERM, on_sig); signal(SIGINT, on_sig);
        long end = now_ms() + (argc > 2 ? atol(argv[2]) : 5) * 1000;
        while(!quit && now_ms() < end) { serve_once(); stall_us(1000); }
        printf("%lu blocos atendidos, %lu inválidos\n", served, bad);
        return 0;
    }
    fprintf(stderr, "comando desconhecido\n");
    return 2;
}
