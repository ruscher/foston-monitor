/* fx.h — efeitos da interface: sons de toque, animações e tela que escurece
 * sem uso (Ajustes > Aparência).
 *
 * Sons: sintetizados na inicialização (PCM 16 bits mono, 22050 Hz, poucos KB)
 * e tocados numa thread em /dev/dsp aberto sem bloquear. Se o dispositivo está
 * ocupado (o Multimídia tocando) ou não existe (simulador), o som é pulado. */
#ifndef BF_FX_H
#define BF_FX_H
#include "lvgl.h"
#include <stdbool.h>

typedef enum { FX_CLICK, FX_TOGGLE, FX_STARTUP } fx_sound_t;

/* Liga o som de toque e a detecção de inatividade aos dispositivos de entrada. */
void fx_init(void);
void fx_sound(fx_sound_t s);         /* respeita o ajuste (o de abertura, boot_sound) */
bool fx_audio_present(void);
lv_anim_enable_t fx_anim(void);      /* LV_ANIM_ON se as animações estão ligadas */
/* Desliza um objeto (translate) de (dx,dy) até a posição normal. */
void fx_slide_in(lv_obj_t *obj, int dx, int dy, uint32_t delay_ms, uint32_t ms);
/* Modo performance: sem sons de toque nem animações. */
void fx_quiet(bool on);
/* Vídeo tocando: a tela não escurece por falta de toque. */
void fx_keep_awake(bool on);
/* Chamado a cada segundo: escurece a tela após o tempo sem toque. */
void fx_tick(void);
/* tela escurecida? acende (teclado físico); true se estava escura */
bool fx_undim(void);
#endif
