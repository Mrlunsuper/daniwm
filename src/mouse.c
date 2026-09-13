#include "mouse.h"

#include <X11/Xutil.h>
#include <X11/cursorfont.h>

#include <stdlib.h>

#include "client.h"
#include "layout.h"
#include "monitor.h"
#include "state.h"

/* ---- mouse: Mod+Left move, Mod+Right resize ----
 * Passive grabs live on client windows (grabbuttons). Plain Mod+click
 * without motion only focuses. Fullscreen never drags.
 * - Mod+Left move: promotes a tiled window to floating once pointer moves
 *   past a 4px deadzone. Dropping on another monitor re-tiles on that monitor;
 *   same-monitor drop stays floating.
 * - Mod+Right resize: in tiling mode (L_TILE), stays tiled: horizontal drag
 *   resizes mfact (master/stack split); vertical drag is border-following:
 *   the dragged window grows by |dy|/150 and the neighbor on the drag side
 *   shrinks by the same amount (pair cfact sum conserved), so the shared
 *   border tracks the pointer. No neighbor on that side (screen edge) is a
 *   clean no-op: cfact is left untouched. On floating windows or in
 *   monocle mode, resizes the window geometry. */

/* bspwm-style swap drag: tiled window (same ws/mon, not floating/fullscreen)
 * whose area contains (px,py). The dragged window itself is floating by
 * then, so it never matches its own filter — no explicit exclude needed,
 * but `c` is still skipped for safety. First match wins: tiles don't
 * overlap, so any hit is unambiguous. */
static Client *tiled_at(Client *c, int px, int py) {
    for (Client *t = clients; t; t = t->next) {
        XWindowAttributes a;
        if (t == c || t->ws != c->ws || t->mon != c->mon ||
            t->floating || t->fullscreen) continue;
        if (!XGetWindowAttributes(dpy, t->win, &a)) continue;
        if (px >= a.x && px < a.x + a.width && py >= a.y && py < a.y + a.height)
            return t;
    }
    return NULL;
}
/* drop-target highlight: border widens by 2px (color-agnostic, theme-safe).
 * arrange() resets widths on the next layout, drag_end() restores live. */
