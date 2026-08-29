// Design tokens ported from dune-weaver-touch/qml/components/ThemeManager.qml
// ("the table at night": warm basalt ground, bone text, one amber accent).
//
// The QML app targets 800x480 at ~133 PPI. The 5B's 5" 1024x600 panel is
// ~237 PPI, so spacing, radii and type carry a 1.5x scale over the QML pixel
// values there. The 7" board's panel IS 800x480 at ~133 PPI — the QML app's
// own target — so it takes those values verbatim, and the reference app is
// the authority for them rather than any division by 1.5.
//
// Note headerHeight/navHeight were never scaled: QML's 60/64 are what both
// panels use, which is why they sit outside the variant block below.
#pragma once

#include <stdbool.h>

#include "lvgl.h"

#if defined(BOARD_WAVESHARE_7)

// ---- 7" 800x480 @ ~133 PPI: the QML values verbatim ------------------------
// Spacing
#define TH_SPACE_XS 4
#define TH_SPACE_SM 8
#define TH_SPACE_MD 12
#define TH_SPACE_LG 16
#define TH_SPACE_XL 24

// Radii
#define TH_RADIUS_SM 10
#define TH_RADIUS_MD 14

// Hit targets / chrome
#define TH_TOUCH_TARGET 48
#define TH_CONTROL_HEIGHT 56
// No QML counterpart (the on-screen keyboard is firmware-only, for the WiFi
// password): four rows of TH_TOUCH_TARGET, leaving 228 px of content above.
#define TH_KEYBOARD_HEIGHT 192

// Type scale: the QML sizes themselves. Same merged Outfit + Material Icons
// Round + FontAwesome build as the 5B set. Regenerate: tools/gen_fonts.sh
LV_FONT_DECLARE(outfit_12)
LV_FONT_DECLARE(outfit_14)
LV_FONT_DECLARE(outfit_sb_17)
LV_FONT_DECLARE(outfit_sb_24)
#define TH_FONT_EYEBROW (&outfit_12)
#define TH_FONT_CAPTION (&outfit_12)
#define TH_FONT_BODY (&outfit_14)
#define TH_FONT_TITLE (&outfit_sb_17)
#define TH_FONT_DISPLAY (&outfit_sb_24)

#else

// ---- 5B 1024x600 @ ~237 PPI: QML values x1.5 (default) ---------------------
// Spacing (QML value x 1.5)
#define TH_SPACE_XS 6
#define TH_SPACE_SM 12
#define TH_SPACE_MD 18
#define TH_SPACE_LG 24
#define TH_SPACE_XL 36

// Radii
#define TH_RADIUS_SM 15
#define TH_RADIUS_MD 21

// Hit targets / chrome
#define TH_TOUCH_TARGET 72
#define TH_CONTROL_HEIGHT 84
#define TH_KEYBOARD_HEIGHT 280

// Type scale (QML 12/14/17/24 x 1.5): Outfit Regular/SemiBold merged with the
// Material Icons Round subset + LVGL's FontAwesome symbols (LV_SYMBOL_* and
// the keyboard's built-in glyphs keep working). Regenerate: tools/gen_fonts.sh
LV_FONT_DECLARE(outfit_18)
LV_FONT_DECLARE(outfit_21)
LV_FONT_DECLARE(outfit_sb_26)
LV_FONT_DECLARE(outfit_sb_36)
#define TH_FONT_EYEBROW (&outfit_18)
#define TH_FONT_CAPTION (&outfit_18)
#define TH_FONT_BODY (&outfit_21)
#define TH_FONT_TITLE (&outfit_sb_26)
#define TH_FONT_DISPLAY (&outfit_sb_36)

#endif  // BOARD_WAVESHARE_7

// PIXELS ARE NOT SQUARE on the 7" panel. Its standard 800x480 active area is
// 154.08 x 85.92 mm (diagonal 176.4 mm = 6.95", which checks out), so a pixel
// is 0.1926 mm across and 0.179 mm down — 7.6% wider than tall. Anything
// square in PIXELS reads as a horizontal ellipse on the GLASS; confirmed by
// eye 2026-08-28 ("the circles look a bit stretched"). Shapes that must read
// round are drawn narrower in x by this ratio.
//
// The 5B is ~108 x 64.8 mm over 1024x600: 2.3% taller than wide, the opposite
// error and never noticed, so it is left at 1000 deliberately rather than
// moving every circle on an already-validated board.
//
// Waveshare publishes no active area, so 1076 is inferred from the standard
// part — it is the ONE number to change if a panel measures otherwise. The
// sim CANNOT show this: SDL pixels are square, so a corrected circle looks
// like an ellipse there and only hardware confirms it.
#if defined(BOARD_WAVESHARE_7)
#define TH_PX_ASPECT_X1000 1076
#else
#define TH_PX_ASPECT_X1000 1000
#endif

// Width in px that renders as wide as `h_px` is tall. Use for circles/squares.
#define TH_SQUARE_W(h_px) \
    (((h_px) * 1000 + TH_PX_ASPECT_X1000 / 2) / TH_PX_ASPECT_X1000)

