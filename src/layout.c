#include "layout.h"

#include "bar.h"
#include "client.h"
#include "ewmh.h"
#include "monitor.h"
#include "state.h"
#include "xerr.h"

/* Raise floating/fullscreen clients above tiled ones while preserving
 * their current relative stacking (XQueryTree returns bottom->top).
 * Raising in client-list order would freeze list order as stacking order
 * and instantly undo an explicit click-raise (or windowraise request) on
 * a small floating window nested inside a bigger one. */
static void raise_floats(int mon) { /* mon < 0: all monitors */
    Window r, p, *kids = NULL;
    unsigned nk = 0;
    trap_errors(dpy);
    if (XQueryTree(dpy, root, &r, &p, &kids, &nk) && kids) {
        for (unsigned i = 0; i < nk; i++) {
            Client *c = find(kids[i]);
            if (!c || c->ws != curws) continue;
            if (mon >= 0 && c->mon != mon) continue;
            if (!c->floating && !c->fullscreen) continue;
            XMapRaised(dpy, c->win);
        }
        XFree(kids);
        untrap_errors(dpy);
        return;
    }
    /* XQueryTree failed: fall back to list order (better than nothing) */
    for (Client *c = clients; c; c = c->next)
        if (c->ws == curws && (mon < 0 || c->mon == mon) &&
            (c->floating || c->fullscreen))
            XMapRaised(dpy, c->win);
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
    if (!show)
        for (Client *c = clients; c; c = c->next)
            if (c->ws == curws && c->mon == m && !c->floating && !c->fullscreen) { show = c; break; }
    for (Client *c = clients; c; c = c->next) {
        if (c->ws != curws || c->mon != m || c->floating || c->fullscreen) continue;
        if (c == show) {
            int g = gaps_on ? S(gap_inner) : 0;
            XMoveResizeWindow(dpy, c->win,
                ax + g / 2, ay + g / 2,
                (unsigned)(aw - 2 * S(BORDER) - g),
                (unsigned)(ah - 2 * S(BORDER) - g));
            XMapWindow(dpy, c->win);
            if (c->hidden) { c->hidden = 0; ewmh_update_state(c); }
        } else {
            XUnmapWindow(dpy, c->win);
            if (!c->hidden) { c->hidden = 1; ewmh_update_state(c); }
        }
    }
    for (Client *c = clients; c; c = c->next)
        if (c->ws == curws && c->mon == m && c->floating) XMapWindow(dpy, c->win);
    raise_floats(m);
}
void monocle(void) {
    for (int m = 0; m < nmons; m++) monocle_mon(m);
}
void arrange(void) {
    if (LAYOUT == L_MONOCLE) monocle();
    else tile();
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
    XSync(dpy, False);
    drawbar();
}
