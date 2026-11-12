/* mosaico_apps.c for the host renderer, with entry points to the pages a tap opens (the static openers), so uishot.c
   can check them too: the recent-events list and one event, a drive log's details, the clock picker, the Wi-Fi
   password page with its keyboard */
#include <time.h>
time_t uishot_time(time_t *t);
#define time(p) uishot_time(p)   /* the demo case's clock (fw_stubs.c) */
#include "../../../main/mosaico_apps.c"
#include "uishot.h"

void uishot_events_list(void) { ev_open(NULL); }
void uishot_event_detail(void) { ev_detail_open(ev_rows_n ? ev_row_ring[0] : 0); }
void uishot_file_detail(void) { fl_show_detail((void *)(uintptr_t)999999); }
void uishot_clock_picker(void) { ck_open(); }
void uishot_wifi_password(void)
{
    ws_row_t r = { .rssi = -100, .secure = true, .saved = false, .joinable = true };
    snprintf(r.ssid, sizeof r.ssid, "%s", "WWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWW");
    pw_open(&r);
}
void uishot_model_refused(void)
{
    char q[112];
    snprintf(q, sizeof q, "Not switched: %s.", "refused: a drive is running (end the drive first, then switch)");
    ui_confirm(q, "OK", NULL, NULL);
}
void uishot_close_overlays(void)
{
    if (pw_page) { lv_obj_delete(pw_page); pw_page = NULL; }
    if (ck_page) { lv_obj_delete(ck_page); ck_page = NULL; }
}
void uishot_health_page(int k)
{
    if (k == h_page) return;
    h_page = (uint8_t)k;
    h_tab_paint();
    h_build();
}
/* Settings scrolled so the NETWORK GUARD row is at the top of the list (shoot_scrolled stops after six pages) */
void uishot_settings_guard(void)
{
    if (!g_list) return;
    for (uint32_t i = 0; i < lv_obj_get_child_count(g_list); i++) {
        lv_obj_t *c = lv_obj_get_child(g_list, (int32_t)i);
        if (lv_obj_check_type(c, &lv_label_class) && !strcmp(lv_label_get_text(c), "NETWORK GUARD")) {
            lv_obj_update_layout(g_list);
            lv_obj_scroll_to_y(g_list, lv_obj_get_y(c), LV_ANIM_OFF);
            lv_obj_update_layout(g_list);
            return;
        }
    }
}
