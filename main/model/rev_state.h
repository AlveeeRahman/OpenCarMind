/* The R.E.V learned state on the SD card (/sdcard/obd/rev_state.bin). Pure C, host-tested.
 *
 * Layout, little-endian, version 1:
 *   header  { magic "REV1", u16 version, u16 nf, u32 prior_id, u32 sizes of the fuel, physics, picker and calibration sections }
 *   fuel    w_live[nf], A[nf][nf] (upper triangle used), b[nf], commits, resets, kept, failed, learned   (only the statistics)
 *   physics rev_phys_t k values and sums (the 5 s window is not saved)
 *   picker  rev_pick_t
 *   cal     rev_cal_t (ledger, fill-ups, c, lifetime totals)
 *   u32 CRC-32 (IEEE) of everything before it
 * The sections are written as the firmware's own structs, so a change to one of them changes its size and the file is
 * started fresh rather than misread. prior_id is the first 4 bytes of the R.E.V model's sha256: a new model has a new z
 * space, so only the fuel head is dropped; physics, picker and calibration do not depend on z and are kept.
 *
 * Save: write path.tmp, flush, fsync, remove path, rename (FAT's rename does not replace). Load: path, else path.tmp (a power
 * cut between remove and rename leaves only the .tmp), each checked for magic, version, sizes, CRC and finite values. */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "rev_learn.h"
#include "rev_cal.h"

typedef enum { REV_LOAD_NONE = 0, REV_LOAD_OK, REV_LOAD_TMP, REV_LOAD_OTHER_MODEL } rev_load_t;

uint32_t   rev_crc32(uint32_t crc, const void *data, size_t n);
/* fuel may be NULL (no head to save). */
bool       rev_state_save(const char *path, const rev_fuel_t *fuel, const rev_phys_t *phys, const rev_pick_t *pick,
                          const rev_cal_t *cal, uint32_t prior_id);
/* On REV_LOAD_NONE nothing is changed. On REV_LOAD_OTHER_MODEL the fuel head is left as it was (the caller's prior).
   The fuel head's w0 pointer is kept; fuel may be NULL to skip it. */
rev_load_t rev_state_load(const char *path, rev_fuel_t *fuel, rev_phys_t *phys, rev_pick_t *pick, rev_cal_t *cal,
                          uint32_t prior_id);

/* A small learner's state in a file of its own (the tank learner: /sdcard/obd/rev_tank.bin), so adding a learner does not
 * change rev_state.bin's layout. Layout: u32 magic, u32 size, the struct, u32 CRC-32. Same tmp + fsync + rename on save; on
 * load path, else path.tmp, each checked for magic, size and CRC. Returns false and leaves *data unchanged on any failure. */
bool rev_blob_save(const char *path, uint32_t magic, const void *data, uint32_t size);
bool rev_blob_load(const char *path, uint32_t magic, void *data, uint32_t size);
