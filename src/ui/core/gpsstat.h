/* gpsstat.h — estado do GPS publicado pelo bfgps (/run/bigfoston-gps, chave=valor,
 * reescrito a cada segundo; docs/gps.md). Também controla o serviço (S60gps).
 * BIGFOSTON_GPS_STATUS muda o arquivo (simulador e capturas). */
#ifndef BF_GPSSTAT_H
#define BF_GPSSTAT_H
#include <stdbool.h>

#define GPS_MAX_SATS 32

typedef struct { int prn, elev, az, snr; } gps_sat_t;   /* snr -1 = sem sinal */

typedef enum { GPS_OFF, GPS_STARTING, GPS_SEARCHING, GPS_FIX2D, GPS_FIX3D } gps_state_t;

typedef struct {
    gps_state_t state;
    bool running;                 /* serviço publicando (arquivo atualizado há < 5 s) */
    char time[12], date[8];       /* UTC */
    int used, view;
    double lat, lon, alt, speed_kmh, course, hdop;
    gps_sat_t sats[GPS_MAX_SATS];
    int n_sats;
} gps_info_t;

void gps_read(gps_info_t *g);
bool gps_available(void);         /* imagem do gps.dll no cartão e serviço instalado */
bool gps_enabled(void);           /* não desligado pela interface */
/* liga/desliga o serviço (fork/exec do S60gps, sem shell) e lembra a escolha */
void gps_set_enabled(bool on);
#endif
