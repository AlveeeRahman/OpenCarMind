/* mosaico_apps.c for the host renderer, with entry points to the pages a tap opens (the static openers), so uishot.c
   can check them too: the recent-events list and one event, a drive log's details, the clock picker, the Wi-Fi
   password page with its keyboard */
#include "../../main/mosaico_apps.c"
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
