/* ui_prefetch.c: the press-time prefetch state machine (ui_prefetch.h). */
#include "ui_prefetch.h"

static int drop(upf_t *p)
{
    int a = p->st == UPF_BUILT ? UPF_CANCEL : UPF_NONE;
    if (a == UPF_CANCEL) p->cancels++;
    p->st = UPF_IDLE;
    return a;
}

int upf_press(upf_t *p, uint8_t kind, uint16_t target, uint32_t t, int16_t x, int16_t y, int *pre)
{
    int c = drop(p);
    if (pre) *pre = c;
    p->kind = kind;
    p->target = target;
    p->t_press = t;
    p->x0 = x;
    p->y0 = y;
    if (kind != UPF_K_APP && kind != UPF_K_BACK) return UPF_NONE;
    if (kind == UPF_K_BACK) { p->st = UPF_BUILT; p->builds++; return UPF_BUILD; }
    p->st = UPF_ARMED;
    return UPF_NONE;
}

int upf_pressing(upf_t *p, uint32_t t, int16_t x, int16_t y, bool scrolling)
{
    if (p->st != UPF_ARMED) return scrolling ? upf_scroll_begin(p) : UPF_NONE;
    int dx = x - p->x0, dy = y - p->y0;
    if (scrolling || dx * dx + dy * dy > UPF_SLOP_PX * UPF_SLOP_PX) { p->st = UPF_IDLE; return UPF_NONE; }   /* a drag */
    if (t - p->t_press < UPF_APP_DELAY_MS) return UPF_NONE;
    p->st = UPF_BUILT;
    p->builds++;
    return UPF_BUILD;
}

int upf_scroll_begin(upf_t *p) { return drop(p); }

int upf_click(upf_t *p, uint8_t kind, uint16_t target)
{
    if (p->st == UPF_BUILT && p->kind == kind && p->target == target) { p->st = UPF_COMMITTED; p->commits++; return UPF_COMMIT; }
    return drop(p);
}

int upf_settle(upf_t *p) { return p->st == UPF_COMMITTED ? (p->st = UPF_IDLE, UPF_NONE) : drop(p); }

bool upf_was_ready(const upf_t *p) { return p->st == UPF_COMMITTED; }
