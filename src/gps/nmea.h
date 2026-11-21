/* nmea.h — interpreta o NMEA do motor (GGA, GSA, GSV, RMC) e grava o estado
 * do GPS num arquivo chave=valor (atômico) para a interface.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef BF_NMEA_H
#define BF_NMEA_H

#define NMEA_MAX_SATS 32

typedef struct { int prn, elev, az, snr; } nmea_sat_t;   /* snr -1 = sem sinal */

typedef struct {
    char time[12], date[8];       /* hhmmss.sss, ddmmyy (UTC) */
    int valid, mode, quality, used, view;
    double lat, lon, alt, speed_kmh, course, hdop;
    nmea_sat_t sats[NMEA_MAX_SATS], sats_new[NMEA_MAX_SATS];
    int n_sats, n_new, view_new;
    unsigned long epochs;
} nmea_state_t;

void nmea_init(nmea_state_t *st);
/* uma frase (com ou sem CR/LF); devolve 1 no fim de cada segundo (RMC) */
int  nmea_feed(nmea_state_t *st, const char *line);
int  nmea_write_status(const nmea_state_t *st, const char *path, long now);
#endif
