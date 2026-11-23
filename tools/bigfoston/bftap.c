/* bftap — toca na tela do FOSTON a partir do terminal, para testar a interface no aparelho.
 *
 *   bftap X Y [MS]        toque em (X, Y) da tela 800x480, segurando MS ms (padrão 80)
 *   bftap -d X1 Y1 X2 Y2  arrasta de (X1, Y1) até (X2, Y2)
 *
 * Escreve eventos no evdev do touchscreen (o kernel repassa a quem lê, como um toque de
 * verdade). As coordenadas brutas saem da inversão da calibração do shell
 * ($BIGFOSTON_DATA/pointercal, formato do tslib: x = (a0*rx + a1*ry + a2) / a6 ...).
 *
 * SPDX-License-Identifier: GPL-3.0-or-later */
#define _GNU_SOURCE
#include <fcntl.h>
#include <linux/input.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static int fd;
static long long a[7];

static void ev(int type, int code, int value)
{
    struct input_event e;
    memset(&e, 0, sizeof e);
    e.type = (unsigned short)type;
    e.code = (unsigned short)code;
    e.value = value;
    if(write(fd, &e, sizeof e) != (ssize_t)sizeof e) perror("write");
}

static void ms(int n) { struct timespec t = { n / 1000, (n % 1000) * 1000000L }; nanosleep(&t, NULL); }

/* tela -> bruto: resolve o sistema 2x2 da calibração */
static void raw(int x, int y, int *rx, int *ry)
{
    double det = (double)a[0] * a[4] - (double)a[1] * a[3];
    double bx = (double)x * a[6] - a[2], by = (double)y * a[6] - a[5];
    *rx = (int)((bx * a[4] - by * a[1]) / det + 0.5);
    *ry = (int)((by * a[0] - bx * a[3]) / det + 0.5);
}

static void point(int x, int y, int down)
{
    int rx, ry;
    raw(x, y, &rx, &ry);
    ev(EV_ABS, ABS_X, rx);
    ev(EV_ABS, ABS_Y, ry);
    if(down >= 0) {
        ev(EV_ABS, ABS_PRESSURE, down ? 200 : 0);
        ev(EV_KEY, BTN_TOUCH, down);
    }
    ev(EV_SYN, SYN_REPORT, 0);
}

int main(int argc, char **argv)
{
    const char *data = getenv("BIGFOSTON_DATA");
    char p[300];
    snprintf(p, sizeof p, "%s/pointercal", data && *data ? data : "/data/bigfoston");
    FILE *f = fopen(p, "r");
    if(!f || fscanf(f, "%lld %lld %lld %lld %lld %lld %lld", &a[0], &a[1], &a[2], &a[3], &a[4], &a[5], &a[6]) != 7) {
        fprintf(stderr, "bftap: sem calibração em %s\n", p);
        return 1;
    }
    fclose(f);
    const char *dev = getenv("BIGFOSTON_TOUCH");
    fd = open(dev && *dev ? dev : "/dev/input/event0", O_WRONLY);
    if(fd < 0) { perror("touch"); return 1; }
    if(argc >= 6 && !strcmp(argv[1], "-d")) {
        int x1 = atoi(argv[2]), y1 = atoi(argv[3]), x2 = atoi(argv[4]), y2 = atoi(argv[5]);
        point(x1, y1, 1);
        for(int i = 1; i <= 12; i++) { ms(25); point(x1 + (x2 - x1) * i / 12, y1 + (y2 - y1) * i / 12, -1); }
        ms(40);
        point(x2, y2, 0);
    } else if(argc >= 3) {
        int x = atoi(argv[1]), y = atoi(argv[2]);
        point(x, y, 1);
        ms(argc >= 4 ? atoi(argv[3]) : 80);
        point(x, y, 0);
    } else {
        fprintf(stderr, "uso: bftap X Y [MS] | bftap -d X1 Y1 X2 Y2\n");
        return 2;
    }
    close(fd);
    return 0;
}
