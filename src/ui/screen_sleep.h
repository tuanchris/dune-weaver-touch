// Screen sleep + touch-to-wake (STATE.md backlog item 2). After
// settings screen_timeout_s of input inactivity the backlight goes off and a
// full-screen shield swallows the wake touch (PORTING_NOTES §5: wake touches
// can report bogus coordinates, and a wake tap must never press a button).
#pragma once

// Creates the 1 Hz idle-check timer. Call from ui_init() (LVGL context).
// Reads screen_timeout_s live on every tick, so chip changes on the Control
// page apply immediately; 0 = never sleep.
void screen_sleep_init(void);
