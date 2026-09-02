#include "theme.h"

theme_colors_t th;
static bool s_dark = true;

static void apply_dark(void)
{
#if defined(BOARD_PANEL_800X480)
    // The reference palette, straight from ThemeManager.qml -- "the table at
    // night": warm basalt ground, bone text, one amber accent. Usable here
    // because these panels run ~39 Hz and do NOT flicker (measured: the 7 on
    // 2026-08-29, the CrowPanel on 2026-09-01). See the 5B branch below for
    // why it was ever anything else.
    th.bg = lv_color_hex(0x171310);
    th.surface = lv_color_hex(0x201b16);
    th.card = lv_color_hex(0x2a241d);
    th.pressed = lv_color_hex(0x332c24);
    th.text = lv_color_hex(0xece4d3);
    th.text2 = lv_color_hex(0xa39885);
    th.text3 = lv_color_hex(0x6e6455);
    th.border = lv_color_hex(0x362f26);
    th.border_light = lv_color_hex(0x2b2620);
    th.accent = lv_color_hex(0xe2a860);
    th.accent_pressed = lv_color_hex(0xc98f49);
    th.on_accent = lv_color_hex(0x241a0c);
    th.accent_soft = lv_color_hex(0x3a2f1e);
    th.ok = lv_color_hex(0x9db07f);
    th.ok_soft = lv_color_hex(0x28301f);
    th.danger = lv_color_hex(0xc65a33);
    th.danger_pressed = lv_color_hex(0xa84a28);
    th.preview_dish = lv_color_hex(0x1b1712);
    th.preview_ring = lv_color_hex(0x3e362c);
    th.preview_sand = lv_color_hex(0xd8b578);
#else
    // 5B ONLY -- constrained by the panel, not by taste. It flickers for any
    // channel in roughly code 16..110 (measured 2026-08-27, see CLAUDE.md),
    // with clean zones at 0..12 and ~110+. Flicker visibility scales with
    // AREA, so only LARGE FILLS are bound: bg, surface, card, pressed,
    // preview_dish. Text, icons, hairlines and small accents sit anywhere.
    //
    // The cost is four elevation levels squeezed into codes 0/6/12, which is
    // nearly indistinguishable -- hierarchy comes from borders and typography
    // here, not fill brightness. Do NOT "restore contrast" on THIS branch;
    // that is exactly what put the whole UI in the flicker band. The 800x480
    // boards above have the real palette because they can afford it.
    th.bg = lv_color_hex(0x000000);           // 0,0,0
    th.surface = lv_color_hex(0x040303);      // 4,3,3
    th.card = lv_color_hex(0x080706);         // 8,7,6
    th.pressed = lv_color_hex(0x0c0a08);      // 12,10,8
    th.text = lv_color_hex(0xd8d2c6);         // 216,210,198
    th.text2 = lv_color_hex(0xa39885);
    th.text3 = lv_color_hex(0x6e6455);
    th.border = lv_color_hex(0x362f26);
    th.border_light = lv_color_hex(0x2b2620);
    th.accent = lv_color_hex(0xdcd9d2);       // soft white; amber is mid-band
    th.accent_pressed = lv_color_hex(0xb0ada6);
    th.on_accent = lv_color_hex(0x000000);
    // accent_soft is a FILL, so it cannot follow the accent: a dim tint is
    // mid-grey, dead centre of the flicker band. Selection is carried by the
    // border and text instead.
    th.accent_soft = lv_color_hex(0x0c0a08);  // 12,10,8
    th.ok = lv_color_hex(0x9db07f);
    th.ok_soft = lv_color_hex(0x28301f);
    th.danger = lv_color_hex(0xc65a33);
    th.danger_pressed = lv_color_hex(0xa84a28);
    th.preview_dish = lv_color_hex(0x000000);
    th.preview_ring = lv_color_hex(0x707070);
    th.preview_sand = lv_color_hex(0xdcd9d2);
#endif
}

static void apply_light(void)
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
    // never existed before (tiles baked the dark dish, so light mode showed
    // dark previews). Dark strokes on a light dish is the honest inversion;
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
        apply_dark();
    } else {
        apply_light();
    }
}

bool theme_is_dark(void)
{
    return s_dark;
}
