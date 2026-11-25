/* test_bgmotion.c - the vehicle glides between GPS fixes without jumping, cutting corners,
 * wandering while parked or driving on after the signal is lost (biggps/bgmotion.c).
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include "../biggps/bgmotion.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FALHOU %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* near the equator: 1 m = U world units */
#define Y0 2147483648.0
static double U;

static uint32_t rx[64], cd[64];
static int rn;
static void route_add(double xm, double ym)
{
    rx[2 * rn] = (uint32_t)lround(1e6 + xm * U);
    rx[2 * rn + 1] = (uint32_t)lround(Y0 - ym * U);       /* y metres northwards */
    if (rn) {
        double dx = ((double)rx[2 * rn] - rx[2 * rn - 2]) / U, dy = ((double)rx[2 * rn + 1] - rx[2 * rn - 1]) / U;
        cd[rn] = cd[rn - 1] + (uint32_t)lround(sqrt(dx * dx + dy * dy) * 10);
    } else {
        cd[0] = 0;
    }
    rn++;
}

/* distance in metres from (x,y) to the route polyline */
static double off_route_m(double x, double y)
{
    double best = 1e30;
    for (int i = 0; i + 1 < rn; i++) {
        double ax = rx[2 * i], ay = rx[2 * i + 1], dx = (double)rx[2 * i + 2] - ax, dy = (double)rx[2 * i + 3] - ay;
        double l2 = dx * dx + dy * dy, t = l2 > 0 ? ((x - ax) * dx + (y - ay) * dy) / l2 : 0;
        if (t < 0) t = 0;
        if (t > 1) t = 1;
        double px = ax + t * dx - x, py = ay + t * dy - y, d = sqrt(px * px + py * py);
        if (d < best) best = d;
    }
    return best / U;
}

/* true position on the route at distance s metres */
static void truth(double s, uint32_t *x, uint32_t *y)
{
    bgmo_t m;
    bgmo_reset(&m);
    bgmo_set_route(&m, rx, cd, rn);
    double fx, fy, h;
    bgmo_route_point(&m, s * 10, 0, &fx, &fy, &h);
    *x = (uint32_t)lround(fx);
    *y = (uint32_t)lround(fy);
}

/* drive the route at v m/s with one fix per second, 20 display steps per second */
static void drive_n(const char *name, double v, double max_dturn, double noise_m, int jitter_ms)
{
    bgmo_t m;
    bgmo_reset(&m);
    bgmo_set_route(&m, rx, cd, rn);
    double total = cd[rn - 1] / 10.0, last_s = -1, max_step = 0, max_lag = 0, max_off = 0, max_turn = 0, last_cam = -1;
    for (uint64_t t = 0; t <= (uint64_t)(total / v * 1e6) + 3000000; t += 50000) {
        double s_true = v * t / 1e6;
        if (s_true > total) s_true = total;
        if (t % 1000000 == 0) {
            uint32_t x, y;
            /* the fix arrives late by up to jitter_ms and is off along the road by up to noise_m */
            double late = jitter_ms ? (rand() % (jitter_ms + 1)) / 1000.0 : 0;
            double sf = s_true - v * late + (noise_m > 0 ? ((rand() % 2001) - 1000) / 1000.0 * noise_m : 0);
            if (sf < 0) sf = 0;
            if (sf > total) sf = total;
            truth(sf, &x, &y);
            bgmo_fix(&m, t, x, y, v * 3.6, -1, 1, (uint32_t)lround(sf * 10));
        }
        bgmo_step(&m, t);
        double s = m.s / 10;
        if (last_s >= 0) {
            CHECK(s >= last_s - 1e-6, "%s: andou para trás em t=%.2f (%.2f -> %.2f)", name, t / 1e6, last_s, s);
            if (s - last_s > max_step) max_step = s - last_s;
        }
        if (t > 2000000 && s_true < total) {
            double lag = fabs(s_true - s);
            if (lag > max_lag) max_lag = lag;
        }
        double off = off_route_m(m.x, m.y);
        if (off > max_off) max_off = off;
        if (last_cam >= 0) {
            double d = fabs(bgmo_angle_diff(last_cam, m.cam));
            if (d > max_turn) max_turn = d;
        }
        last_cam = m.cam;
        last_s = s;
    }
    printf("%s: passo máx %.2f m (%.2f esperado), atraso máx %.2f m, fora da via %.3f m, giro máx %.1f°/passo\n",
           name, max_step, v * 0.05, max_lag, max_off, max_turn);
    CHECK(max_step < v * 0.05 * 2.5 + noise_m * 0.25, "%s: salto de %.2f m num passo de 50 ms", name, max_step);
    CHECK(max_lag < 3.0 + 1.5 * noise_m + v * jitter_ms / 1000.0, "%s: ficou %.2f m atrás da posição real", name, max_lag);
    CHECK(max_off < 0.05, "%s: saiu da via %.2f m (cortou a esquina)", name, max_off);
    CHECK(max_turn <= max_dturn, "%s: câmera girou %.1f° num passo", name, max_turn);
    CHECK(fabs(m.s / 10 - total) < 0.5, "%s: não chegou ao fim (%.1f de %.1f m)", name, m.s / 10, total);
}

