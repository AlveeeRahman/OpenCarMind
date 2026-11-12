#!/bin/sh
# Source audit for the pairing safety glue (run by `make guard`): what test_adapter_guard.c cannot see in obd_ble.c,
# elm327.c and wifi_sync.c. Sections 1-7 read the ESP-Mosaico sources (CONFIG_OBD_BLE_PAIR_CONFIRM=1, BOARD_MOSAICO=1),
# section 8 the S3's (both 0).
src="$(cd "$(dirname "$0")/../../main" && pwd)" || exit 1
tmp=$(mktemp -d) || exit 1
trap 'rm -rf "$tmp"' EXIT
mkdir "$tmp/s31" "$tmp/s3"
for f in obd_ble.c elm327.c wifi_sync.c mosaico_console.c; do
    unifdef -DCONFIG_OBD_BLE_PAIR_CONFIRM=1 -DBOARD_MOSAICO=1 "$src/$f" > "$tmp/s31/$f"
    unifdef -DCONFIG_OBD_BLE_PAIR_CONFIRM=0 -DBOARD_MOSAICO=0 "$src/$f" > "$tmp/s3/$f"
done
cd "$tmp/s31" || exit 1
fail=0
bad() { echo "FAIL $*"; fail=1; }

# 1. The fingerprint is written to NVS in fp_store() only, and fp_store() runs on three paths: the first link after the
#    user's yes (LEARN), the user's "Use it", and the protocol joining a trusted record.
w=$(grep -c 'nvs_set_blob(h, "fp"' obd_ble.c)
[ "$w" = 1 ] || bad "obd_ble.c writes the fingerprint in $w places (expected 1)"
awk '/^static void fp_store\(/,/^}/' obd_ble.c | grep -q 'nvs_set_blob(h, "fp"' || bad "the fingerprint write is not in fp_store()"
c=$(grep -n -E '^[^/*]*\bfp_store\(' obd_ble.c | grep -v 'static void fp_store' | wc -l | tr -d ' ')
[ "$c" = 3 ] || bad "fp_store() has $c callers (expected 3)"
awk '/^void obd_ble_guard_elm\(/,/^}/' obd_ble.c | awk '/case AG_R1_LEARN:/{s=1} s&&/fp_store\(/{ok=1} /break;/{s=0} END{exit !ok}' ||
    bad "obd_ble_guard_elm() does not learn in its LEARN case"
awk '/^static void guard_finish\(/,/^}/' obd_ble.c | awk '/k == AG_Q_CHANGED/{c=1} c&&/if \(yes\) \{/{s=1} s&&/fp_store\(/{ok=1} /\} else if \(timeout\)/{s=0} END{exit !ok}' ||
    bad "guard_finish() does not re-learn only on the R1 yes"

# 2. No VIN in the guard: the pure rules never name it, the guard is fed ATI and AT@1 only
grep -i -n -w 'vin' "$src/adapter_guard.c" | grep -v -i 'never\|not a field\|no vin' && bad "adapter_guard.c mentions the VIN"
for fn in obd_ble_guard_elm obd_ble_guard_protocol obd_ble_guard_meta guard_finish fp_from_adv; do
    awk "/^(static )?(void|bool|int) $fn\\(/,/^}/" obd_ble.c | grep -i -q -w 'vin' && bad "$fn() touches the VIN"
done
grep -q 'obd_ble_guard_elm(ati, at1);' elm327.c || bad "elm327.c feeds the guard something else than ATI and AT@1"

# 3. No address in the guard's log lines (pair_audit.sh checks addr_str for the whole file)
awk '/pairing safety \(adapter_guard.h\) \*\/$/,0' obd_ble.c | grep -E 'sys_event|ESP_LOG' | grep -E 'addr|val\[' &&
    bad "a guard log line names an address"

# 4. R2: the event handler goes through the table, and only its ALLOW branch re-pairs
grep -q 'return guard_repeat_pairing();' obd_ble.c || bad "BLE_GAP_EVENT_REPEAT_PAIRING does not ask the guard"
r=$(grep -c 'BLE_GAP_REPEAT_PAIRING_RETRY' obd_ble.c)
[ "$r" = 1 ] || bad "RETRY appears $r times (expected 1, the Allow branch)"
awk '/^static int guard_repeat_pairing\(/,/^}/' obd_ble.c | awk '/AG_R2_ALLOW\) \{/{s=1} s&&/REPEAT_PAIRING_RETRY/{ok=1} /^    \}/{s=0} END{exit !ok}' ||
    bad "RETRY is outside the Allow branch"
grep -q 'guard_after_enc()) return 0;' obd_ble.c || bad "an encryption does not check for a silent re-pair"

# 5. The learner gets nothing from a different-looking adapter: rev_row and pattern_step are behind the check
awk '/if \(!obd_ble_adapter_changed\(\)\)/{getline n; print n}' elm327.c > gated.txt
grep -q 'rev_row(&snap' gated.txt || bad "rev_row() is not behind obd_ble_adapter_changed()"
grep -q 'pattern_step(&snap);' gated.txt || bad "pattern_step() is not behind obd_ble_adapter_changed()"

# 6. The fingerprint is checked before the session starts and before the first PID request
awk '/^static bool elm_initialise\(/,/^}/' elm327.c > init.txt
l1=$(grep -n 'obd_ble_guard_elm(' init.txt | cut -d: -f1)
l2=$(grep -n '"0100"' init.txt | head -1 | cut -d: -f1)
l3=$(grep -n 'sdlog_begin_drive' init.txt | cut -d: -f1)
[ -n "$l1" ] && [ -n "$l2" ] && [ -n "$l3" ] && [ "$l1" -lt "$l2" ] && [ "$l2" -lt "$l3" ] ||
    bad "elm_initialise: the fingerprint check must come before the first 0100 and the session ($l1 / $l2 / $l3)"

# 7. R6: the 2026-2099 range first, then the jump guard, then the clock is set
awk '/^static bool sync_clock\(/,/^}/' wifi_sync.c > sync.txt
a=$(grep -n 'harden_epoch_plausible' sync.txt | cut -d: -f1)
b=$(grep -n 'jump_ok(epoch)' sync.txt | cut -d: -f1)
s=$(grep -n 'settimeofday' sync.txt | cut -d: -f1)
[ -n "$a" ] && [ -n "$b" ] && [ -n "$s" ] && [ "$a" -lt "$b" ] && [ "$b" -lt "$s" ] || bad "sync_clock: range, jump guard, set ($a / $b / $s)"

# 8. The S3 sources compile none of it, and the 1200-baud door has no guard in its path
cd "$tmp/s3" || exit 1
for f in obd_ble.c elm327.c wifi_sync.c; do
    sed -e 's:/\*.*\*/::g' "$f" | grep -n -E 'adapter_guard|guard_|jump_ok|AT@1|afp_|ag_q|ag_r[12]' && bad "s3: $f compiles pairing-safety code (above)"
done
grep -q 'return BLE_GAP_REPEAT_PAIRING_RETRY;' obd_ble.c || bad "s3: the car board's REPEAT_PAIRING handler changed"
grep -n -E 'obd_ble|guard|os_allows|os_refusal' "$tmp/s31/mosaico_console.c" | grep -v '^.*/\*' && bad "the 1200-baud door path calls into the guard or the runlevels"

[ $fail = 0 ] && echo "guard audit: fingerprint write PASS ($c callers), no VIN PASS, no address PASS, R2 table PASS, learner gate PASS, check order PASS, R6 order PASS, S3 untouched PASS"
exit $fail
