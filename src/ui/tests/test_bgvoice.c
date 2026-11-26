/* test_bgvoice.c - voice pack: ADPCM keeps the speech (SNR), keys normalize like the PC tool,
 * lookup finds every phrase the navigation asks for (biggps/bgvoice.c).
 *   test_bgvoice [PACOTE.bgv]   with a pack, also checks the phrases in it
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../biggps/bgvoice.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FALHOU %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static void norm_case(const char *in, const char *want)
{
    char out[256];
    bgv_normalize(in, out, sizeof out);
    CHECK(!strcmp(out, want), "normalize(\"%s\") = \"%s\" (esperado \"%s\")", in, out, want);
}

int main(int argc, char **argv)
{
    /* sinal parecido com fala: vogais (harmônicos) que sobem e descem, com pausas */
    enum { N = 22050 * 2 };
    static int16_t in[N], out[N];
    static uint8_t enc[4 + N / 2 + 1];
    for (int i = 0; i < N; i++) {
        double t = i / 22050.0, f0 = 110 + 40 * sin(t * 3), env = fmax(0, sin(t * 9)) * 0.8;
        double v = 0;
        for (int h = 1; h <= 8; h++) v += sin(2 * M_PI * f0 * h * t) / h;
        in[i] = (int16_t)(v * env * 12000);
    }
    size_t len = bgv_encode(in, N, enc);
    CHECK(len == 4 + N / 2, "tamanho codificado %zu", len);
    bgv_clip_t c = { enc, N };
    bgv_decode(&c, out);
    double sig = 0, err = 0;
    for (int i = 0; i < N; i++) { sig += (double)in[i] * in[i]; err += (double)(in[i] - out[i]) * (in[i] - out[i]); }
    double snr = 10 * log10(sig / (err + 1));
    printf("ADPCM 4 bits: SNR %.1f dB, %zu bytes por %d amostras (%.1f KB/s a 22050 Hz)\n", snr, len, N, len / 2.0 / 1024);
    CHECK(snr > 20, "SNR %.1f dB", snr);

    norm_case("Em 300 metros,", "em 300 metros");
    norm_case("  Você chegou ao destino. ", "você chegou ao destino");
    norm_case("Na rotatória, pegue a 1ª saída", "na rotatória, pegue a 1ª saída");
    norm_case("NO TÚNEL  ÁGUA CLARA", "no túnel água clara");
    norm_case("Recalculando rota…", "recalculando rota");
    norm_case("para a DF-440", "para a df-440");

    if (argc > 1) {
        char e[256];
        bgv_pack_t *p = bgv_open(argv[1], e, sizeof e);
        CHECK(p, "%s", e);
        if (p) {
            static const char *need[] = { "Em 300 metros", "vire à direita", "Vire à esquerda", "Siga em frente",
                                          "Na rotatória, pegue a 2ª saída", "e depois", "Você chegou ao destino",
                                          "Recalculando a rota", "Radar a 400 metros", "limite de 60 quilômetros por hora",
                                          "Lombada à frente", "Passagem de nível à frente", "Em 1 quilômetro e meio" };
            uint32_t rate = bgv_rate(p);
            for (unsigned i = 0; i < sizeof need / sizeof need[0]; i++) {
                bgv_clip_t k;
                int ok = bgv_find(p, need[i], &k);
                CHECK(ok, "o pacote não tem \"%s\"", need[i]);
                if (ok) {
                    double s = (double)k.nsamples / rate;
                    CHECK(s > 0.3 && s < 4, "\"%s\": %.2f s", need[i], s);
                    if (i < 3) printf("  \"%s\": %.2f s\n", need[i], s);
                }
            }
            bgv_clip_t k;
            CHECK(!bgv_find(p, "Rua que não existe", &k), "achou frase inexistente");
            printf("pacote: %u frases a %u Hz\n", bgv_count(p), rate);
            bgv_close(p);
        }
    }
    if (fails) { printf("%d falha(s)\n", fails); return 1; }
    printf("ok\n");
    return 0;
}
