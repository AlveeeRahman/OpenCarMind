#pragma once
/* ESP-Mosaico drive-log store rules (F1, CONFIG_OCM_NAND_LOGS). No IDF dependency: host-tested in tests/host/test_log_store.c.
 *
 * Two FAT volumes: the 128 MB SPI NAND (Dhara FTL, mounted at CONFIG_OCM_NAND_MOUNT_POINT) holds the drive logs, and the
 * flash FAT in the NOR "storage" partition (CONFIG_SDLOG_MOUNT_POINT) keeps the state files (learned R.E.V state, tank,
 * clock.txt, config.json, wifi.csv, custom_pids.csv, patterns.json). Without a mounted NAND the drive logs stay on the
 * flash FAT as before. Drive logs found on the flash FAT while the NAND is mounted are moved to the NAND once, each file
 * copied, synced, read back and compared before the flash copy is removed (log_store_move_step). */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* A drive-log file: "sNNNNNN." + any suffix (session_id_of_name != 0), or drives.csv. Everything else is a state file. */
bool log_store_is_drive_file(const char *name);

/* Where a file goes: true = the NAND, false = the flash FAT. Drive logs go to the NAND when it is mounted. */
bool log_store_on_nand(const char *name, bool nand_mounted);

/* The next session number: one past the NVS counter and the highest session number on either store. */
uint32_t log_store_next_session(uint32_t nvs_last, uint32_t highest_nand, uint32_t highest_flash);

/* The suffix of a copy in progress on the NAND ("s000012.csv.mv"): removed at the next boot, never listed. */
#define LOG_STORE_MOVE_SUFFIX ".mv"
bool log_store_is_move_tmp(const char *name);

/* One drive file on the flash FAT while the NAND is mounted. */
typedef enum {
    LS_COPY,        /* not on the NAND yet: copy to <name>.mv, sync, compare, rename, then remove the flash copy */
    LS_DROP_FLASH,  /* the NAND already has the same bytes (a reset came between the rename and the remove): remove the flash copy */
    LS_CONFLICT,    /* the NAND has a different file of that name: keep both, the NAND one is listed, nothing is removed */
} ls_move_t;
ls_move_t log_store_move_step(bool on_nand, uint64_t nand_size, uint32_t nand_crc, uint64_t flash_size, uint32_t flash_crc);

/* After the copy: the flash copy may be removed only when the NAND copy was read back with the same size and CRC. */
bool log_store_copy_verified(uint64_t flash_size, uint32_t flash_crc, uint64_t nand_size, uint32_t nand_crc);

/* drives.csv on both stores: one header, then every data row of both files once, ordered by the session number in the
   first column (rows without a number keep their order at the end). Returns the merged length, or -1 when out does not
   hold it (out is then left as an empty string). Either input may be NULL or empty. */
long log_store_merge_drives_csv(const char *a, const char *b, char *out, size_t out_n);
