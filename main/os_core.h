/* os_core: the pure rules of the mossREV OS layer (ESP-Mosaico, mossREV 2.0.0). No IDF, no heap, no globals that
 * change: host-tested in tests/host/test_os.c. The firmware glue that acts on these rules is os.c (S31 only).
 *
 * 1. Resets. Every reset somebody or something asks for goes through sys_reset(reason) (os.h). The plan for a reason
 *    is a fixed sequence, always in this order:
 *      a. wait pre_ms, so a reply or a log line gets out (0 where the caller already waited),
 *      b. write the marker: "asked for" (the next boot does not count it toward safe mode) or nothing (a crash path:
 *         the stall guard and the monitor's restart must count),
 *      c. quiesce storage: the NOR FAT, then the NAND (sdlog_quiesce_for_reset),
 *      d. wait post_us (busy wait: the door runs in an esp_timer callback),
 *      e. reset with the reason's method.
 *    The marker comes first so that a hang inside the quiesce, which the task watchdog then resets, still reads as
 *    the reset that was asked for.
 */
#ifndef OS_CORE_H
#define OS_CORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---------- 1. resets ---------- */
typedef enum {
    SYS_RESET_REBOOT = 0,    /* the door's REBOOT command */
    SYS_RESET_DOOR,          /* the 1200-baud touch: ROM download mode for esptool */
    SYS_RESET_FORMAT,        /* the restart after a damaged NOR volume was formatted */
    SYS_RESET_UI_STALL,      /* the UI tick stopped for 15 s: abort for a core dump */
    SYS_RESET_MONITOR,       /* sched_mon: an I/O job hung past 3 deadlines */
    SYS_RESET_N
} sys_reset_reason_t;

enum { OS_RST_RESTART = 0, OS_RST_ROM_DOWNLOAD, OS_RST_ABORT };
enum { OS_STEP_WAIT = 1, OS_STEP_MARK, OS_STEP_QUIESCE, OS_STEP_SETTLE, OS_STEP_RESET };

typedef struct {
    const char *name;        /* for the boot log: "REBOOT", "door", ... */
    uint8_t     asked_for;   /* 1: write the marker, the next boot does not count it toward safe mode */
    uint8_t     quiesce;     /* 1: flush and lock the NOR FAT and the NAND first */
    uint8_t     method;      /* OS_RST_* */
    uint16_t    pre_ms;      /* step a */
    uint32_t    post_us;     /* step d */
} os_reset_plan_t;

const os_reset_plan_t *os_reset_plan(sys_reset_reason_t r);        /* NULL for an unknown reason */
/* The steps in execution order (OS_STEP_*), steps that do nothing left out. Returns the count (<= 5). */
int os_reset_steps(sys_reset_reason_t r, uint8_t steps[5]);

#ifdef __cplusplus
}
#endif
#endif
