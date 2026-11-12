#!/bin/sh
# Source audit for the mossREV OS layer (run by `make os`): rules that a unit test cannot see.
# 1. Resets: only os.c (and the reset-injection test, storage_rt.c) may call the reset primitives. In files shared with
#    the car board (S3), an esp_restart() must follow an OCM_SYS_RESET(...) or sys_reset(...) on the same or the line
#    before, so on the S31 it is never reached.
cd "$(dirname "$0")/../../main" || exit 1
fail=0
for f in *.c *.cpp; do
    case "$f" in os.c|storage_rt.c|display.c) continue ;; esac   # display.c: S3 only
    if grep -n -E '^[^/*]*\b(esp_rom_software_reset_system|esp_system_abort)\(' "$f" >/dev/null; then
        echo "FAIL $f calls a reset primitive directly:"; grep -n -E '^[^/*]*\b(esp_rom_software_reset_system|esp_system_abort)\(' "$f"; fail=1
    fi
    awk -v f="$f" '
        { incom = c; t = $0; if (t ~ /\/\*/ && t !~ /\*\//) c = 1; else if (t ~ /\*\//) c = 0 }
        !incom && /^[^\/*]*esp_restart\(\)/ && !/register_shutdown/ {
            if ($0 !~ /(OCM_SYS_RESET|sys_reset)\(/ && prev !~ /(OCM_SYS_RESET|sys_reset)\(/ && prev !~ /^#endif/ ) { print "FAIL " f ":" NR ": esp_restart() not behind sys_reset"; bad = 1 }
            if (prev ~ /^#endif/ && prev2 !~ /sys_reset\(/) { print "FAIL " f ":" NR ": esp_restart() after #endif without sys_reset"; bad = 1 }
        }
        { prev2 = prev; prev = $0 }
        END { exit bad }' "$f" || fail=1
done
n=$(grep -l -E '\bsys_reset\(|OCM_SYS_RESET\(' *.c | wc -l | tr -d ' ')
[ "$n" -ge 5 ] || { echo "FAIL only $n files call sys_reset (door, REBOOT, format, stall guard, monitor)"; fail=1; }
# 2. Runlevels: no S31-only file decides a refusal from the drive, the car link or safe mode by itself any more.
for f in mosaico_*.c ui_*.c board_pins_safe.c; do
    if grep -n -E 'if \(!?(elm_drive_active\(\)|sched_safe_mode\(\)|sys_mode_get\(\) == MODE_DRIVE)\)' "$f" >/dev/null; then
        echo "FAIL $f refuses on its own check, not os_allows():"; grep -n -E 'if \(!?(elm_drive_active\(\)|sched_safe_mode\(\)|sys_mode_get\(\) == MODE_DRIVE)\)' "$f"; fail=1
    fi
done
m=$(grep -h -o -E 'os_(allows|refusal|refuse_sheet)\(OS_SVC_[A-Z_]+|OCM_REFUSED\(OS_SVC_[A-Z_]+' *.c *.cpp | sed 's/.*(//' | sort -u | wc -l | tr -d ' ')
[ "$m" -ge 8 ] || { echo "FAIL only $m services are looked up"; fail=1; }
# 3. Feature gate: the thresholds exist only in ui_metrics.c, and no feature runs a test of its own.
t=$(grep -l -E 'define UM_(MIN_PER_ARM|MAX_PER_ARM|READY_PM)|UM_POL_N\] =' *.c *.h | tr '\n' ' ')
[ "$t" = "ui_metrics.c " ] || { echo "FAIL the gate thresholds are defined in: $t"; fail=1; }
if grep -n -E 'um_decide|um_mw_z100|um_holm|um_gate_step|z100|MIN_PER_ARM' swipe_intent.c ui_prefetch.c >/dev/null; then
    echo "FAIL a feature runs its own gate:"; grep -n -E 'um_decide|um_mw_z100|um_holm|um_gate_step|z100|MIN_PER_ARM' swipe_intent.c ui_prefetch.c; fail=1
fi
# 4. Wi-Fi boot hold: app_main starts Wi-Fi only through OCM_WIFI_BOOT_START (the S31 holds it), the held start touches
#    no netif, driver or task, the automatic start asks the verdict (wifi_up(false) only in wifi_sync_boot_release, which
#    os.c calls only behind os_wifi_may_start(v, false)), and the screen's three owner paths use wifi_sync_up().
if grep -n -E '^[^/*]*\bwifi_sync_start\(\)' app_main.c | grep -v 'OCM_WIFI_BOOT_START(wifi_sync_start())' >/dev/null; then
    echo "FAIL app_main.c starts Wi-Fi without the boot hold:"; grep -n 'wifi_sync_start()' app_main.c; fail=1
fi
h=$(awk '/^esp_err_t wifi_sync_start_held\(void\)/,/^}/' wifi_sync.c)
[ -n "$h" ] || { echo "FAIL wifi_sync_start_held() not found"; fail=1; }
if echo "$h" | grep -E 'esp_wifi_|esp_netif_|esp_event_|xTaskCreate|s31_task_up|radio_' >/dev/null; then
    echo "FAIL the held Wi-Fi start brings something up at boot:"; echo "$h" | grep -E 'esp_wifi_|esp_netif_|esp_event_|xTaskCreate|s31_task_up|radio_'; fail=1
fi
w=$(grep -c -E 'wifi_up\((true|false)\)' wifi_sync.c)
o=$(grep -c -E 'wifi_up\(true\)' wifi_sync.c)
[ "$w" = 2 ] && [ "$o" = 1 ] && grep -q -E '^bool wifi_sync_up\(void\) \{ return wifi_up\(true\); \}' wifi_sync.c || {
    echo "FAIL wifi_up() callers: expected wifi_sync_up -> wifi_up(true) and the release -> wifi_up(false) only"; fail=1; }
grep -q -E 'if \(os_wifi_may_start\(v, false\)\) wifi_sync_boot_release\(\);' os.c || { echo "FAIL os.c releases Wi-Fi without the verdict"; fail=1; }
r=$(grep -l 'wifi_sync_boot_release()' *.c | tr '\n' ' ')
[ "$r" = "os.c wifi_sync.c " ] || { echo "FAIL wifi_sync_boot_release() called from: $r"; fail=1; }
u=$(grep -c 'wifi_sync_up()' mosaico_board.c)
[ "$u" -ge 3 ] || { echo "FAIL the screen's owner paths (sync now, search, join) call wifi_sync_up() $u times"; fail=1; }
[ $fail = 0 ] && echo "os audit: resets PASS ($n files go through sys_reset), runlevels PASS ($m services looked up, no own checks), feature gate PASS (thresholds in ui_metrics.c only), Wi-Fi boot hold PASS (held start brings nothing up, $u owner paths)"
exit $fail
