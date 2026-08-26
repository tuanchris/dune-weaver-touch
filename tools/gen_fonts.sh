#!/bin/bash
# Generate the app fonts (PORTING_NOTES §7): Outfit + Material Icons Round
# (28-glyph subset) + LVGL's FontAwesome symbol set (keeps LV_SYMBOL_* and the
# keyboard's built-in glyphs working), one merged font per type-scale size.
# Requires node (npx lv_font_conv). Output: src/ui/fonts/*.c
set -euo pipefail
cd "$(dirname "$0")/.."

REF_FONTS=/Volumes/SSD/projects/dune-weaver-pi/dune-weaver-touch/fonts
FA=managed_components/lvgl__lvgl/scripts/built_in_font/FontAwesome5-Solid+Brands+Regular.woff
OUT=src/ui/fonts
mkdir -p "$OUT"

# ASCII + degree, middle dot, dashes, quotes, bullet, ellipsis (strings use · and —)
LATIN='0x20-0x7F,0xB0,0xB7,0x2013-0x2014,0x2018-0x2019,0x201C-0x201D,0x2022,0x2026'
# Material Icons Round subset (PORTING_NOTES §7, 28 glyphs)
MATERIAL='0xe145,0xe39e,0xe2ea,0xe3ab,0xe5ca,0xef4a,0xe5cd,0xe872,0xe5cf,0xe88a,0xe518,0xe0f0,0xe405,0xe034,0xe037,0xe1c4,0xe05f,0xe8ac,0xe03d,0xe836,0xe5d5,0xf053,0xe8b6,0xe043,0xe044,0xe047,0xe429,0xe63e'
# LVGL built-in symbol codepoints (from scripts/built_in_font/built_in_font_gen.py)
SYMS='61441,61448,61451,61452,61453,61457,61459,61461,61465,61468,61473,61478,61479,61480,61502,61507,61512,61515,61516,61517,61521,61522,61523,61524,61543,61544,61550,61552,61553,61556,61559,61560,61561,61563,61587,61589,61636,61637,61639,61641,61664,61671,61674,61683,61724,61732,61787,61931,62016,62017,62018,62019,62020,62087,62099,62212,62189,62810,63426,63650'

gen() { # <size> <weight-file> <out-name>
    npx --yes lv_font_conv --no-compress --bpp 4 --size "$1" \
        --font "$REF_FONTS/$2" -r "$LATIN" \
        --font "$REF_FONTS/MaterialIconsRound-Regular.otf" -r "$MATERIAL" \
        --font "$FA" -r "$SYMS" \
        --format lvgl -o "$OUT/$3.c"
    echo "generated $OUT/$3.c"
}

gen 18 Outfit-Regular.ttf outfit_18
gen 21 Outfit-Regular.ttf outfit_21
gen 26 Outfit-SemiBold.ttf outfit_sb_26
gen 36 Outfit-SemiBold.ttf outfit_sb_36
