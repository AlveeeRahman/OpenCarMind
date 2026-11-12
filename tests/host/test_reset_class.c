/* reset_class.c: which resets count toward the boot-loop safe mode (see main/reset_class.h). */
#include <stdio.h>
#include "reset_class.h"

static int fails, runs;
#define CHECK(c) do { runs++; if (!(c)) { fails++; printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); } } while (0)

int main(void)
{
    /* crashes count */
    CHECK(reset_is_abnormal(RESET_KIND_PANIC, false, false));           /* also the ui-stall guard's abort */
    CHECK(reset_is_abnormal(RESET_KIND_INT_WDT, false, false));
    CHECK(reset_is_abnormal(RESET_KIND_TASK_WDT, false, false));
    CHECK(reset_is_abnormal(RESET_KIND_WDT, false, false));
    CHECK(reset_is_abnormal(RESET_KIND_SW, true, false));               /* the monitor's restart_hung */
    /* nothing else does */
    CHECK(!reset_is_abnormal(RESET_KIND_SW, false, false));             /* esp_restart() without a monitor reason */
    CHECK(!reset_is_abnormal(RESET_KIND_POWERON, false, false));
    CHECK(!reset_is_abnormal(RESET_KIND_USB, false, false));
    CHECK(!reset_is_abnormal(RESET_KIND_DEEPSLEEP, false, false));
    CHECK(!reset_is_abnormal(RESET_KIND_BROWNOUT, false, false));
    CHECK(!reset_is_abnormal(RESET_KIND_OTHER, false, false));
    /* a reset somebody asked for never counts, whatever the chip reports: the door flash reads as a panic */
    for (int k = RESET_KIND_POWERON; k <= RESET_KIND_OTHER; k++) {
        CHECK(!reset_is_abnormal((reset_kind_t)k, false, true));
        CHECK(!reset_is_abnormal((reset_kind_t)k, true, true));
    }
    /* three door flashes in a row, then a real panic: only the panic counts */
    int count = 0;
    for (int i = 0; i < 3; i++) count = reset_is_abnormal(RESET_KIND_PANIC, false, true) ? count + 1 : 0;
    CHECK(count == 0);
    count = reset_is_abnormal(RESET_KIND_PANIC, false, false) ? count + 1 : 0;
    CHECK(count == 1);
    printf("reset_class: %d checks, %d failures\n", runs, fails);
    return fails != 0;
}
