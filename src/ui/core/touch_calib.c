#include "touch_calib.h"
#include "settings.h"
#include <stdio.h>
#include <unistd.h>

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
    bool ok = fflush(f) == 0 && fsync(fileno(f)) == 0;   /* cartão SD */
    return fclose(f) == 0 && ok;
}

/* Calibração gravada pelo usuário ($BIGFOSTON_DATA/pointercal) ou, se não houver,
 * a medida na fábrica do BigFoston e embutida no sistema. */
#define FACTORY_CAL "/etc/bigfoston/touchcal"

bool calib_load(bf_calib_t *c)
{
    char p[300];
    data_path(p, sizeof p, "pointercal");
    FILE *f = fopen(p, "r");
    if(!f) f = fopen(FACTORY_CAL, "r");
    if(!f) return false;
    int v[7];
    bool ok = fscanf(f, "%d %d %d %d %d %d %d", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6]) == 7;
    fclose(f);
    if(!ok || v[6] == 0) return false;
    c->a = v[0]; c->b = v[1]; c->c = v[2]; c->d = v[3]; c->e = v[4]; c->f = v[5]; c->s = v[6];
    return true;
}

#ifdef BIGFOSTON_TARGET
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <unistd.h>

/* Leitor evdev próprio: o lv_evdev do LVGL 9.2 só faz escala linear por eixo,
 * e o painel do FS-707DC precisa da transformação afim de 5 pontos. */
static struct {
    int fd;
    bool have_cal, down;
    bool seen;              /* a pressão atual já foi entregue ao LVGL */
    bool up_pending;        /* soltou na mesma leitura em que encostou: solta na próxima */
    bf_calib_t cal;
    int32_t rx, ry;         /* bruto, último valor do evdev */
    lv_point_t pt;          /* calibrado e filtrado */
    int32_t hx[3], hy[3];   /* últimas amostras calibradas (mediana) */
    unsigned filt_n;        /* amostras desde que o dedo encostou */
} ts = { .fd = -1 };

static int32_t med3(int32_t a, int32_t b, int32_t c)
{
    if(a > b) { int32_t t = a; a = b; b = t; }
    if(b > c) b = c;
    return a > b ? a : b;
}

static void ts_map(void)
{
    int32_t x = ts.rx, y = ts.ry;
    if(ts.have_cal) {
        x = (int32_t)(((int64_t)ts.cal.a * ts.rx + (int64_t)ts.cal.b * ts.ry + ts.cal.c) / ts.cal.s);
        y = (int32_t)(((int64_t)ts.cal.d * ts.rx + (int64_t)ts.cal.e * ts.ry + ts.cal.f) / ts.cal.s);
    }
    else {                  /* sem calibração: escala crua 0..1023 -> tela */
        x = ts.rx * 800 / 1024;
        y = ts.ry * 480 / 1024;
    }
    x = LV_CLAMP(0, x, 799);
    y = LV_CLAMP(0, y, 479);
    /* Painel resistivo: picos isolados (sobretudo ao encostar e ao soltar) e tremor de
     * alguns pixels deixavam a rolagem instável. Mediana das 3 últimas amostras (tira os
     * picos), média com a posição anterior (suaviza) e zona morta de 2 px parado. */
    if(!ts.filt_n) { for(int i = 0; i < 3; i++) { ts.hx[i] = x; ts.hy[i] = y; } ts.pt.x = x; ts.pt.y = y; }
    ts.hx[ts.filt_n % 3] = x;
    ts.hy[ts.filt_n % 3] = y;
    ts.filt_n++;
    int32_t mx = med3(ts.hx[0], ts.hx[1], ts.hx[2]), my = med3(ts.hy[0], ts.hy[1], ts.hy[2]);
    int32_t nx = (ts.pt.x + mx + 1) / 2, ny = (ts.pt.y + my + 1) / 2;
    if(LV_ABS(nx - ts.pt.x) >= 2 || LV_ABS(ny - ts.pt.y) >= 2) { ts.pt.x = nx; ts.pt.y = ny; }
}

static void ts_read(lv_indev_t *indev, lv_indev_data_t *data)
{
    (void)indev;
    struct input_event ev;
    /* Toque curto com a interface ocupada (vídeo em tela cheia: ~60 ms por quadro): o
     * "encostou" e o "soltou" chegam juntos numa leitura só e o LVGL via só o estado final,
     * solto; o toque sumia. Então entrega a pressão agora e a soltura na leitura seguinte
     * (main.c chama de novo enquanto touch_pending()). */
    if(ts.up_pending) ts.up_pending = ts.down = false;
    else while(read(ts.fd, &ev, sizeof ev) == (ssize_t)sizeof ev) {
        if(ev.type == EV_ABS && ev.code == ABS_X) ts.rx = ev.value;
        else if(ev.type == EV_ABS && ev.code == ABS_Y) ts.ry = ev.value;
        else if(ev.type == EV_KEY && ev.code == BTN_TOUCH) {
            if(!ev.value && ts.down && !ts.seen) { ts.up_pending = true; break; }
            ts.down = ev.value != 0;
            if(ts.down) { ts.filt_n = 0; ts.seen = false; }   /* novo toque: o filtro recomeça */
        }
        else if(ev.type == EV_SYN) ts_map();
    }
    data->point = ts.pt;
    data->state = ts.down ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
    if(ts.down) ts.seen = true;
}

lv_indev_t *touch_create(const char *dev)
{
    ts.fd = open(dev, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if(ts.fd < 0) return NULL;
    lv_indev_t *in = lv_indev_create();
    lv_indev_set_type(in, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(in, ts_read);
    /* por evento: o laço principal espera no fd (poll) e lê na hora em que o kernel
     * entrega o toque; enquanto o dedo está na tela o LVGL lê pelo próprio timer */
    lv_indev_set_mode(in, LV_INDEV_MODE_EVENT);
    /* rolagem: só começa depois de 16 px (toques não viram rolagem) e o arremesso
     * desacelera mais rápido (menos "escorregões" com o tremor do resistivo) */
    lv_indev_set_scroll_limit(in, 16);
    lv_indev_set_scroll_throw(in, 20);
    return in;
}

int touch_fd(void) { return ts.fd; }

bool touch_pending(void) { return ts.up_pending; }

bool touch_last_raw(bf_pt_t *out)
{
    if(ts.fd < 0) return false;
    out->x = ts.rx;
    out->y = ts.ry;
    return true;
}

bool touch_calibration_apply(const bf_calib_t *c)
{
    if(!c->s) return false;
    ts.cal = *c;
    ts.have_cal = true;
    return ts.fd >= 0;
}
#else
bool touch_last_raw(bf_pt_t *out) { (void)out; return false; }

bool touch_calibration_apply(const bf_calib_t *c)
{
    (void)c;
    return false;           /* simulador: não há touchscreen */
}
#endif
