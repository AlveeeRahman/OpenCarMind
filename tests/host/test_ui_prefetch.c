/* Host tests for main/ui_prefetch.c: when the press-time prefetch builds, commits and cancels. Build: make prefetch */
#include "ui_prefetch.h"
#include <stdio.h>

static int fails, checks;
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)

int main(void)
{
    upf_t p = { 0 };
    int pre;
    /* 1. a tap on an app icon: built after the delay, used by the click */
    CHECK(upf_press(&p, UPF_K_APP, 3, 1000, 200, 200, &pre) == UPF_NONE && pre == UPF_NONE);
    CHECK(upf_pressing(&p, 1016, 201, 200, false) == UPF_NONE);
    CHECK(upf_pressing(&p, 1032, 203, 202, false) == UPF_BUILD);
    CHECK(upf_pressing(&p, 1048, 203, 202, false) == UPF_NONE);          /* once */
    CHECK(upf_click(&p, UPF_K_APP, 3) == UPF_COMMIT && upf_was_ready(&p));
    CHECK(upf_settle(&p) == UPF_NONE && !upf_was_ready(&p));
    /* 2. the press starts a launcher swipe before the delay: nothing is built */
    upf_press(&p, UPF_K_APP, 3, 2000, 300, 200, &pre);
    CHECK(upf_pressing(&p, 2016, 270, 200, false) == UPF_NONE && p.st == UPF_IDLE);
    CHECK(upf_pressing(&p, 2032, 200, 200, true) == UPF_NONE && upf_scroll_begin(&p) == UPF_NONE);
    CHECK(upf_click(&p, UPF_K_APP, 3) == UPF_NONE);
    /* 3. built, then the finger drags: the scroll cancels it */
    upf_press(&p, UPF_K_APP, 5, 3000, 100, 300, &pre);
    CHECK(upf_pressing(&p, 3030, 100, 300, false) == UPF_BUILD);
    CHECK(upf_pressing(&p, 3046, 60, 300, true) == UPF_CANCEL && p.st == UPF_IDLE);
    /* 4. built, released elsewhere (no click): the settle cancels it */
    upf_press(&p, UPF_K_APP, 5, 4000, 100, 300, &pre);
    upf_pressing(&p, 4030, 100, 300, false);
    CHECK(upf_settle(&p) == UPF_CANCEL);
    /* 5. built for one app, the click opens another: cancel, the caller builds the right one */
    upf_press(&p, UPF_K_APP, 5, 5000, 100, 300, &pre);
    upf_pressing(&p, 5030, 100, 300, false);
    CHECK(upf_click(&p, UPF_K_APP, 6) == UPF_CANCEL && !upf_was_ready(&p));
    /* 6. Back builds at the press and is used by its click */
    CHECK(upf_press(&p, UPF_K_BACK, 0x0301, 6000, 40, 100, &pre) == UPF_BUILD);
    CHECK(upf_click(&p, UPF_K_BACK, 0x0301) == UPF_COMMIT && upf_was_ready(&p));
    upf_settle(&p);
    /* 7. arm A0 (kind 0): never anything */
    CHECK(upf_press(&p, 0, 3, 7000, 200, 200, &pre) == UPF_NONE);
    CHECK(upf_pressing(&p, 7100, 200, 200, false) == UPF_NONE && upf_click(&p, UPF_K_APP, 3) == UPF_NONE && upf_settle(&p) == UPF_NONE);
    /* 8. a new press while a target is still built cancels it first */
    upf_press(&p, UPF_K_BACK, 1, 8000, 40, 100, &pre);
    CHECK(upf_press(&p, UPF_K_APP, 2, 8100, 200, 200, &pre) == UPF_NONE && pre == UPF_CANCEL);
    /* 9. a tap shorter than the delay: the click comes while only armed, the normal path builds (not ready) */
    upf_press(&p, UPF_K_APP, 4, 9000, 200, 200, &pre);
    upf_pressing(&p, 9016, 200, 200, false);
    CHECK(upf_click(&p, UPF_K_APP, 4) == UPF_NONE && !upf_was_ready(&p));
    /* 10. at most one target: builds - commits - cancels = 0 when idle */
    CHECK(p.st == UPF_IDLE && p.builds == p.commits + p.cancels);
    printf("ui_prefetch: %d checks, %d failures (builds %lu, commits %lu, cancels %lu)\n", checks, fails,
           (unsigned long)p.builds, (unsigned long)p.commits, (unsigned long)p.cancels);
    return fails != 0;
}
