#!/bin/sh
# Source audit for the network guard (run by `make ids`): what test_ids.c cannot see. The radio hooks only count and set
# bitmap bits; the hook call sites only call hooks or read the verdict; the guard never transmits, pairs, forgets, sets
# the clock or resets; no address, SSID or BSSID reaches a log or an event; the S3 sources compile no guard code.
src="$(cd "$(dirname "$0")/../../main" && pwd)" || exit 1
tmp=$(mktemp -d) || exit 1
trap 'rm -rf "$tmp"' EXIT
fail=0
bad() { echo "FAIL $*"; fail=1; }
FORBID='malloc|calloc|realloc|free\(|ESP_LOG|printf|sys_event|nvs_|xSemaphore|xQueue|portENTER_CRITICAL|taskENTER_CRITICAL|vTaskDelay|portMAX_DELAY|ble_gap_|esp_wifi_|STATUS_LOCK|_lock|mutex|fopen|fwrite'

# 1. the hooks: between the HOOKS BEGIN and HOOKS END markers of ids_svc.c, and all of ids_hooks.h
awk '/HOOKS BEGIN/{s=1} /HOOKS END/{s=0} s' "$src/ids_svc.c" > "$tmp/hooks.c"
n=$(grep -c '^void ids_hook_' "$tmp/hooks.c")
[ "$n" = 9 ] || bad "expected the 9 hooks between the markers, found $n"
for f in "$tmp/hooks.c" "$src/ids_hooks.h"; do
    grep -n -E "$FORBID" "$f" | grep -v '^\s*[0-9]*:\s*/\?\*' && bad "a hook does more than count: $(basename "$f")"
done
grep -c 'ids_hook_' "$src/ids_svc.c" | awk '{ if ($1 < 9) exit 1 }' || bad "hooks missing in ids_svc.c"
awk '/^void ids_hook_/,/^}/' "$src/ids_svc.c" | grep -q -E 'for *\(|while *\(' && bad "a hook loops (the bitmap clear is in ids_bm_add, bounded)"

# 2. the call sites: every ids_ word in the radio code is a hook, a verdict read or the guard's include
for f in obd_ble.c wifi_sync.c elm327.c; do
    grep -n -o -E '\bids_[a-z_]+' "$src/$f" | grep -v -E ':ids_(hook_(ble_adv|link|link_rtt|wifi_disc|wifi_round|wifi_scan_begin|wifi_ap|wifi_scan_done|wifi_leave)|ble_blocking|wifi_refuse|rows_suspect|svc)$' &&
        bad "$f uses the guard beyond hooks and verdict reads"
done
grep -n 'IDS_HOOK(ids_hook_ble_adv' "$src/obd_ble.c" | grep -q . || bad "no advert hook in obd_ble.c"
awk '/case BLE_GAP_EVENT_DISC: \{/{s=1} s&&/IDS_HOOK\(ids_hook_ble_adv/{h=NR} s&&/ble_gap_conn_active\(\)\) return 0/{c=NR; exit} END{exit !(h && c && h < c)}' "$src/obd_ble.c" ||
    bad "the advert hook is not the first thing in BLE_GAP_EVENT_DISC"
grep -n 'IDS_HOOK(ids_hook_wifi_disc' "$src/wifi_sync.c" | grep -q . || bad "no disconnect hook in wifi_sync.c"
grep -n 'IDS_HOOK(ids_hook_link_rtt' "$src/elm327.c" | grep -q . || bad "no round-trip hook in elm327.c"

# 3. the guard itself never acts on the radios or the clock, never resets, never logs an address
for f in ids_svc.c ids_core.c ids_core.h ids_hooks.h health_view.c; do
    grep -n -E 'ble_gap_(connect|terminate|adv_|security|unpair)|ble_store_util_delete|esp_wifi_(80211_tx|connect|disconnect|deauth)|settimeofday|sys_reset|esp_restart|addr_str|ssid|bssid\[[0-9]' "$src/$f" |
        grep -v -E '^\s*[0-9]+:\s*(/\*| \*)' | grep -v 'ids_hook_wifi_ap(const uint8_t bssid\[6\])' && bad "$f acts on a radio or the clock"
done
grep -n -E 'ESP_LOG.*(addr|bssid|ssid)|sys_event.*(addr|bssid|ssid)' "$src/ids_svc.c" && bad "ids_svc.c may log an address"

# 4. what blocking does is in the consumers, and nothing cuts the paired link
grep -q 'ids_ble_blocking() && s_have_saved && ble_gap_wl_set(&s_saved_addr, 1)' "$src/obd_ble.c" || bad "the accept list is not the paired adapter only"
grep -q 'OBD_PICK_OFFER && ids_ble_blocking()) return OBD_PICK_NONE' "$src/obd_ble.c" || bad "offers are not stopped while blocking"
grep -q 'AG_R2_ASK && ids_ble_blocking()) r = AG_R2_REFUSE' "$src/obd_ble.c" || bad "re-pair requests are not refused while blocking"
grep -n -E 'ids_[a-z_]+\(.*ble_gap_terminate|ble_gap_terminate.*ids_' "$src/obd_ble.c" && bad "the guard terminates a link"
awk '/static bool sync_clock\(/,/^}/' "$src/wifi_sync.c" | awk '/ids_wifi_refuse/{r=NR} /settimeofday/{t=NR} END{exit !(r && t && r < t)}' ||
    bad "the clock is set before the guard's Wi-Fi verdict is read"

# 5. the S3 compiles no guard: every guard line in the shared files is under BOARD_MOSAICO (or an empty IDS_HOOK)
for f in obd_ble.c elm327.c wifi_sync.c; do
    unifdef -DBOARD_MOSAICO=0 -UCONFIG_OCM_IDS "$src/$f" 2>/dev/null | grep -n -E '(^|[^a-z_])ids_[a-z_]+\(' | grep -v 'IDS_HOOK(ids_hook_' && bad "$f: the S3 build reaches guard code"
done
[ $fail = 0 ] && echo "ids audit: hooks count only, call sites clean, the guard never transmits, S3 untouched"
exit $fail
