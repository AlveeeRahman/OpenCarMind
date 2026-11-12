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
[ $fail = 0 ] && echo "os audit: resets PASS ($n files go through sys_reset), runlevels PASS ($m services looked up, no own checks), feature gate PASS (thresholds in ui_metrics.c only)"
exit $fail
