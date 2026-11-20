#!/bin/sh
# Gera as fontes LVGL do BigFoston (Montserrat com Latin-1 + símbolos LVGL).
# Requer node/npx (lv_font_conv 1.5.2). Saída: src/ui/assets/fonts/*.c
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
OUT="$HERE/../assets/fonts"
F="$HERE/fonts"
CONV=${LV_FONT_CONV:-"npx --yes lv_font_conv@1.5.2"}
mkdir -p "$OUT"

# ASCII + Latin-1 (acentos pt/es) + travessão, reticências, bullet, aspas, › ‹ €
TEXT_RANGE="0x20-0x7E,0xA0-0xFF,0x2013,0x2014,0x2022,0x2026,0x2018,0x2019,0x201C,0x201D,0x2039,0x203A,0x20AC"
# Símbolos LV_SYMBOL_* padrão do LVGL 9.2 + terminal (0xF120) + extras
SYMS="61441,61448,61451,61452,61453,61457,61459,61461,61465,61468,61473,61478,61479,61480,61502,61507,61512,61515,61516,61517,61521,61522,61523,61524,61543,61544,61550,61552,61553,61556,61559,61560,61561,61563,61587,61589,61636,61637,61639,61641,61664,61671,61674,61683,61724,61732,61787,61931,62016,62017,62018,62019,62020,62087,62099,62212,62189,62810,63426,63650,0xF120,0xF0E2,0xF05A,0xF071,0xF017,0xF1EB,0xF6FF,0xF7C0,0xF185,0xF186,0xF287,0xF519,0xF084,0xF1AB,0xF073,0xF05B,0xF0AD,0xF135,0xF015,0xF2F9,0xF005,0xF0A0,0xF002,0xF065,0xF02E,0xF53F,0xF042,0xF03E,0xF0D0,0xF0AC"

for sz in 14 20 28; do
  $CONV --no-compress --no-prefilter --bpp 4 --size $sz \
    --font "$F/Montserrat-Medium.ttf" -r "$TEXT_RANGE" \
    --font "$F/FontAwesome5-Solid+Brands+Regular.woff" -r "$SYMS" \
    --format lvgl --lv-include lvgl.h --force-fast-kern-format \
    -o "$OUT/bf_font_$sz.c"
done

# Fonte monoespaçada (terminal / sysinfo "retrô")
$CONV --no-compress --no-prefilter --bpp 4 --size 16 \
  --font "$F/DejaVuSansMono.ttf" -r "0x20-0x7E,0xA0-0xFF,0x2014,0x2022,0x2500-0x257F,0x2580-0x259F" \
  --format lvgl --lv-include lvgl.h -o "$OUT/bf_mono_16.c"
# LVGL 9: corrige checagens de versão do lv_font_conv
python3 "$HERE/fix_fonts_v9.py" "$OUT"/*.c
# Remove caminhos absolutos do comentário "Opts" (reprodutibilidade)
sed -i "s#$F/##g; s#$OUT/##g" "$OUT"/*.c
echo "fontes geradas em $OUT"
