#include "monitor.h"

#include <X11/extensions/Xinerama.h>

#include "bar.h"
#include "ewmh.h"
#include "layout.h"
#include "state.h"

/* ---- monitors (Xinerama; fallback: whole screen) ---- */
void initmons(void) {
    nmons = 0;
    if (XineramaIsActive(dpy)) {
        int n = 0;
        XineramaScreenInfo *info = XineramaQueryScreens(dpy, &n);
        if (info) {
            for (int i = 0; i < n && nmons < MAXMONS; i++) {
                int dup = 0;
                for (int j = 0; j < nmons; j++)
                    if (mons[j].x == info[i].x_org && mons[j].y == info[i].y_org &&
                        mons[j].w == info[i].width && mons[j].h == info[i].height) dup = 1;
                if (!dup) {
                    mons[nmons].x = info[i].x_org;
                    mons[nmons].y = info[i].y_org;
                    mons[nmons].w = info[i].width;
                    mons[nmons].h = info[i].height;
                    nmons++;
                }
            }
            XFree(info);
        }
    }
    if (nmons == 0) {
        mons[0].x = 0; mons[0].y = 0; mons[0].w = sw; mons[0].h = sh;
        nmons = 1;
    }
    barw = mons[0].w;
}
int mon_at(int x, int y) {
    for (int i = 0; i < nmons; i++)
        if (x >= mons[i].x && x < mons[i].x + mons[i].w &&
            y >= mons[i].y && y < mons[i].y + mons[i].h) return i;
    return 0;
}
int mon_by_pointer(void) {
    Window r, c;
    int x, y, wx, wy;
    unsigned m;
    if (XQueryPointer(dpy, root, &r, &c, &x, &y, &wx, &wy, &m))
        return mon_at(x, y);
    return 0;
}
/* usable area of monitor m: minus bar (mon 0), struts, and outer gaps.
 * BAR/gaps are raw config values scaled by ui_scale; struts stay physical. */
void getarea(int m, int *ax, int *ay, int *aw, int *ah) {
    int o = gaps_on ? S(gap_outer) : 0;
    int top = (m == 0 && bar_on) ? S(BAR_H) : 0;
    if (mon_struts[m].top > top) top = mon_struts[m].top;
    int bot = mon_struts[m].bottom;
    int left = mon_struts[m].left;
    int right = mon_struts[m].right;
    *ax = mons[m].x + left + o;
    *ay = mons[m].y + top + o;
    *aw = mons[m].w - left - right - 2 * o;
    *ah = mons[m].h - top - bot - 2 * o;
    if (*aw < 50) *aw = 50;
    if (*ah < 50) *ah = 50;
}
/* Re-read monitors + refresh layout. Idempotent: no-op when geometry
 * is unchanged (RandR fires bursts on a single replug). */
void on_monitors_changed(void) {
    int ox[MAXMONS], oy[MAXMONS], ow[MAXMONS], oh[MAXMONS];
    int on = nmons;
    for (int i = 0; i < on; i++) { ox[i] = mons[i].x; oy[i] = mons[i].y; ow[i] = mons[i].w; oh[i] = mons[i].h; }
    sw = DisplayWidth(dpy, screen);
    sh = DisplayHeight(dpy, screen);
    initmons();
    int same = (nmons == on);
    if (same)
        for (int i = 0; i < nmons; i++)
            if (mons[i].x != ox[i] || mons[i].y != oy[i] || mons[i].w != ow[i] || mons[i].h != oh[i]) { same = 0; break; }
    if (same) return;
    for (Client *c = clients; c; c = c->next) {
        if (c->mon < 0 || c->mon >= nmons) c->mon = mon_at(c->fx + c->fw / 2, c->fy + c->fh / 2);
        if (c->floating) { /* keep floating windows on-screen */
            if (c->fx < mons[c->mon].x) c->fx = mons[c->mon].x;
            if (c->fy < mons[c->mon].y) c->fy = mons[c->mon].y;
            if (c->fx + c->fw > mons[c->mon].x + mons[c->mon].w)
                c->fx = mons[c->mon].x + mons[c->mon].w - c->fw;
            if (c->fy + c->fh > mons[c->mon].y + mons[c->mon].h)
                c->fy = mons[c->mon].y + mons[c->mon].h - c->fh;
            if (c->fx < mons[c->mon].x) c->fx = mons[c->mon].x;
            if (c->fy < mons[c->mon].y) c->fy = mons[c->mon].y;
        }
    }
    if (bar) XMoveResizeWindow(dpy, bar, mons[0].x, mons[0].y, (unsigned)barw, (unsigned)S(BAR_H));
    bar_style(); /* rebuilds pixmap at new barw */
    update_struts();
    arrange();
}

/* Bounding box of all monitors in root coords. Falls back to 0,0,sw,sh
 * before initmons() (mismatched origins are what this fixes). */
void screen_extents(long *x0, long *y0, long *x1, long *y1) {
    if (nmons <= 0) { *x0 = 0; *y0 = 0; *x1 = sw; *y1 = sh; return; }
    *x0 = mons[0].x; *y0 = mons[0].y;
    *x1 = mons[0].x + mons[0].w; *y1 = mons[0].y + mons[0].h;
    for (int i = 1; i < nmons; i++) {
        if (mons[i].x < *x0) *x0 = mons[i].x;
        if (mons[i].y < *y0) *y0 = mons[i].y;
        if (mons[i].x + mons[i].w > *x1) *x1 = mons[i].x + mons[i].w;
        if (mons[i].y + mons[i].h > *y1) *y1 = mons[i].y + mons[i].h;
    }
    /* before initmons() mons are zeroed: fall back to the screen size */
    if (*x1 <= *x0) { *x0 = 0; *x1 = sw; }
    if (*y1 <= *y0) { *y0 = 0; *y1 = sh; }
}
