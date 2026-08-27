#include "theme.h"

theme_colors_t th;
static bool s_dark = true;

static void apply_night(void)
{
    // DARK PALETTE — constrained by the panel, not by taste. It flickers for any
    // channel in roughly code 16..110 (measured on hardware 2026-08-27, see
    // CLAUDE.md), with clean zones at 0..12 and ~110+. Flicker visibility scales
    // with AREA, so only LARGE FILLS are bound by that: bg, surface, card,
    // pressed, preview_dish. Text, icons, hairline borders and small accents are
    // free to sit anywhere, which is why text/accent/text2/text3/border keep
    // their original values even though several land mid-range.
    //
    // The cost is that four elevation levels are squeezed into codes 0/6/12,
    // which is nearly indistinguishable. Hierarchy has to come from borders and
    // typography here, not from fill brightness. Do NOT "restore contrast" by
    // lightening these fills — that is exactly what put the whole UI in the
    // flicker band. Raise a border or a text colour instead.
    th.bg = lv_color_hex(0x000000);           // 0,0,0
    th.surface = lv_color_hex(0x040303);      // 4,3,3
    th.card = lv_color_hex(0x080706);         // 8,7,6
    th.pressed = lv_color_hex(0x0c0a08);      // 12,10,8
    th.text = lv_color_hex(0xd8d2c6); // 216,210,198 — was 236
    th.text2 = lv_color_hex(0xa39885);
    th.text3 = lv_color_hex(0x6e6455);
    th.border = lv_color_hex(0x362f26);
    th.border_light = lv_color_hex(0x2b2620);
    th.accent = lv_color_hex(0xdcd9d2); // 220,217,210 — soft white, was 255        // 255,255,255
    th.accent_pressed = lv_color_hex(0xb0ada6); // 176,173,166 // 192 — clean, reads as a press
    th.on_accent = lv_color_hex(0x000000);     // black on a white accent fill
    // accent_soft is a FILL (selected chips, and the current-playlist row at
    // ui.c:392), so it cannot follow the accent: a dim white tint is mid-grey,
    // dead centre of the flicker band. There is nothing between code 12 and
    // ~128 to use, so "soft" is not available on this panel — this stays in the
    // bottom clean zone and selection is carried by the white border + white
    // text the chips already draw.
    th.accent_soft = lv_color_hex(0x0c0a08);   // 12,10,8
    th.ok = lv_color_hex(0x9db07f);
    th.ok_soft = lv_color_hex(0x28301f);
    th.danger = lv_color_hex(0xc65a33);
    th.danger_pressed = lv_color_hex(0xa84a28);
    th.preview_dish = lv_color_hex(0x000000); // 0,0,0
    th.preview_ring = lv_color_hex(0x707070); // 2 px band (thr_preview.c:55): area
    // too small to beat, neutral so the dish rim reads on black without the warm cast
    // The strokes, and the only part of a tile with real area. The old beige
    // (216,181,120) also had a blue channel at 120, inside the unverified gap
    // between the last band that flickered (96) and the first that did not
    // (128). White removes both the beige and the doubt.
    th.preview_sand = lv_color_hex(0xdcd9d2); // 220,217,210 — matches accent, was 255 // 255,255,255
}

static void apply_day(void)
{
    th.bg = lv_color_hex(0xe0d9ca); // -12/ch, was #ECE5D6
    th.surface = lv_color_hex(0xe9e4d9); // -12/ch, was #F5F0E5
    th.card = lv_color_hex(0xd8d0be); // -12/ch, was #E4DCCA
    th.pressed = lv_color_hex(0xcdc3ae); // -12/ch, was #D9CFBA
    th.text = lv_color_hex(0x332c22);
    th.text2 = lv_color_hex(0x7c7161);
    th.text3 = lv_color_hex(0xa89d8a);
    th.border = lv_color_hex(0xcac0ab); // -12/ch, was #D6CCB7
    th.border_light = lv_color_hex(0xd4ccba); // -12/ch, was #E0D8C6
    th.accent = lv_color_hex(0xb0791f);
    th.accent_pressed = lv_color_hex(0x8f6014);
    th.on_accent = lv_color_hex(0xf1ece2); // -12/ch, was #FDF8EE
    th.accent_soft = lv_color_hex(0xded3b6); // -12/ch, was #EADFC2
    th.ok = lv_color_hex(0x5f7a3f);
    th.ok_soft = lv_color_hex(0xd6dac6); // -12/ch, was #E2E6D2
    th.danger = lv_color_hex(0xb0431d);
    th.danger_pressed = lv_color_hex(0x8f3517);
    // NEW and not yet reviewed against the reference app: day-mode previews
    // never existed before (tiles baked the night dish, so day mode showed
    // night previews). Dark strokes on a light dish is the honest inversion;
    // eyeball it on glass before treating these as settled.
    th.preview_dish = lv_color_hex(0xdcd5c4); // -12/ch, was #E8E1D0
    th.preview_ring = lv_color_hex(0xc6bba2); // -12/ch, was #D2C7AE
    th.preview_sand = lv_color_hex(0x8a6a3a);
}

void theme_init(void)
{
    theme_set_dark(true);
}

void theme_set_dark(bool dark)
{
    s_dark = dark;
    if (dark) {
        apply_night();
    } else {
        apply_day();
    }
}

bool theme_is_dark(void)
{
    return s_dark;
}
