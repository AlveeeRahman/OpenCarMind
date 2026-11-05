#include "power_state.h"

power_state_t power_state(bool card, bool saved, int64_t ms_since_write, bool driving, bool uploading)
{
    if (!card) return POWER_NO_CARD;
    if (driving || ms_since_write < POWER_STALE_MS) return POWER_LOGGING;
    if (!saved) return POWER_SAVING;
    if (uploading) return POWER_UPLOADING;
    return POWER_SAFE;
}

const char *power_state_text(power_state_t s)
{
    switch (s) {
    case POWER_SAFE:      return "Safe to turn off";
    case POWER_NO_CARD:   return "No SD card";
    case POWER_LOGGING:   return "Not safe: logging";
    case POWER_SAVING:    return "Not safe: saving";
    case POWER_UPLOADING: return "Not safe: upload";
    }
    return "";
}
