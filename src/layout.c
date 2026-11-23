#include "layout.h"
#include "overview.h"
#include "keys.h"
#include "animation-lifecycle.h"
#include "layout-animation.h"

#include <stdint.h>
#include <X11/Xatom.h>

#include "bar.h"
#include "client.h"
#include "ewmh.h"
#include "monitor.h"
#include "state.h"
#include "xerr.h"

/* Raise floating/fullscreen clients above tiled ones while preserving
 * their current relative stacking (XQueryTree returns bottom->top).
 * Do not raise each float bottom->top: even though the final order matches,
 * intermediate inversions generate EnterNotify events under a stationary
 * pointer. In monocle, hover focus calls arrange(), creating a focus loop.
 * Raise only the highest float, then stack the others below it top->bottom
 * without ever reversing the relative order of overlapping floats. */
static void raise_floats(int mon) { /* mon < 0: all monitors */
    Window r, p, *kids = NULL;
    unsigned nk = 0;
    trap_errors(dpy);
    if (XQueryTree(dpy, root, &r, &p, &kids, &nk) && kids) {
        unsigned nf = 0;
        for (unsigned i = 0; i < nk; i++) {
            Client *c = find(kids[i]);
            if (!c || c->ws != curws) continue;
            if (mon >= 0 && c->mon != mon) continue;
            if (!c->floating && !c->fullscreen) continue;
            kids[nf++] = c->win;
        }
        for (unsigned i = 0; i < nf / 2; i++) {
            Window tmp = kids[i];
            kids[i] = kids[nf - 1 - i];
            kids[nf - 1 - i] = tmp;
        }
        if (nf > 0) {
            XRaiseWindow(dpy, kids[0]);
            if (nf > 1) XRestackWindows(dpy, kids, (int)nf);
        }
    }
    if (kids) XFree(kids);
    /* On query failure, leave stacking alone rather than guess its order. */
    untrap_errors(dpy);
}

