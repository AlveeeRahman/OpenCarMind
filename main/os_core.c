/* os_core.c: the pure rules of the mossREV OS layer. See os_core.h. */
#include "os_core.h"
#include <string.h>

/* ---------- 1. resets ----------
   REBOOT and FORMAT: their callers (usb_door.c, sdlog.c, shared with the S3 build) already wait 300 ms, so pre_ms is 0.
   The door's 100 ms after the quiesce is the old mosaico_console_download() delay, kept. The stall guard and the
   monitor's restart are crash paths: no marker, so three of them in a row still reach safe mode. */
static const os_reset_plan_t PLANS[SYS_RESET_N] = {
    [SYS_RESET_REBOOT]    = { "REBOOT",    1, 1, OS_RST_RESTART,      0, 0 },
    [SYS_RESET_DOOR]      = { "door",      1, 1, OS_RST_ROM_DOWNLOAD, 0, 100000 },
    [SYS_RESET_FORMAT]    = { "format",    1, 1, OS_RST_RESTART,      0, 0 },
    [SYS_RESET_UI_STALL]  = { "ui stall",  0, 1, OS_RST_ABORT,        0, 0 },
    [SYS_RESET_MONITOR]   = { "monitor",   0, 1, OS_RST_RESTART,      0, 0 },
};

const os_reset_plan_t *os_reset_plan(sys_reset_reason_t r)
{
    return (unsigned)r < SYS_RESET_N ? &PLANS[r] : NULL;
}

int os_reset_steps(sys_reset_reason_t r, uint8_t steps[5])
{
    const os_reset_plan_t *p = os_reset_plan(r);
    int n = 0;
    if (!p) return 0;
    if (p->pre_ms) steps[n++] = OS_STEP_WAIT;
    if (p->asked_for) steps[n++] = OS_STEP_MARK;
    if (p->quiesce) steps[n++] = OS_STEP_QUIESCE;
    if (p->post_us) steps[n++] = OS_STEP_SETTLE;
    steps[n++] = OS_STEP_RESET;
    return n;
}
