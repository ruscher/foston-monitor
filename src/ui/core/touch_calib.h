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
/* Aplica a calibração ao driver de toque.
 * STUB: o driver do touchscreen do FS-707DC ainda não existe. Retorna false
 * ("pendente") e só registra no log. Ponto de integração para o firmware. */
bool touch_calibration_apply(const bf_calib_t *c);
#endif
