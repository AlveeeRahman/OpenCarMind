#pragma once
/* Session numbers on the flash log store (no IDF dependency, host-tested in tests/host/test_session_next.c).
 *
 * The session counter lives in NVS ("obdlog"/"session"). Writing a merged image erases NVS, so after a re-flash the counter
 * starts again at 1 while the drive logs of the earlier numbers are still in the storage partition, and the next session
 * would reopen s000001.csv with "w" and overwrite an old drive. The next number is therefore one past the larger of the
 * NVS counter and the highest session number found among the files on the storage. */
#include <stdint.h>

/* "s" + 6 digits + "." + any suffix (.csv, .ml.csv, .json, .json.tmp, .sys.txt, ...) -> the number; anything else -> 0 */
uint32_t session_id_of_name(const char *name);

/* Highest session number among the files in dir (0 when there are none or dir cannot be opened). */
uint32_t session_highest_in_dir(const char *dir);

/* max(nvs_last, highest_on_storage) + 1 */
uint32_t session_next_id(uint32_t nvs_last, uint32_t highest_on_storage);
