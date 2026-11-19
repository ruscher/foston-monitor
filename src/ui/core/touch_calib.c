#include "touch_calib.h"
#include "settings.h"
#include <stdio.h>

/* Resolve [sxx sxy sx; sxy syy sy; sx sy n] * [p q r]^T = [v1 v2 v3]^T (Cramer).
 * ARMv6 sem VFP: double é emulado em software, mas isto roda uma vez só. */
static bool solve3(const double m[3][3], const double v[3], double out[3])
{
    double det = m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1])
               - m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0])
               + m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);
    if(det > -1e-9 && det < 1e-9) return false;
    for(int k = 0; k < 3; k++) {
        double t[3][3];
        for(int i = 0; i < 3; i++)
            for(int j = 0; j < 3; j++) t[i][j] = j == k ? v[i] : m[i][j];
        out[k] = (t[0][0] * (t[1][1] * t[2][2] - t[1][2] * t[2][1])
                - t[0][1] * (t[1][0] * t[2][2] - t[1][2] * t[2][0])
                + t[0][2] * (t[1][0] * t[2][1] - t[1][1] * t[2][0])) / det;
    }
    return true;
}

bool calib_compute(const bf_pt_t scr[BF_CALIB_POINTS], const bf_pt_t tp[BF_CALIB_POINTS],
                   bf_calib_t *o, int *err_px)
{
    double m[3][3] = { { 0 } }, vx[3] = { 0 }, vy[3] = { 0 };
    for(int i = 0; i < BF_CALIB_POINTS; i++) {
        double x = tp[i].x, y = tp[i].y;
        m[0][0] += x * x; m[0][1] += x * y; m[0][2] += x;
        m[1][1] += y * y; m[1][2] += y;
        vx[0] += x * scr[i].x; vx[1] += y * scr[i].x; vx[2] += scr[i].x;
        vy[0] += x * scr[i].y; vy[1] += y * scr[i].y; vy[2] += scr[i].y;
    }
    m[1][0] = m[0][1]; m[2][0] = m[0][2]; m[2][1] = m[1][2]; m[2][2] = BF_CALIB_POINTS;
    double px[3], py[3];
    if(!solve3(m, vx, px) || !solve3(m, vy, py)) return false;
    const double S = 65536.0;
    o->s = 65536;
    o->a = (int32_t)(px[0] * S); o->b = (int32_t)(px[1] * S); o->c = (int32_t)(px[2] * S);
    o->d = (int32_t)(py[0] * S); o->e = (int32_t)(py[1] * S); o->f = (int32_t)(py[2] * S);
    long long sum = 0;
    for(int i = 0; i < BF_CALIB_POINTS; i++) {
        long long X = ((long long)o->a * tp[i].x + (long long)o->b * tp[i].y + o->c) / o->s;
        long long Y = ((long long)o->d * tp[i].x + (long long)o->e * tp[i].y + o->f) / o->s;
        long long dx = X - scr[i].x, dy = Y - scr[i].y;
        long long d2 = dx * dx + dy * dy, r = 0;
        while((r + 1) * (r + 1) <= d2) r++;  /* raiz inteira (valores pequenos) */
        sum += r;
    }
    if(err_px) *err_px = (int)(sum / BF_CALIB_POINTS);
    /* sanidade: escala absurda => pontos tocados errado */
    return o->a > 0 && o->e > 0;
}

bool calib_save(const bf_calib_t *c)
{
    char p[300];
    data_path(p, sizeof p, "pointercal");
    FILE *f = fopen(p, "w");
    if(!f) return false;
    fprintf(f, "%d %d %d %d %d %d %d 800 480\n", (int)c->a, (int)c->b, (int)c->c,
            (int)c->d, (int)c->e, (int)c->f, (int)c->s);
    return fclose(f) == 0;
}

bool calib_load(bf_calib_t *c)
{
    char p[300];
    data_path(p, sizeof p, "pointercal");
    FILE *f = fopen(p, "r");
    if(!f) return false;
    int v[7];
    bool ok = fscanf(f, "%d %d %d %d %d %d %d", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6]) == 7;
    fclose(f);
    if(!ok || v[6] == 0) return false;
    c->a = v[0]; c->b = v[1]; c->c = v[2]; c->d = v[3]; c->e = v[4]; c->f = v[5]; c->s = v[6];
    return true;
}

bool touch_calibration_apply(const bf_calib_t *c)
{
    /* TODO(firmware): quando existir o driver do touchscreen resistivo do
     * MT3351, aplicar aqui (ex.: envolver o read_cb do lv_evdev com a
     * transformação, ou gravar /etc/pointercal para o tslib). */
    fprintf(stderr, "bigfoston: touch_calibration_apply(a=%d b=%d c=%d d=%d e=%d f=%d s=%d): "
            "STUB — driver de toque ainda não existe\n", (int)c->a, (int)c->b, (int)c->c,
            (int)c->d, (int)c->e, (int)c->f, (int)c->s);
    return false;
}
