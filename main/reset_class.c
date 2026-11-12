/* Reset classification for the boot-loop safe mode: see reset_class.h. */
#include "reset_class.h"

bool reset_is_abnormal(reset_kind_t kind, bool monitor_asked, bool intentional)
{
    if (intentional) return false;
    switch (kind) {
    case RESET_KIND_PANIC:
    case RESET_KIND_INT_WDT:
    case RESET_KIND_TASK_WDT:
    case RESET_KIND_WDT:
        return true;
    case RESET_KIND_SW:
        return monitor_asked;
    default:
        return false;
    }
}
