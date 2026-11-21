/* perf.h — medidas de desempenho do shell: tempo de desenho do LVGL, duração
 * de cada volta do laço principal (atraso máximo para atender um toque) e
 * quadros do mapa do Big GPS. Usado pelo modo de teste BIGFOSTON_BENCH
 * (docs/performance). */
#ifndef BF_PERF_H
#define BF_PERF_H
#include "lvgl.h"
#include <stdint.h>

typedef enum {
    PERF_RENDER,      /* desenho de um quadro pelo LVGL (render start -> ready) */
    PERF_LOOP,        /* uma chamada de lv_timer_handler */
    PERF_MAP,         /* quadro do mapa na thread do motor (leitura + desenho) */
    PERF_MAP_FETCH,
    PERF_MAP_DRAW,
    PERF_MAP_LABELS,  /* posicionar rótulos na interface */
    PERF__N
} perf_metric_t;

void     perf_init(lv_display_t *d);
void     perf_add(perf_metric_t m, uint32_t us);
void     perf_reset(void);
uint64_t perf_now_us(void);
/* "tag 5.0s fps 9.8 | render n=.. avg=.. p50=.. p95=.. max=.. ms | ..." */
int      perf_format(char *buf, int n, const char *tag);
/* BIGFOSTON_PROF=arquivo: amostra o PC a cada 1 ms de CPU até perf_prof_stop */
void     perf_prof_start(void);
void     perf_prof_stop(void);
#endif
