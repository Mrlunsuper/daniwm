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
 *   resizes mfact (master/stack split), vertical drag resizes cfact
 *   (window height weight 0.25..4.0); on floating windows or in
 *   monocle mode, resizes the window geometry. */
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
    drag.x = a.x; drag.y = a.y; drag.w = a.width; drag.h = a.height;
    drag.promoted = c->floating;
    drag.tiled0 = !c->floating;
    drag.mon0 = c->mon;
    drag.mfact0 = MFACT;
    drag.cfact0 = (c->cfact < 0.1f) ? 1.0f : c->cfact;
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
        float new_mfact = drag.mfact0 + (float)dx / (float)aw;
        if (new_mfact < 0.1f) new_mfact = 0.1f;
        if (new_mfact > 0.9f) new_mfact = 0.9f;
        MFACT = new_mfact;
        float base = (drag.cfact0 < 0.1f) ? 1.0f : drag.cfact0;
        float new_cfact = base + (float)dy / 150.0f;
        if (new_cfact < 0.25f) new_cfact = 0.25f;
        if (new_cfact > 4.0f) new_cfact = 4.0f;
        c->cfact = new_cfact;
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
    }
}
void drag_end(int px, int py) {
    Client *c;
    if (drag.win == None) return;
    XUngrabPointer(dpy, CurrentTime);
    c = find(drag.win);
    int mode = drag.mode, tiled0 = drag.tiled0, mon0 = drag.mon0;
    drag.win = None; drag.mode = 0;
    if (c) {
        if (mode == 1) {
            int newmon = mon_at(px, py);
            c->mon = newmon;
            if (tiled0 && newmon != mon0)
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
