/* touch_calib.h — calibração afim de 5 pontos (formato compatível com o
 * "pointercal" do tslib: x' = (a*x + b*y + c) / s ; y' = (d*x + e*y + f) / s). */
#ifndef BF_TOUCH_CALIB_H
#define BF_TOUCH_CALIB_H
#include <stdbool.h>
#include <stdint.h>

typedef struct { int32_t x, y; } bf_pt_t;
typedef struct { int32_t a, b, c, d, e, f, s; } bf_calib_t;

#define BF_CALIB_POINTS 5

/* Ajuste por mínimos quadrados. err_px recebe o erro residual médio (px). */
bool calib_compute(const bf_pt_t screen[BF_CALIB_POINTS], const bf_pt_t touch[BF_CALIB_POINTS],
                   bf_calib_t *out, int *err_px);
bool calib_save(const bf_calib_t *c);   /* $BIGFOSTON_DATA/pointercal */
bool calib_load(bf_calib_t *c);
/* Aplica a calibração ao leitor de toque (só no alvo; no simulador retorna false). */
bool touch_calibration_apply(const bf_calib_t *c);

/* Última leitura bruta do touchscreen (ADC 0..1023, como o evdev entrega) do toque
 * atual; false no simulador. A tela de calibração usa isto, não o ponto já calibrado. */
bool touch_last_raw(bf_pt_t *out);

#ifdef BIGFOSTON_TARGET
#include "lvgl.h"
/* Ponteiro LVGL lendo o evdev do toque e aplicando a calibração afim (pointercal). */
lv_indev_t *touch_create(const char *dev);
int touch_fd(void);                      /* fd do evdev do toque (-1 sem) */
#endif
#endif
