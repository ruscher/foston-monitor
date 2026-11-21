/* status_sim.c — valores SIMULADOS para o simulador de PC.
 * Tudo aqui é falso de propósito; status.simulated = true faz a interface
 * exibir o selo "SIM" na barra superior. */
#include "status.h"
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static int sim_backlight = 80;
static int sim_volume = 60;

/* BIGFOSTON_SIM_BATTERY="pct,estado" (d descarregando, c carregando, f carga
 * completa, n ligado sem carregar, u desconhecido) ou sim_set_battery(): para
 * ver e fotografar cada estado do ícone no simulador. */
static int ov_pct = -2;
static char ov_state;

void sim_set_battery(int pct, char state)
{
    ov_pct = pct;
    ov_state = state;
}

static void sim_battery_override(bf_status_t *s)
{
    if(ov_pct == -2) {
        const char *e = getenv("BIGFOSTON_SIM_BATTERY");
        int pct;
        char st;
        if(!e || sscanf(e, "%d,%c", &pct, &st) != 2) return;
        ov_pct = pct;
        ov_state = st;
    }
    s->battery_pct = ov_pct < -1 ? -1 : ov_pct > 100 ? 100 : ov_pct;
    switch(ov_state) {
    case 'c': s->battery_state = BF_BAT_CHARGING; s->charging = BF_YES; s->ext_power = BF_YES; break;
    case 'f': s->battery_state = BF_BAT_FULL; s->charging = BF_NO; s->ext_power = BF_YES; break;
    case 'n': s->battery_state = BF_BAT_NOT_CHARGING; s->charging = BF_NO; s->ext_power = BF_YES; break;
    case 'u': s->battery_state = BF_BAT_UNKNOWN; s->charging = BF_UNKNOWN; break;
    default:  s->battery_state = BF_BAT_DISCHARGING; s->charging = BF_NO; s->ext_power = BF_NO; break;
    }
}

static void sim_read(bf_status_t *s)
{
    time_t t = time(NULL);
    s->battery_present = BF_YES;
    s->battery_pct = 76;                  /* SIMULADO */
    s->charging = BF_NO;
    s->battery_state = BF_BAT_DISCHARGING;
    s->ext_power = BF_NO;
    sim_battery_override(s);
    s->volume_pct = sim_volume;
    s->muted = sim_volume == 0 ? BF_YES : BF_NO;
    s->usb = BF_YES;                      /* SIMULADO */
    s->bluetooth = BF_NO;                 /* SIMULADO */
    s->network = BF_YES;                  /* SIMULADO */
    snprintf(s->ip, sizeof s->ip, "192.168.7.2");
    s->gps_fix = (t / 10) % 2 ? BF_YES : BF_NO; /* SIMULADO: alterna a cada 10 s */
    s->gps_sats = s->gps_fix == BF_YES ? 7 : 2;
}

static int  sim_bl_get(void) { return sim_backlight; }
static bool sim_bl_set(int p) { sim_backlight = p; return true; }
static const char *sim_bl_dev(void) { return "simulador"; }
static int  sim_vol_get(void) { return sim_volume; }
static bool sim_vol_set(int p) { sim_volume = p; return true; }

const bf_status_provider_t bf_status_sim = {
    .name = "sim", .simulated = true, .read = sim_read,
    .backlight_get = sim_bl_get, .backlight_set = sim_bl_set, .backlight_device = sim_bl_dev,
    .volume_get = sim_vol_get, .volume_set = sim_vol_set,
};
