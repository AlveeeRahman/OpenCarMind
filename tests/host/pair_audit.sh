#!/bin/sh
# Source audit for the adapter pairing rules (run by `make pair`): what test_obd_pair.c cannot see in obd_ble.c, the
# firmware glue around the pure rules (owner 2026-11-11, "ask + strong signal").
cd "$(dirname "$0")/../../main" || exit 1
fail=0
bad() { echo "FAIL $*"; fail=1; }

# 1. Nothing is saved without the user's yes: the address is written to NVS only in save_adapter(), and save_adapter() is
#    called only from pair_finish()'s OBD_DONE_SAVE branch, which only obd_pair_answer(.., OBD_ANS_YES, ..) produces.
w=$(grep -n 'nvs_set_blob(h, "addr"' obd_ble.c | wc -l | tr -d ' ')
[ "$w" = 1 ] || bad "obd_ble.c writes the adapter address in $w places (expected 1, save_adapter)"
awk '/^static void save_adapter\(/,/^}/' obd_ble.c | grep -q 'nvs_set_blob(h, "addr"' || bad "the address write is not in save_adapter()"
c=$(grep -n -E '^[^/*]*\bsave_adapter\(' obd_ble.c | grep -v 'static void save_adapter' | wc -l | tr -d ' ')
[ "$c" = 1 ] || bad "save_adapter() has $c callers (expected 1)"
awk '/^static void pair_finish\(/,/^}/' obd_ble.c | awk '/OBD_DONE_SAVE\) \{/{s=1} s&&/save_adapter\(/{ok=1} /\} else \{/{s=0} END{exit !ok}' ||
    bad "save_adapter() is not inside pair_finish()'s OBD_DONE_SAVE branch"
grep -q 'save_current' obd_ble.c && bad "the old save_current() (saved at the first ELM prompt) is back"
a=$(grep -l -E '\bobd_ble_pair_answer\(' *.c | tr '\n' ' ')
[ "$a" = "app_main.c mosaico_board.c obd_ble.c " ] || bad "obd_ble_pair_answer() is called from: $a"
grep -q -E 'case MB_BLE_PAIR: +obd_ble_pair_answer\(r->arg != 0\);' mosaico_board.c || bad "mosaico_board.c MB_BLE_PAIR"
awk '/^#if !BOARD_MOSAICO/{m=1} /^#endif/{m=0} /obd_ble_pair_answer\(true\)/{if(!m) bad=1; n++} END{exit bad || n != 1}' app_main.c ||
    bad "app_main.c answers yes outside the car board's BOOT tap"
grep -q 'mosaico_board_request(MB_BLE_PAIR' mosaico_display.c || bad "the pairing sheet does not send its answer"

# 2. A new adapter is not used before the yes: elm_initialise stops right after the reset when it is not the paired one,
#    before the session starts (sdlog_begin_drive) and before any PID request.
awk '/^static bool elm_initialise\(/,/^}/' elm327.c > /tmp/pair_audit_init.$$
l1=$(grep -n 'if (!obd_ble_adapter_confirmed()) return false;' /tmp/pair_audit_init.$$ | cut -d: -f1)
l2=$(grep -n 'sdlog_begin_drive' /tmp/pair_audit_init.$$ | cut -d: -f1)
l3=$(grep -n '"0100"' /tmp/pair_audit_init.$$ | head -1 | cut -d: -f1)
rm -f /tmp/pair_audit_init.$$
[ -n "$l1" ] && [ -n "$l2" ] && [ -n "$l3" ] && [ "$l1" -lt "$l3" ] && [ "$l3" -lt "$l2" ] ||
    bad "elm_initialise: the confirm check must come before the first 0100 and the session ($l1 / $l3 / $l2)"

# 3. The automatic search never replaces or bypasses a saved adapter: app_main's automatic paths call elm_explore_auto(),
#    elm_explore() stays the user's (double-click; the screen's Find OBD), and obd_ble_explore(false) returns before it
#    lifts the lock or clears a skip list.
n=$(grep -c 'elm_explore_auto();' app_main.c)
[ "$n" = 2 ] || bad "app_main.c: $n automatic searches through elm_explore_auto() (expected 2)"
grep -n -E '^[^/*]*\belm_explore\(\)' app_main.c | grep -v 'else elm_explore();' >/dev/null && bad "app_main.c calls the user's search elsewhere"
awk '/^void obd_ble_explore\(bool user\)/,/^}/' obd_ble.c | awk '/if \(!user\) \{/{s=1} s&&/s_explore_until|s_reject|obd_pair_user_search/{bad=1} s&&/return;/{s=0; ok=1} END{exit bad || !ok}' ||
    bad "obd_ble_explore(false) touches the lock or the skip lists"
grep -q 'obd_ble_explore(user)' elm327.c || bad "elm327.c does not pass who started the search"

# 4. The address is never shown or logged: no addr_str() in a log line or an event; it only fills the paired adapter's
#    status (session file).
if grep -n 'addr_str' obd_ble.c | grep -E 'ESP_LOG|sys_event' >/dev/null; then bad "obd_ble.c logs an address:"; grep -n 'addr_str' obd_ble.c | grep -E 'ESP_LOG|sys_event'; fi
k=$(grep -c 'addr_str' obd_ble.c)
[ "$k" = 2 ] || bad "obd_ble.c uses addr_str $k times (expected 2: the paired adapter's status)"
grep -q 'if (pick == OBD_PICK_SAVED) snprintf(g_status.adapter_addr' obd_ble.c || bad "a probed device's address reaches the status"
grep -n 'adapter_addr' display.c mosaico_*.c >/dev/null && bad "a screen shows the adapter address"

# 5. The runlevel's car-link input on the ESP-Mosaico is os_rl_link_input() (logging only).
grep -q 'bool link = os_rl_link_input(g_status.ble);' sys_mode.c || bad "sys_mode.c: the S31 car link is not os_rl_link_input()"

[ $fail = 0 ] && echo "pair audit: save only on yes PASS ($c caller), new adapter unused before yes PASS, automatic search PASS ($n paths), no address in logs PASS, runlevel input PASS"
exit $fail
