/* nmea.c — ver nmea.h.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "nmea.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int hexv(char c)
{
    if(c >= '0' && c <= '9') return c - '0';
    if(c >= 'A' && c <= 'F') return c - 'A' + 10;
    if(c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

/* separa os campos de "$XXYYY,a,b,...*hh" (confere a soma); devolve quantos */
static int split(char *s, char **f, int max)
{
    if(s[0] != '$') return 0;
    char *star = strchr(s, '*');
    if(!star || !star[1] || !star[2]) return 0;
    unsigned sum = 0;
    for(char *p = s + 1; p < star; p++) sum ^= (unsigned char)*p;
    int h1 = hexv(star[1]), h2 = hexv(star[2]);
    if(h1 < 0 || h2 < 0 || (unsigned)(h1 * 16 + h2) != sum) return 0;
    *star = 0;
    int n = 0;
    char *p = s + 1;
    while(n < max) {
        f[n++] = p;
        char *c = strchr(p, ',');
        if(!c) break;
        *c = 0;
        p = c + 1;
    }
    return n;
}

/* ddmm.mmmm + hemisfério -> graus decimais */
static double coord(const char *v, const char *h)
{
    if(!*v) return 0;
    double x = atof(v);
    int deg = (int)(x / 100);
    double d = deg + (x - deg * 100) / 60.0;
    return (*h == 'S' || *h == 'W') ? -d : d;
}

void nmea_init(nmea_state_t *st)
{
    memset(st, 0, sizeof *st);
    st->hdop = 99.99;
}

int nmea_feed(nmea_state_t *st, const char *line)
{
    char buf[256], *f[40];
    snprintf(buf, sizeof buf, "%s", line);
    size_t l = strlen(buf);
    while(l && (buf[l - 1] == '\r' || buf[l - 1] == '\n')) buf[--l] = 0;
    int n = split(buf, f, 40);
    if(n < 1 || strlen(f[0]) != 5) return 0;
    const char *t = f[0] + 2;              /* GP/GN/GL + tipo */
    if(!strcmp(t, "GGA") && n >= 10) {
        snprintf(st->time, sizeof st->time, "%.10s", f[1]);
        st->quality = atoi(f[6]);
        st->used = atoi(f[7]);
        if(*f[8]) st->hdop = atof(f[8]);
        st->alt = atof(f[9]);
        if(st->quality > 0) { st->lat = coord(f[2], f[3]); st->lon = coord(f[4], f[5]); }
        return 0;
    }
    if(!strcmp(t, "GSA") && n >= 3) {
        st->mode = atoi(f[2]);             /* 1 sem fix, 2 = 2D, 3 = 3D */
        return 0;
    }
    if(!strcmp(t, "GSV") && n >= 4) {
        int total = atoi(f[1]), idx = atoi(f[2]);
        if(idx == 1) st->n_new = 0;
        st->view_new = atoi(f[3]);
        for(int k = 4; k < n && st->n_new < NMEA_MAX_SATS; k += 4) {    /* até 4 satélites por frase */
            if(!*f[k]) break;
            nmea_sat_t *s = &st->sats_new[st->n_new++];
            s->prn = atoi(f[k]);
            s->elev = k + 1 < n ? atoi(f[k + 1]) : 0;
            s->az = k + 2 < n ? atoi(f[k + 2]) : 0;
            s->snr = k + 3 < n && *f[k + 3] ? atoi(f[k + 3]) : -1;
        }
        if(idx >= total) {                  /* última frase do grupo: publica */
            memcpy(st->sats, st->sats_new, sizeof st->sats);
            st->n_sats = st->n_new;
            st->view = st->view_new;
        }
        return 0;
    }
    if(!strcmp(t, "RMC") && n >= 10) {
        snprintf(st->time, sizeof st->time, "%.10s", f[1]);
        st->valid = *f[2] == 'A';
        if(st->valid) { st->lat = coord(f[3], f[4]); st->lon = coord(f[5], f[6]); }
        st->speed_kmh = atof(f[7]) * 1.852;
        st->course = atof(f[8]);
        snprintf(st->date, sizeof st->date, "%.6s", f[9]);
        st->epochs++;
        return 1;                           /* fim do segundo (o MNL manda RMC depois de GGA/GSA/GSV) */
    }
    return 0;
}

int nmea_write_status(const nmea_state_t *st, const char *path, long now)
{
    char tmp[300];
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "w");
    if(!f) return -1;
    fprintf(f, "updated=%ld\nepochs=%lu\ntime=%s\ndate=%s\nvalid=%d\nmode=%d\nquality=%d\n"
               "lat=%.6f\nlon=%.6f\nalt=%.1f\nspeed_kmh=%.1f\ncourse=%.1f\nhdop=%.2f\nused=%d\nview=%d\nsats=",
            now, st->epochs, st->time, st->date, st->valid, st->mode, st->quality,
            st->lat, st->lon, st->alt, st->speed_kmh, st->course, st->hdop, st->used, st->view);
    for(int i = 0; i < st->n_sats; i++)
        fprintf(f, "%s%d:%d:%d:%d", i ? "," : "", st->sats[i].prn, st->sats[i].elev, st->sats[i].az, st->sats[i].snr);
    fprintf(f, "\n");
    if(fclose(f)) return -1;
    return rename(tmp, path);
}