static void drive(const char *name, double v, double max_dturn) { drive_n(name, v, max_dturn, 0, 0); }

static void test_parked(void)
{
    bgmo_t m;
    bgmo_reset(&m);
    srand(7);
    double x0 = 1e6, y0 = Y0, maxd = 0;
    for (uint64_t t = 0; t < 30000000; t += 50000) {
        if (t % 1000000 == 0) {
            double nx = x0 + ((rand() % 800) - 400) / 100.0 * U, ny = y0 + ((rand() % 800) - 400) / 100.0 * U;
            bgmo_fix(&m, t, (uint32_t)lround(nx), (uint32_t)lround(ny), 0.5, -1, 0, 0);
        }
        bgmo_step(&m, t);
        double d = hypot(m.x - x0, m.y - y0) / U;
        if (d > maxd) maxd = d;
    }
    printf("parado com ruído de ±4 m: o marcador se afastou no máximo %.2f m\n", maxd);
    CHECK(maxd < 6.0, "parado: o marcador andou %.2f m com ruído do GPS", maxd);
}

static void test_signal_lost(void)
{
    bgmo_t m;
    bgmo_reset(&m);
    double v = 20, y = Y0;
    for (uint64_t t = 0; t <= 5000000; t += 50000) {
        if (t % 1000000 == 0)
            bgmo_fix(&m, t, (uint32_t)lround(1e6 + v * t / 1e6 * U), (uint32_t)y, v * 3.6, 90, 0, 0);
        bgmo_step(&m, t);
    }
    double at_loss = 1e6 + v * 5 * U;
    for (uint64_t t = 5050000; t <= 20000000; t += 50000) bgmo_step(&m, t);
    double ahead = (m.x - at_loss) / U;
    printf("sem sinal por 15 s a 72 km/h: o marcador avançou %.1f m além do último ponto (limite %.0f m), incerto=%d\n",
           ahead, BGMO_PRED_M, m.stale);
    CHECK(ahead <= BGMO_PRED_M + 0.5, "sem sinal: continuou andando %.1f m", ahead);
    CHECK(m.stale, "sem sinal: não marcou a posição como incerta");
}

static void test_camera_wrap(void)
{
    bgmo_t m;
    bgmo_reset(&m);
    double mid_max = 0;
    bgmo_fix(&m, 0, 1000000, (uint32_t)Y0, 50, 350, 0, 0);
    for (uint64_t t = 50000; t <= 4000000; t += 50000) {
        if (t == 1000000) bgmo_fix(&m, t, 1000000, (uint32_t)Y0, 50, 10, 0, 0);
        bgmo_step(&m, t);
        double far = fabs(bgmo_angle_diff(0, m.cam));
        if (far > mid_max) mid_max = far;
    }
    printf("rumo 350° -> 10°: câmera em %.1f°, maior afastamento do norte %.1f°\n", m.cam, mid_max);
    CHECK(mid_max <= 10.5, "a câmera deu a volta pelo lado longo (%.1f°)", mid_max);
    CHECK(fabs(bgmo_angle_diff(m.cam, 10)) < 1, "a câmera não chegou ao rumo (%.1f°)", m.cam);
}

static void test_snap(void)
{
    bgmo_t m;
    bgmo_reset(&m);
    bgmo_fix(&m, 0, 1000000, (uint32_t)Y0, 0, -1, 0, 0);
    bgmo_step(&m, 50000);
    bgmo_fix(&m, 1000000, (uint32_t)lround(1e6 + 500 * U), (uint32_t)Y0, 0, -1, 0, 0);
    bgmo_step(&m, 1050000);
    double d = fabs(m.x - (1e6 + 500 * U)) / U;
    CHECK(d < 0.5, "posição 500 m adiante (nova rota): deveria saltar, ficou a %.1f m", d);
}

int main(void)
{
    U = 4294967296.0 / 40075016.686;
    /* reta de 600 m para leste */
    rn = 0;
    route_add(0, 0); route_add(600, 0);
    drive("reta 50 km/h", 50 / 3.6, 3.0);
    drive("reta 110 km/h", 110 / 3.6, 3.0);
    /* esquina de 90° à direita e depois à esquerda */
    rn = 0;
    route_add(0, 0); route_add(200, 0); route_add(200, -150); route_add(400, -150);
    drive("esquinas 40 km/h", 40 / 3.6, 4.6);
    /* retorno (180°) */
    rn = 0;
    route_add(0, 0); route_add(150, 0); route_add(150, -8); route_add(0, -8);
    drive("retorno 20 km/h", 20 / 3.6, 4.6);
    /* GPS de verdade: ponto até 5 m para a frente ou para trás e até 300 ms atrasado */
    rn = 0;
    route_add(0, 0); route_add(200, 0); route_add(200, -150); route_add(400, -150);
    srand(3);
    drive_n("esquinas 50 km/h com ruído", 50 / 3.6, 4.6, 5, 300);
    test_parked();
    test_signal_lost();
    test_camera_wrap();
    test_snap();
    if (fails) { printf("%d falha(s)\n", fails); return 1; }
    printf("ok\n");
    return 0;
}
