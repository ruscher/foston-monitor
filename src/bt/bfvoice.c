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
    /* liga a voz */
    w16(W_MBOX, 0x94, 0x0110);
    w16(W_YM, 0x7F2C, 0);
    w32(W_DSP, 0x04, 0x838);
    /* amplificador do alto-falante (GPIO 5) */
    m32(W_GPIO, 0x300, 1u << 5, 1u << 5);
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
    fprintf(stderr, "comando desconhecido\n");
    return 2;
}
