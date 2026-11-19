#!/usr/bin/env python3
"""Gera src/ui/lv_conf.h a partir do lv_conf_template.h do LVGL vendorizado,
aplicando os ajustes do BigFoston. Reexecute após atualizar o LVGL."""
import os, re
UI = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
s = open(os.path.join(UI, "third_party/lvgl/lv_conf_template.h"), encoding="utf-8-sig").read()

def setd(name, val):
    global s
    pat = re.compile(r'^(\s*#define\s+' + re.escape(name) + r')(?=[ \t\n])[ \t]*[^\n]*?([ \t]*/\*.*)?$', re.M)
    s, n = pat.subn(lambda m: m.group(1) + ' ' + val + (m.group(2) or ''), s, count=1)
    assert n == 1, name

s = s.replace('#if 0 /*Set it to "1" to enable content*/', '#if 1 /* BigFoston: habilitado */', 1)
s = s.replace(' * Configuration file for v9.2.2', ' * Configuration file for v9.2.2\n'
              ' * GERADO por src/ui/tools/make_lv_conf.py — BigFoston OS shell, ajustado para\n'
              ' * FOSTON FS-707DC (MT3351, ARMv6 sem VFP, 128 MB RAM, 800x480 RGB565, toque\n'
              ' * resistivo de ponto único). -DBIGFOSTON_TARGET => fbdev+evdev;\n'
              ' * -DBIGFOSTON_SIM_SDL => janela SDL2; nenhum => só renderização headless.', 1)
setd('LV_USE_STDLIB_MALLOC', 'LV_STDLIB_CLIB')     # malloc da libc (musl) — sem pool fixo
setd('LV_USE_STDLIB_STRING', 'LV_STDLIB_CLIB')
setd('LV_DPI_DEF', '133')                           # 7" 800x480
for f in ['L8', 'AL88', 'I1']:  # RGB888 fica: exigido pelo XRGB8888 no 9.2
    setd('LV_DRAW_SW_SUPPORT_' + f, '0')
setd('LV_USE_LOG', '1')
setd('LV_LOG_PRINTF', '1')
for w in ['ANIMIMG', 'CALENDAR', 'CANVAS', 'CHECKBOX', 'DROPDOWN', 'IMAGEBUTTON', 'LED', 'LINE',
          'LIST', 'MENU', 'MSGBOX', 'ROLLER', 'SCALE', 'SPAN', 'SPINBOX', 'SPINNER', 'TABLE',
          'TABVIEW', 'WIN', 'THEME_SIMPLE', 'THEME_MONO', 'OBSERVER']:
    setd('LV_USE_' + w, '0')
setd('LV_FONT_MONTSERRAT_14', '0')
setd('LV_FONT_CUSTOM_DECLARE', 'LV_FONT_DECLARE(bf_font_14) LV_FONT_DECLARE(bf_font_20) '
     'LV_FONT_DECLARE(bf_font_28) LV_FONT_DECLARE(bf_mono_16)')
setd('LV_FONT_DEFAULT', '&bf_font_20')
setd('LV_USE_SDL', 'BIGFOSTON_USE_SDL')
setd('LV_USE_LINUX_FBDEV', 'BIGFOSTON_USE_FBDEV')
setd('LV_LINUX_FBDEV_BUFFER_COUNT', '2')            # buffer duplo
setd('LV_LINUX_FBDEV_BUFFER_SIZE', '48')            # 48 linhas = 1/10 de 480
setd('LV_USE_EVDEV', 'BIGFOSTON_USE_FBDEV')
setd('LV_BUILD_EXAMPLES', '0')
pre = '''
/* BigFoston: seleção de drivers pelo alvo de build */
#if defined(BIGFOSTON_TARGET)
    #define BIGFOSTON_USE_FBDEV 1
    #define BIGFOSTON_USE_SDL   0
#elif defined(BIGFOSTON_SIM_SDL)
    #define BIGFOSTON_USE_FBDEV 0
    #define BIGFOSTON_USE_SDL   1
#else
    #define BIGFOSTON_USE_FBDEV 0
    #define BIGFOSTON_USE_SDL   0
#endif
'''
s = s.replace('#ifndef LV_CONF_H\n#define LV_CONF_H\n', '#ifndef LV_CONF_H\n#define LV_CONF_H\n' + pre, 1)
open(os.path.join(UI, "lv_conf.h"), "w").write(s)
print("lv_conf.h gerado")
