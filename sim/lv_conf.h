/* LVGL config for the desktop simulator — mirrors the device's Kconfig
 * (sdkconfig.defaults): RGB565, clib malloc/string/sprintf, the same
 * Montserrat sizes. Everything else takes LVGL 9 defaults. */
#ifndef LV_CONF_H
#define LV_CONF_H

#define LV_COLOR_DEPTH 16

#define LV_USE_STDLIB_MALLOC LV_STDLIB_CLIB
#define LV_USE_STDLIB_STRING LV_STDLIB_CLIB
#define LV_USE_STDLIB_SPRINTF LV_STDLIB_CLIB

#define LV_FONT_MONTSERRAT_14 1
#define LV_FONT_MONTSERRAT_16 1
#define LV_FONT_MONTSERRAT_18 1
#define LV_FONT_MONTSERRAT_20 1
#define LV_FONT_MONTSERRAT_22 1
#define LV_FONT_MONTSERRAT_24 1
#define LV_FONT_MONTSERRAT_26 1
#define LV_FONT_MONTSERRAT_28 1
#define LV_FONT_MONTSERRAT_32 1
#define LV_FONT_MONTSERRAT_36 1
#define LV_FONT_MONTSERRAT_40 1
#define LV_FONT_MONTSERRAT_48 1

/* SDL window backend (sim only; the device uses the RGB panel driver). */
#define LV_USE_SDL 1

/* Self-screenshot for automated visual checks (sim/shot.py). */
#define LV_USE_SNAPSHOT 1

#endif /* LV_CONF_H */
