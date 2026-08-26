#include "theme.h"

theme_colors_t th;
static bool s_dark = true;

static void apply_night(void)
{
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
}

static void apply_day(void)
{
    th.bg = lv_color_hex(0xece5d6);
    th.surface = lv_color_hex(0xf5f0e5);
    th.card = lv_color_hex(0xe4dcca);
    th.pressed = lv_color_hex(0xd9cfba);
    th.text = lv_color_hex(0x332c22);
    th.text2 = lv_color_hex(0x7c7161);
    th.text3 = lv_color_hex(0xa89d8a);
    th.border = lv_color_hex(0xd6ccb7);
    th.border_light = lv_color_hex(0xe0d8c6);
    th.accent = lv_color_hex(0xb0791f);
    th.accent_pressed = lv_color_hex(0x8f6014);
    th.on_accent = lv_color_hex(0xfdf8ee);
    th.accent_soft = lv_color_hex(0xeadfc2);
    th.ok = lv_color_hex(0x5f7a3f);
    th.ok_soft = lv_color_hex(0xe2e6d2);
    th.danger = lv_color_hex(0xb0431d);
    th.danger_pressed = lv_color_hex(0x8f3517);
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
