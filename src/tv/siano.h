/* siano.h — sintonizador Siano "Nova B0" do FOSTON (siano.c; docs/tv/02-hardware-sintonizador.md). */
#ifndef BF_SIANO_H
#define BF_SIANO_H
#include <stdint.h>

typedef struct { uint16_t type; uint8_t src, dst; uint16_t len, flags; } __attribute__((packed)) hdr_t;

extern int clk_div;
extern void (*on_msg)(const hdr_t *h, const uint8_t *d, int n);
extern uint16_t got_type;
extern uint8_t got_data[512];
extern int got_n;
extern unsigned long n_msgs;

int map(void);                       /* /dev/mem: GPIO, sistema, SPI */
void power_on(void);                 /* GPIO0/1/2 como o CE */
void power_off(void);
void spi_setup(void);                /* clock id 36, pinos, CFG0/CFG1/CMD */
int fw_download(const char *path);   /* padrão de despertar, 710, 660..., 664 */
int send_msg(uint16_t type, uint8_t src, const void *data, int dn);
int poll_read(void);                 /* 256 bytes; mensagens vão para on_msg */
int wait_msg(uint16_t want, int ms);
void store(const hdr_t *h, const uint8_t *d, int n);
int version(void);
int set_mode_tune(double mhz, int seg);
typedef struct {
    int valid, rf_locked, demod_locked, snr, rssi, in_band_pwr, carrier_offset;
    uint32_t frequency;
} siano_stats_t;
extern siano_stats_t last_stats;
int stats(void);                     /* preenche last_stats */
void msleep(int ms);
long now_ms(void);
void gpio_dir(int pin, int out);
void gpio_set(int pin, int on);
#endif
