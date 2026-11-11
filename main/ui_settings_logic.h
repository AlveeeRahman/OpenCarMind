/* ESP-Mosaico Settings logic, pure C (no LVGL, no ESP-IDF), host-tested in tests/host/test_ui_settings.c:
 * the settings blob kept in NVS, its migration between versions, Home-order moves, what a change has to refresh,
 * and the debounce that turns a burst of taps into one NVS write or one rebuild.
 *
 * Why a debounce: before S1, every Settings tap rebuilt the whole launcher in the click handler, refilled every
 * Settings row and retook the launcher pictures (measured on the board: nothing new on screen until 725-1648 ms after
 * the lift). Now a tap changes the tapped control's own style, the value applies in RAM at once, and the slow work
 * (the NVS write, a launcher rebuild after an order change, the theme switch) waits until the taps stop. */
#ifndef UI_SETTINGS_LOGIC_H
#define UI_SETTINGS_LOGIC_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define UI_MAX_APPS 16
typedef struct {
    uint8_t  ver;
    uint8_t  n;                      /* entries in order[] */
    uint8_t  order[UI_MAX_APPS];     /* app registry indices, Home order */
    uint16_t hidden_v5;              /* version 5 could hide apps from Home: read once by the migration, 0 since 6 */
    uint8_t  widget;                 /* ui_widget_t; kept for the blob layout, no Mosaico screen reads it */
    uint8_t  brightness;             /* percent */
    uint16_t screen_off_s;           /* 0 = never */
    uint8_t  econ_l100;              /* economy shown as 0 = km/L, 1 = L/100 km */
    uint8_t  theme;                  /* 0 auto (light by day once the clock is known), 1 light, 2 dark */
} ui_settings_t;
/* 6: the Wi-Fi app joined the registry (just before Settings) and app hiding is gone. Same size and layout as 5.
   7: the E.V app joined the registry right after the R.E.V app (mossREV M1). Same size and layout as 6. */
#define UI_SETTINGS_VER 7

/* ---- loading: the saved blob -> the settings to use ----
   napps = apps in this firmware's registry (Settings is the last one). wifi = the Wi-Fi app's registry index (the app
   version 6 added), ev = the E.V app's registry index (the app version 7 added, registered right after R.E.V); -1 when
   the firmware has none. Each step moves every saved index from the new app's index upwards up by one:
   5 -> 6 puts Wi-Fi in just before Settings and shows every hidden app again, 6 -> 7 puts E.V in just after R.E.V (the
   app before it in the registry), so a user's order is kept around the new app. A version-5 blob takes both steps.
   Any version: invalid and repeated indices are dropped and apps missing from the order are appended in registry
   order. */
enum { OCM_SET_REJECT = 0, OCM_SET_OK, OCM_SET_CHANGED };   /* REJECT: use the defaults; CHANGED: save the result back */
int ocm_settings_migrate(const ui_settings_t *saved, ui_settings_t *out, int napps, int wifi, int ev);

/* ---- Home order: move the app at pos one place up (dir -1) or down (+1); false at either end ---- */
bool ocm_order_move(ui_settings_t *s, int pos, int dir);

/* ---- what changed between two settings, and what that change has to refresh ---- */
enum { OCM_CHG_THEME = 1u, OCM_CHG_ORDER = 2u, OCM_CHG_UNITS = 4u, OCM_CHG_SCREEN_OFF = 8u, OCM_CHG_BRIGHT = 16u,
       OCM_CHG_OTHER = 32u };
unsigned ocm_settings_diff(const ui_settings_t *before, const ui_settings_t *after);
/* OCM_DO_SAVE: one debounced NVS write. OCM_DO_THEME: one debounced theme check (the launcher is rebuilt only if the
   look changes). OCM_DO_LAUNCHER: one debounced launcher rebuild (the Home order changed). OCM_DO_PANEL: a panel
   brightness command. Units and screen-off need nothing else: every value that shows economy calls ui_econ on its
   own next refresh, and the screen-off timer reads the setting on every UI tick. */
enum { OCM_DO_SAVE = 1u, OCM_DO_THEME = 2u, OCM_DO_LAUNCHER = 4u, OCM_DO_PANEL = 8u };
unsigned ocm_settings_actions(unsigned chg);

/* ---- debounce: fires once, quiet_ms after the last touch, or max_defer_ms after the first touch of a burst
   that never pauses (so a write is never put off for ever) ---- */
typedef struct { bool pending; int64_t first_ms, due_ms; } ocm_debounce_t;
void    ocm_debounce_touch(ocm_debounce_t *d, int64_t now_ms, int32_t quiet_ms);
bool    ocm_debounce_fire(ocm_debounce_t *d, int64_t now_ms, int32_t max_defer_ms);   /* true once, then idle */
int32_t ocm_debounce_wait_ms(const ocm_debounce_t *d, int64_t now_ms, int32_t max_defer_ms, int32_t cap_ms);
void    ocm_debounce_cancel(ocm_debounce_t *d);

#define OCM_SAVE_QUIET_MS      500    /* NVS write: 500 ms after the last change */
#define OCM_SAVE_MAX_MS        3000   /* ... and at most 3 s after the first one */
#define OCM_THEME_QUIET_MS     400    /* theme: one switch once the taps stop */
#define OCM_LAUNCHER_QUIET_MS  600    /* launcher rebuild after order moves */
#define OCM_IDLE_FINGER_MS     250    /* deferred UI work waits until the finger has been up this long */

#ifdef __cplusplus
}
#endif
#endif
