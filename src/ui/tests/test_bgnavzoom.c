/* test_bgnavzoom.c - camera zoom during navigation: by speed, closer before maneuvers, more for
 * complex ones, back after (biggps/bgnav.c: bgn_zoom256).
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include <stdio.h>
#include "../biggps/bgnav.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FALHOU %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static int zoom(int type, double dist_m, double kmh, int profile)
{
    bgn_man_t m[2] = { { .type = (uint8_t)type }, { .type = BGN_ARRIVE } };
    bgn_route_t r = { .man = m, .nman = 2 };
    bgn_state_t st = { .valid = 1, .next = 0, .next_dm = (uint32_t)(dist_m * 10) };
    return bgn_zoom256(&r, &st, kmh, profile);
}

int main(void)
{
    const int Z165 = 16 * 256 + 128, Z16 = 16 * 256, Z155 = 15 * 256 + 128, Z17 = 17 * 256, Z175 = 17 * 256 + 128;
    CHECK(zoom(BGN_RIGHT, 2000, 40, BGR_CAR) == Z165, "cidade, longe da curva");
    CHECK(zoom(BGN_RIGHT, 2000, 60, BGR_CAR) == Z16, "60 km/h");
    CHECK(zoom(BGN_RIGHT, 5000, 100, BGR_CAR) == Z155, "estrada");
    CHECK(zoom(BGN_RIGHT, 100, 40, BGR_CAR) == Z17, "curva a 100 m: aproxima");
    CHECK(zoom(BGN_RIGHT, 130, 40, BGR_CAR) == Z165, "curva a 130 m a 40 km/h: ainda não");
    CHECK(zoom(BGN_ROUNDABOUT, 190, 40, BGR_CAR) == Z175, "rotatória a 190 m: mais perto");
    CHECK(zoom(BGN_RAMP_RIGHT, 240, 100, BGR_CAR) == Z175, "saída a 240 m a 100 km/h (9 s = 250 m)");
    CHECK(zoom(BGN_RAMP_RIGHT, 300, 100, BGR_CAR) == Z155, "saída a 300 m a 100 km/h: ainda não");
    CHECK(zoom(BGN_STRAIGHT, 50, 40, BGR_CAR) == Z165, "seguir em frente não aproxima");
    CHECK(zoom(BGN_ARRIVE, 50, 40, BGR_CAR) == Z165, "chegada não aproxima");
    CHECK(zoom(BGN_ROUNDABOUT, 50, 15, BGR_BIKE) == Z17, "bicicleta: sempre 17");
    bgn_route_t r = { 0 };
    bgn_state_t st = { 0 };
    CHECK(bgn_zoom256(&r, &st, 40, BGR_CAR) == Z165, "sem rota casada: pela velocidade");
    if (fails) { printf("%d falha(s)\n", fails); return 1; }
    printf("ok\n");
    return 0;
}