static void swap_highlight(Window w, int on) {
    Client *c = (w == None) ? NULL : find(w);
    int bw;
    if (!c) return;
    bw = S(BORDER);
    XSetWindowBorderWidth(dpy, c->win, (unsigned)(on ? bw + 2 : bw));
    XFlush(dpy);
}
/* position of c among tiled windows of its (ws, mon): index, count, nmaster */
static int tiled_pos(Client *c, int *np, int *nmp) {
    int n = 0, idx = -1;
    for (Client *t = clients; t; t = t->next)
        if (t->ws == c->ws && t->mon == c->mon && !t->floating && !t->fullscreen) {
            if (t == c) idx = n;
            n++;
        }
    int nm = NMASTER < n ? NMASTER : n;
    if (np) *np = n;
    if (nmp) *nmp = nm;
    return idx;
}
void grabbuttons(Client *c) {
    unsigned int masks[] = { 0, LockMask, Mod2Mask, LockMask | Mod2Mask };
    XUngrabButton(dpy, AnyButton, AnyModifier, c->win);
    for (unsigned m = 0; m < sizeof(masks) / sizeof(masks[0]); m++)
        for (int b = Button1; b <= Button3; b += 2) /* left + right only */
            XGrabButton(dpy, (unsigned int)b, MOD | masks[m], c->win, False,
                ButtonPressMask, GrabModeAsync, GrabModeAsync, None, None);
}
void drag_start(Client *c, int mode, int px, int py) {
    XWindowAttributes a;
    if (!c || c->fullscreen) return;
    if (!XGetWindowAttributes(dpy, c->win, &a)) return;
    if (cur_move == None) cur_move = XCreateFontCursor(dpy, XC_fleur);
    if (cur_resize == None) cur_resize = XCreateFontCursor(dpy, XC_bottom_right_corner);
    if (cur_hsplit == None) cur_hsplit = XCreateFontCursor(dpy, XC_sb_h_double_arrow);
    Cursor cur = cur_move;
    if (mode == 2)
        cur = (!c->floating && LAYOUT == L_TILE) ? cur_hsplit : cur_resize;
    if (XGrabPointer(dpy, root, False, PointerMotionMask | ButtonReleaseMask,
            GrabModeAsync, GrabModeAsync, None,
            cur, CurrentTime) != GrabSuccess)
        return;
    drag.win = c->win; drag.mode = mode;
    drag.px = px; drag.py = py;
    drag.swap_target = None;
    drag.x = a.x; drag.y = a.y; drag.w = a.width; drag.h = a.height;
    drag.promoted = c->floating;
    drag.tiled0 = !c->floating;
    drag.mon0 = c->mon;
    drag.mfact0 = MFACT;
    drag.cfact0 = (c->cfact < 0.1f) ? 1.0f : c->cfact;
    /* snapshot same-column neighbors + their cfacts at press time, so every
     * motion sets ABSOLUTE pair values (no compounding across events) */
    drag.nb_up = drag.nb_dn = None;
    drag.nb_up0 = drag.nb_dn0 = 1.0f;
    {
        int n, nm, idx = tiled_pos(c, &n, &nm);
        if (idx >= 0) {
            int col0 = (idx < nm) ? 0 : nm;
            int col1 = (idx < nm) ? nm : n;
            int k = 0;
            for (Client *t = clients; t; t = t->next) {
                if (t->ws != c->ws || t->mon != c->mon || t->floating || t->fullscreen) continue;
                if (k == idx - 1 && idx - 1 >= col0) {
                    drag.nb_up = t->win;
                    drag.nb_up0 = t->cfact < 0.1f ? 1.0f : t->cfact;
                }
                if (k == idx + 1 && idx + 1 < col1) {
                    drag.nb_dn = t->win;
                    drag.nb_dn0 = t->cfact < 0.1f ? 1.0f : t->cfact;
                }
                k++;
            }
        }
    }
}
void drag_motion(int px, int py) {
    Client *c;
    int dx, dy;
    if (drag.win == None) return;
    c = find(drag.win);
    if (!c) { drag.win = None; drag.mode = 0; XUngrabPointer(dpy, CurrentTime); return; }
    dx = px - drag.px; dy = py - drag.py;
    int dz = S(4); if (dz < 2) dz = 2;
    if (abs(dx) < dz && abs(dy) < dz) return;

    if (drag.mode == 2 && drag.tiled0 && LAYOUT == L_TILE && c->ws == curws) {
        int ax, ay, aw, ah;
        getarea(c->mon, &ax, &ay, &aw, &ah);
        if (aw < 50) aw = 50;
        if (ah < 50) ah = 50;
        int n, nm, idx = tiled_pos(c, &n, &nm);
        if (n > nm) {
            /* split exists: horizontal moves it (n<=nm fills full width,
             * so mfact would be an invisible mutation: skip it) */
            float new_mfact = drag.mfact0 + (float)dx / (float)aw;
            if (new_mfact < 0.1f) new_mfact = 0.1f;
            if (new_mfact > 0.9f) new_mfact = 0.9f;
            MFACT = new_mfact;
        }
        if (dy == 0) {
            /* pure horizontal drag: restore both vertical neighbors
             * so a prior vertical tweak does not stick asymmetrically */
            Client *opu = (drag.nb_up == None) ? NULL : find(drag.nb_up);
            Client *opd = (drag.nb_dn == None) ? NULL : find(drag.nb_dn);
            if (opu && opu->ws == c->ws && opu->mon == c->mon &&
                !opu->floating && !opu->fullscreen) opu->cfact = drag.nb_up0;
            if (opd && opd->ws == c->ws && opd->mon == c->mon &&
                !opd->floating && !opd->fullscreen) opd->cfact = drag.nb_dn0;
        }
        if (dy != 0 && idx >= 0) {
            Window nbw = (dy > 0) ? drag.nb_dn : drag.nb_up;
            float nb0 = (dy > 0) ? drag.nb_dn0 : drag.nb_up0;
            Client *nb = (nbw == None) ? NULL : find(nbw);
            int nbi = nb ? tiled_pos(nb, NULL, NULL) : -1;
            int ncol0 = (idx < nm) ? 0 : nm, ncol1 = (idx < nm) ? nm : n;
            if (nb && nb->ws == c->ws && nb->mon == c->mon &&
                !nb->floating && !nb->fullscreen &&
                nbi >= ncol0 && nbi < ncol1 && (nbi == idx - 1 || nbi == idx + 1)) {
                /* border-following 1:1: d scales with column total / area height
                 * so the shared border tracks the pointer (old /150 was ~7x
                 * too fast on 1080p and DPI-dependent). */
                float col_total = 0;
                {
                    int k = 0;
                    for (Client *t = clients; t; t = t->next) {
                        if (t->ws != c->ws || t->mon != c->mon || t->floating || t->fullscreen) continue;
                        if (k >= ncol0 && k < ncol1) {
                            float f = t->cfact < 0.1f ? 1.0f : t->cfact;
                            if (f < 0.25f) f = 0.25f;
                            if (f > 4.0f) f = 4.0f;
                            col_total += f;
                        }
                        k++;
                    }
                    if (col_total < 0.5f) col_total = 2.0f;
                }
                float d = ((dy > 0 ? (float)dy : -(float)dy) / (float)ah) * col_total;
                float base = (drag.cfact0 < 0.1f) ? 1.0f : drag.cfact0;
                float fc = base + d, fn = (nb0 < 0.1f ? 1.0f : nb0) - d;
                if (fc < 0.25f) fc = 0.25f;
                if (fc > 4.0f) fc = 4.0f;
                if (fn < 0.25f) fn = 0.25f;
                if (fn > 4.0f) fn = 4.0f;
                c->cfact = fc;
                nb->cfact = fn;
                /* symmetry: restore the neighbor on the idle side to its
                 * baseline so reversing direction does not compound */
                {
                    Window idle_w = (dy > 0) ? drag.nb_up : drag.nb_dn;
                    float idle0 = (dy > 0) ? drag.nb_up0 : drag.nb_dn0;
                    Client *idle = (idle_w == None || idle_w == nbw) ? NULL : find(idle_w);
                    if (idle && idle->ws == c->ws && idle->mon == c->mon &&
                        !idle->floating && !idle->fullscreen) idle->cfact = idle0;
                }
            }
        }
        arrange();
        return;
    }

    if (!drag.promoted) {
        c->floating = 1;
        drag.promoted = 1;
        XRaiseWindow(dpy, c->win);
        keep_docks_on_top();
    }
    if (drag.mode == 2) {
        int w = drag.w + dx, h = drag.h + dy;
        if (w < 50) w = 50;
        if (h < 50) h = 50;
        XResizeWindow(dpy, c->win, (unsigned)w, (unsigned)h);
        XFlush(dpy); /* live feedback: push resizes immediately */
    } else {
        XMoveWindow(dpy, c->win, drag.x + dx, drag.y + dy);
        XFlush(dpy);
        /* live swap-target tracking: highlight the tile under the pointer
         * so the drop preview reads before release (tile layout only) */
        if (drag.tiled0 && LAYOUT == L_TILE && c->ws == curws) {
            Client *t = tiled_at(c, px, py);
            Window tw = t ? t->win : None;
            if (tw != drag.swap_target) {
                swap_highlight(drag.swap_target, 0);
                drag.swap_target = tw;
                swap_highlight(drag.swap_target, 1);
            }
        }
    }
}
void drag_end(int px, int py) {
    Client *c;
    if (drag.win == None) return;
    XUngrabPointer(dpy, CurrentTime);
    c = find(drag.win);
    int mode = drag.mode, tiled0 = drag.tiled0, mon0 = drag.mon0;
    Window swapt = drag.swap_target;
    drag.win = None; drag.mode = 0; drag.swap_target = None;
    if (c) {
        if (mode == 1) {
            int newmon = mon_at(px, py);
            Client *t = NULL;
            /* bspwm swap: dropped onto another tile on the same monitor
             * in tile layout -> exchange tiling positions, both stay tiled.
             * Dropped elsewhere -> legacy behavior (float / cross-mon re-tile). */
            if (tiled0 && newmon == mon0 && LAYOUT == L_TILE && c->ws == curws)
                t = tiled_at(c, px, py);
            if (swapt != None) swap_highlight(swapt, 0);
            c->mon = newmon;
            if (t) {
                swap_order(c, t);
                c->floating = 0;
            } else if (tiled0 && newmon != mon0)
                c->floating = 0; /* carried to another monitor: re-tile there */
        }
        if (c->floating) {
            XWindowAttributes wa;
            if (XGetWindowAttributes(dpy, c->win, &wa)) {
                c->fx = wa.x; c->fy = wa.y; c->fw = wa.width; c->fh = wa.height;
            }
        }
        arrange();
        focus(c);
    }
}