// One-off sizes that the token scale does not cover: write them as the QML
// pixel value and let TH_S() place them on this panel. Integer math is exact
// for every value in use, so the 5B keeps the literals it had.
#if defined(BOARD_WAVESHARE_7)
#define TH_S(qml_px) (qml_px)
#else
#define TH_S(qml_px) ((qml_px) * 3 / 2)
#endif

// Same on both panels: QML's radiusPill, headerHeight and navHeight.
#define TH_RADIUS_PILL LV_RADIUS_CIRCLE
#define TH_HEADER_HEIGHT 60
#define TH_NAV_HEIGHT 64

// Material Icons Round glyphs (UTF-8 for the PORTING_NOTES §7 subset).
#define TH_ICON_ADD "\xEE\x85\x85"            // U+E145
#define TH_ICON_ADJUST "\xEE\x8E\x9E"         // U+E39E
#define TH_ICON_BACK "\xEE\x8B\xAA"           // U+E2EA arrow_back
#define TH_ICON_BRIGHTNESS "\xEE\x8E\xAB"     // U+E3AB
#define TH_ICON_CHECK "\xEE\x97\x8A"          // U+E5CA
#define TH_ICON_CIRCLE "\xEE\xBD\x8A"         // U+EF4A
#define TH_ICON_CLOSE "\xEE\x97\x8D"          // U+E5CD
#define TH_ICON_DELETE "\xEE\xA1\xB2"         // U+E872
#define TH_ICON_EXPAND_MORE "\xEE\x97\x8F"    // U+E5CF
#define TH_ICON_HOME "\xEE\xA2\x8A"           // U+E88A
#define TH_ICON_LIGHT_MODE "\xEE\x94\x98"     // U+E518
#define TH_ICON_LIGHTBULB "\xEE\x83\xB0"      // U+E0F0
#define TH_ICON_MUSIC_NOTE "\xEE\x90\x85"     // U+E405
#define TH_ICON_PAUSE "\xEE\x80\xB4"          // U+E034
#define TH_ICON_PLAY "\xEE\x80\xB7"           // U+E037 play_arrow
#define TH_ICON_PLAY_CIRCLE "\xEE\x87\x84"    // U+E1C4
#define TH_ICON_PLAYLIST_PLAY "\xEE\x81\x9F"  // U+E05F
#define TH_ICON_POWER "\xEE\xA2\xAC"          // U+E8AC
#define TH_ICON_QUEUE_MUSIC "\xEE\x80\xBD"    // U+E03D
#define TH_ICON_RADIO_UNCHECKED "\xEE\xA0\xB6" // U+E836
#define TH_ICON_REFRESH "\xEE\x97\x95"        // U+E5D5
#define TH_ICON_RESTART "\xEF\x81\x93"        // U+F053 restart_alt
#define TH_ICON_SEARCH "\xEE\xA2\xB6"         // U+E8B6
#define TH_ICON_SHUFFLE "\xEE\x81\x83"        // U+E043
#define TH_ICON_SKIP_NEXT "\xEE\x81\x84"      // U+E044
#define TH_ICON_STOP "\xEE\x81\x87"           // U+E047
#define TH_ICON_TUNE "\xEE\x90\xA9"           // U+E429
#define TH_ICON_WIFI "\xEE\x98\xBE"           // U+E63E
// WiFi signal ladder, weakest to strongest. One wedge that fills progressively
// (the network_wifi* family) so the five read as a single scale — see
// tools/gen_fonts.sh for why the wifi_1_bar/wifi_2_bar names are NOT these.
#define TH_ICON_WIFI_0 "\xEF\x82\xB0"         // U+F0B0 signal_wifi_0_bar
#define TH_ICON_WIFI_1 "\xEE\xAF\xA4"         // U+EBE4 network_wifi_1_bar
#define TH_ICON_WIFI_2 "\xEE\xAF\x96"         // U+EBD6 network_wifi_2_bar
#define TH_ICON_WIFI_3 "\xEE\xAF\xA1"         // U+EBE1 network_wifi_3_bar
#define TH_ICON_WIFI_4 "\xEE\x86\xBA"         // U+E1BA network_wifi

typedef struct {
    lv_color_t bg;
    lv_color_t surface;
    lv_color_t card;
    lv_color_t pressed;
    lv_color_t text;
    lv_color_t text2;
    lv_color_t text3;
    lv_color_t border;
    lv_color_t border_light;
    lv_color_t accent;
    lv_color_t accent_pressed;
    lv_color_t on_accent;
    lv_color_t accent_soft;
    lv_color_t ok;
    lv_color_t ok_soft;
    lv_color_t danger;
    lv_color_t danger_pressed;
    // Preview dish. Card tiles are bare 4-bit coverage masks (PORTING_NOTES
    // §7a), so these three are what thr_preview.c composites them through —
    // which is why retheming no longer needs the card re-prepped. The dark
    // values are exactly what tiles used to bake, so nothing shifts.
    lv_color_t preview_dish;
    lv_color_t preview_ring;
    lv_color_t preview_sand;
} theme_colors_t;

// Active palette. The default is a PANEL property, not a preference:
// settings.c gives the 7 dark and the 5B light (the 5B flickers in
// mid-greys at 24 Hz). theme_set_dark(false) switches to the light one.
extern theme_colors_t th;

void theme_init(void);
void theme_set_dark(bool dark);
bool theme_is_dark(void);