/* ---- layouts (per monitor) ---- */
void tile_mon(int m) {
    int n = 0;
    for (Client *c = clients; c; c = c->next)
        if (c->ws == curws && c->mon == m && !c->floating && !c->fullscreen) n++;
    if (n == 0) return;
    int ax, ay, aw, ah;
    getarea(m, &ax, &ay, &aw, &ah);
    int g = gaps_on ? S(gap_inner) : 0;
    int nm = NMASTER < n ? NMASTER : n;
    int mw = (n > nm) ? (int)((float)aw * MFACT) : aw;
    /* per-client height weights (cfact, like dwm): master/stack sums */
    float mtotal = 0, stotal = 0;
    {
        int k = 0;
        for (Client *c = clients; c; c = c->next) {
            if (c->ws != curws || c->mon != m || c->floating || c->fullscreen) continue;
            float f = c->cfact < 0.1f ? 1.0f : c->cfact;
            if (f < 0.25f) f = 0.25f;
            if (f > 4.0f) f = 4.0f;
            if (k < nm) mtotal += f;
            else stotal += f;
            k++;
        }
        if (mtotal < 0.1f) mtotal = (float)(nm > 0 ? nm : 1);
        if (n > nm && stotal < 0.1f) stotal = (float)(n - nm);
    }
    float mrem = mtotal, srem = stotal;
    int i = 0, my = ay, sy = ay;
    int bw = S(BORDER);
    for (Client *c = clients; c; c = c->next) {
        if (c->ws != curws || c->mon != m || c->floating || c->fullscreen) continue;
        if (i < nm) {
            float f = c->cfact < 0.1f ? 1.0f : c->cfact;
            if (f < 0.25f) f = 0.25f;
            if (f > 4.0f) f = 4.0f;
            int h;
            if (i == nm - 1) h = (ay + ah - my);
            else { h = (int)((float)(ay + ah - my) * f / mrem); mrem -= f; }
            int ww = mw - 2 * bw - g, wh = h - 2 * bw - g;
            if (ww < 1) ww = 1;
            if (wh < 1) wh = 1;
            XMoveResizeWindow(dpy, c->win,
                ax + g / 2, my + g / 2,
                (unsigned)ww, (unsigned)wh);
            my += h;
        } else {
            int ns = n - nm, si = i - nm;
            float f = c->cfact < 0.1f ? 1.0f : c->cfact;
            if (f < 0.25f) f = 0.25f;
            if (f > 4.0f) f = 4.0f;
            int h;
            if (si == ns - 1) h = (ay + ah - sy);
            else { h = (int)((float)(ay + ah - sy) * f / srem); srem -= f; }
            int ww = aw - mw - 2 * bw - g, wh = h - 2 * bw - g;
            if (ww < 1) ww = 1;
            if (wh < 1) wh = 1;
            XMoveResizeWindow(dpy, c->win,
                ax + mw + g / 2, sy + g / 2,
                (unsigned)ww, (unsigned)wh);
            sy += h;
        }
        XMapWindow(dpy, c->win);
        i++;
    }
}
void tile(void) {
    for (int m = 0; m < nmons; m++) tile_mon(m);
    /* tiling: all windows visible, clear any monocle-set hidden state */
    for (Client *c = clients; c; c = c->next)
        if (c->ws == curws && !c->floating && !c->fullscreen && c->hidden) {
            c->hidden = 0; ewmh_update_state(c);
        }
}
void monocle_mon(int m) {
    int ax, ay, aw, ah;
    getarea(m, &ax, &ay, &aw, &ah);
    Client *show = NULL;
    if (sel && sel->ws == curws && sel->mon == m &&
        !sel->floating && !sel->fullscreen) show = sel;
    /* Focusing a float (e.g. scratchpad) must keep the current monocle
     * window underneath it, rather than switching back to the list head. */
    if (!show)
        for (Client *c = clients; c; c = c->next)
            if (c->ws == curws && c->mon == m && !c->floating &&
                !c->fullscreen && !c->hidden) { show = c; break; }
    if (!show)
        for (Client *c = clients; c; c = c->next)
            if (c->ws == curws && c->mon == m && !c->floating && !c->fullscreen) { show = c; break; }
    for (Client *c = clients; c; c = c->next) {
        if (c->ws != curws || c->mon != m || c->floating || c->fullscreen) continue;
        if (c == show) {
            int g = gaps_on ? S(gap_inner) : 0;
            int ww = aw - 2 * S(BORDER) - g, wh = ah - 2 * S(BORDER) - g;
            if (ww < 1) ww = 1; /* never wrap to a huge unsigned size (#8) */
            if (wh < 1) wh = 1;
            XMoveResizeWindow(dpy, c->win,
                ax + g / 2, ay + g / 2, (unsigned)ww, (unsigned)wh);
            XMapWindow(dpy, c->win);
            if (c->hidden) { c->hidden = 0; ewmh_update_state(c); }
        } else {
            overview_capture(c->win);
            /* Hide reason only on a new hide: it clears pending close fades. */
            if (!c->hidden) {
                animation_unmap(c->win, ANIMATION_HIDE_MONOCLE);
                c->hidden = 1; ewmh_update_state(c);
            } else XUnmapWindow(dpy, c->win);
        }
    }
    for (Client *c = clients; c; c = c->next)
        if (c->ws == curws && c->mon == m && c->floating) XMapWindow(dpy, c->win);
    raise_floats(m);
}
void monocle(void) {
    for (int m = 0; m < nmons; m++) monocle_mon(m);
}
/* Coalesced arrange (audit-0930 #7): client-triggered paths (Configure-
 * Request, known remap) only mark dirty; the event loop flushes once the
 * queue drains, so a request storm costs one relayout, not one per event.
 * A direct arrange() (user actions) cancels any pending one. */
static int arrange_dirty;
static int layout_animation_blocked;
static uint32_t layout_animation_sequence;
typedef struct {
    Window id;
    int x, y, w, h;
} LayoutOld;
static LayoutOld layout_old[LAYOUT_ANIMATION_MAX_CLIENTS];
static unsigned layout_old_count;

void layout_animation_suppress(int suppressed) { layout_animation_blocked = suppressed != 0; }

/* Snapshot only the old positions locally. The later property is advisory:
 * the compositor receives it after the WM's geometry requests and uses the
 * ConfigureNotify-updated position as the target. No WM/compositor
 * acknowledgment or server grab is used. */
