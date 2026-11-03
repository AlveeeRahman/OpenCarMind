/* Whether the power can be cut now, and if not, why. The screen shows it live on page 3. */
#pragma once
#include <stdbool.h>
#include <stdint.h>

#define POWER_STALE_MS 5000   /* car data counts as stopped this long after the last row */

typedef enum {
    POWER_SAFE = 0,      /* car data has stopped, everything is on the card, nothing is uploading */
    POWER_NO_CARD,       /* nothing can be saved */
    POWER_LOGGING,       /* the car link is up or rows arrived in the last POWER_STALE_MS (the model runs per row) */
    POWER_SAVING,        /* data stopped but the last of it is not on the card yet (at most one save period) */
    POWER_UPLOADING,     /* a pass to the desktop app is running */
} power_state_t;

power_state_t power_state(bool card, bool saved, int64_t ms_since_write, bool driving, bool uploading);
const char   *power_state_text(power_state_t s);   /* short enough for one 160 px line on page 3 */
