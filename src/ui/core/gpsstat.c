/* gpsstat.c — ver gpsstat.h. */
#define _DEFAULT_SOURCE
#include "gpsstat.h"
#include "settings.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define SERVICE "/etc/init.d/S60gps"
#define BFX     "/mnt/sd/BigFoston/gps/gps.bfx"

static const char *status_path(void)
{
    const char *e = getenv("BIGFOSTON_GPS_STATUS");
    return e && *e ? e : "/run/bigfoston-gps";
}

static void off_flag(char *out, size_t n)
{
    data_path(out, n, "gps-off");
}

bool gps_available(void)
{
    if(getenv("BIGFOSTON_GPS_STATUS")) return true;     /* simulador / capturas */
    return access(SERVICE, X_OK) == 0 && access(BFX, R_OK) == 0;
}

bool gps_enabled(void)
{
    char f[300];
    off_flag(f, sizeof f);
    return access(f, F_OK) != 0;
}

void gps_set_enabled(bool on)
{
    char f[300];
    off_flag(f, sizeof f);
    if(on) unlink(f);
    else { FILE *fp = fopen(f, "w"); if(fp) fclose(fp); }
    if(access(SERVICE, X_OK)) return;
    pid_t pid = fork();
    if(pid == 0) {
        /* o S60gps já se separa (laço em segundo plano): esperar só o script */
        execl(SERVICE, "S60gps", on ? "start" : "stop", (char *)NULL);
        _exit(127);
    }
    if(pid > 0) { int st; waitpid(pid, &st, 0); }
}

void gps_read(gps_info_t *g)
{
    memset(g, 0, sizeof *g);
    g->hdop = 99.99;
    FILE *f = fopen(status_path(), "r");
    if(!f) { g->state = GPS_OFF; return; }
    char line[1024];
    long updated = 0;
    int valid = 0, mode = 0;
    unsigned long epochs = 0;
    while(fgets(line, sizeof line, f)) {
        char *eq = strchr(line, '=');
        if(!eq) continue;
        *eq = 0;
        char *v = eq + 1;
        v[strcspn(v, "\n")] = 0;
        if(!strcmp(line, "updated")) updated = atol(v);
        else if(!strcmp(line, "epochs")) epochs = strtoul(v, NULL, 10);
        else if(!strcmp(line, "time")) snprintf(g->time, sizeof g->time, "%s", v);
        else if(!strcmp(line, "date")) snprintf(g->date, sizeof g->date, "%s", v);
        else if(!strcmp(line, "valid")) valid = atoi(v);
        else if(!strcmp(line, "mode")) mode = atoi(v);
        else if(!strcmp(line, "lat")) g->lat = atof(v);
        else if(!strcmp(line, "lon")) g->lon = atof(v);
        else if(!strcmp(line, "alt")) g->alt = atof(v);
        else if(!strcmp(line, "speed_kmh")) g->speed_kmh = atof(v);
        else if(!strcmp(line, "course")) g->course = atof(v);
        else if(!strcmp(line, "hdop")) g->hdop = atof(v);
        else if(!strcmp(line, "used")) g->used = atoi(v);
        else if(!strcmp(line, "view")) g->view = atoi(v);
        else if(!strcmp(line, "sats")) {
            char *p = v;
            while(*p && g->n_sats < GPS_MAX_SATS) {
                gps_sat_t *s = &g->sats[g->n_sats];
                if(sscanf(p, "%d:%d:%d:%d", &s->prn, &s->elev, &s->az, &s->snr) == 4) g->n_sats++;
                char *c = strchr(p, ',');
                if(!c) break;
                p = c + 1;
            }
        }
    }
    fclose(f);
    long now = (long)time(NULL);
    /* simulador: "updated=0" vale sempre */
    g->running = updated == 0 || (now - updated >= -5 && now - updated < 5);
    if(!g->running) g->state = GPS_OFF;
    else if(valid && mode >= 3) g->state = GPS_FIX3D;
    else if(valid) g->state = GPS_FIX2D;
    else if(!epochs) g->state = GPS_STARTING;
    else g->state = GPS_SEARCHING;
}
