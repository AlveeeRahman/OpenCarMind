/* ui_prefetch: the press-time prefetch decision (Smart UI arm A1), as a pure state machine the display glue drives.
 *
 * A press on an app icon or a Back control, in arm A1 of its class (ui_metrics.h decides the arm), arms a prefetch.
 * Back builds at once (refresh the launcher behind the app: no visible change). An app builds once the finger has
 * stayed within UPF_SLOP_PX of the touch-down point for UPF_APP_DELAY_MS, so a press that starts a launcher swipe
 * does not pay for an app screen first. The built target is used only if LVGL then sends CLICKED for the same
 * target; a scroll, a release elsewhere, a new press or a different target cancels it. The prefetch only builds:
 * LVGL's own click still decides whether and where the page changes.
 *
 * Actions returned to the caller: UPF_BUILD (build the target now), UPF_CANCEL (throw the built target away),
 * UPF_COMMIT (show the built target instead of building it), UPF_NONE. C99, no platform includes. */
#ifndef UI_PREFETCH_H
#define UI_PREFETCH_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define UPF_APP_DELAY_MS 24      /* about 1.5 touch reads: past the first move a swipe makes */
#define UPF_SLOP_PX      10

enum { UPF_IDLE = 0, UPF_ARMED, UPF_BUILT, UPF_COMMITTED };
enum { UPF_NONE = 0, UPF_BUILD, UPF_CANCEL, UPF_COMMIT };
enum { UPF_K_APP = 1, UPF_K_BACK = 2 };

typedef struct {
    uint8_t  st, kind;
    uint16_t target;
    uint32_t t_press;
    int16_t  x0, y0;
    uint32_t builds, commits, cancels;     /* counters for the logs */
} upf_t;

/* a new touch-down; kind 0 = not a prefetch target or not arm A1. A target still built from an earlier press is
   cancelled first: then *pre = UPF_CANCEL. Returns UPF_BUILD for Back (built at press). */
int  upf_press(upf_t *p, uint8_t kind, uint16_t target, uint32_t t, int16_t x, int16_t y, int *pre);
int  upf_pressing(upf_t *p, uint32_t t, int16_t x, int16_t y, bool scrolling);
int  upf_scroll_begin(upf_t *p);                 /* the drag scrolls something: no tap is coming */
int  upf_click(upf_t *p, uint8_t kind, uint16_t target);   /* LVGL's click is about to open kind/target */
int  upf_settle(upf_t *p);                        /* the event ended without a click: drop what was built */
bool upf_was_ready(const upf_t *p);               /* the last click used a built target */

#ifdef __cplusplus
}
#endif
#endif