static void layout_animation_capture_old(void) {
    layout_old_count = 0;
    if (layout_animation_blocked || !checkwin || !COMPOSITOR || !ANIMATIONS ||
        REDUCE_MOTION || ANIMATION_LAYOUT_MS <= 0 || LAYOUT == L_MONOCLE) return;
    for (Client *c = clients; c && layout_old_count < LAYOUT_ANIMATION_MAX_CLIENTS; c = c->next) {
        XWindowAttributes a;
        if (c->ws != curws || c->floating || c->fullscreen || c->hidden ||
            !XGetWindowAttributes(dpy, c->win, &a) || a.map_state != IsViewable) continue;
        layout_old[layout_old_count++] = (LayoutOld){c->win, a.x, a.y, a.width, a.height};
    }
}
static void layout_animation_publish(void) {
    unsigned long words[LAYOUT_ANIMATION_MAX_WORDS];
    unsigned count = 0;
    if (!layout_old_count || !checkwin || !COMPOSITOR || !ANIMATIONS ||
        REDUCE_MOTION || ANIMATION_LAYOUT_MS <= 0) return;
    /* Publish the old geometry after the final XMoveResizeWindow batch. The
     * compositor observes the target through ConfigureNotify and can still
     * retain its already-painted picture. There is deliberately no XSync,
     * acknowledgment wait or server grab here. */
    for (unsigned i = 0; i < layout_old_count; i++) {
        LayoutOld *o = &layout_old[i];
        unsigned long *q = words + LAYOUT_ANIMATION_HEADER_WORDS + count * LAYOUT_ANIMATION_ENTRY_WORDS;
        q[0] = o->id; q[1] = (unsigned long)o->x; q[2] = (unsigned long)o->y;
        q[3] = (unsigned long)o->w; q[4] = (unsigned long)o->h;
        count++;
    }
    if (!count) { layout_old_count = 0; return; }
    if (++layout_animation_sequence == 0) ++layout_animation_sequence;
    words[0] = LAYOUT_ANIMATION_VERSION;
    words[1] = (unsigned long)checkwin;
    words[2] = (unsigned long)layout_animation_sequence;
    words[3] = (unsigned long)ANIMATION_LAYOUT_MS;
    words[4] = count;
    Atom prop = XInternAtom(dpy, LAYOUT_ANIMATION_ATOM, False);
    XChangeProperty(dpy, root, prop, XA_CARDINAL, 32, PropModeReplace,
        (unsigned char *)words,
        (int)(LAYOUT_ANIMATION_HEADER_WORDS + count * LAYOUT_ANIMATION_ENTRY_WORDS));
    XFlush(dpy);
    layout_old_count = 0;
}
void arrange_later(void) { arrange_dirty = 1; }
void arrange_flush(void) {
    if (arrange_dirty) arrange();
}
void arrange(void) {
    arrange_dirty = 0;
    layout_animation_capture_old();
    if (LAYOUT == L_MONOCLE) monocle();
    else tile();
    layout_animation_publish();
    for (int m = 0; m < nmons; m++) {
        int ax, ay, aw, ah;
        getarea(m, &ax, &ay, &aw, &ah);
        for (Client *c = clients; c; c = c->next) {
            if (c->ws != curws || c->mon != m) continue;
            if (c->fullscreen) { /* overlay: whole monitor, covers bar */
                XSetWindowBorderWidth(dpy, c->win, 0);
                XMoveResizeWindow(dpy, c->win, mons[m].x, mons[m].y, (unsigned)mons[m].w, (unsigned)mons[m].h);
                XMapWindow(dpy, c->win);
                XRaiseWindow(dpy, c->win);
                continue;
            }
            if (c->floating) {
                XMapWindow(dpy, c->win);
            }
            XSetWindowBorderWidth(dpy, c->win, (unsigned)S(BORDER));
            XSetWindowBorder(dpy, c->win, (c == sel) ? BORDER_FOCUS : BORDER_NORMAL);
        }
    }
    raise_floats(-1); /* floats above tiled, keeping their stacking */
    for (Dock *d = docks; d; d = d->next)
        XRaiseWindow(dpy, d->win);
    for (Client *c = clients; c; c = c->next) {
        if (c->ws == curws && c->fullscreen)
            XRaiseWindow(dpy, c->win);
    }
    overview_top(); /* Structural restacks must not cover the keyboard grab UI. */
    XSync(dpy, False);
    drawbar();
}
