#!/usr/bin/env python3
"""Adapta a saída do lv_font_conv 1.5.2 (feita para LVGL 8) ao LVGL 9:
LV_VERSION_CHECK(8,0,0) é falso no v9 e o campo .cache não existe mais."""
import re, sys
for path in sys.argv[1:]:
    s = open(path, encoding="utf-8").read()
    s = s.replace("#if LV_VERSION_CHECK(8, 0, 0)", "#if LVGL_VERSION_MAJOR >= 8")
    s = re.sub(r"\nstatic\s+lv_font_fmt_txt_glyph_cache_t cache;", "", s)
    s = re.sub(r",?\n#if LVGL_VERSION_MAJOR >= 8\n    \.cache = &cache\n#endif", "", s)
    open(path, "w", encoding="utf-8").write(s)
