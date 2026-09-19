#include "ewmh.h"

#include <X11/Xatom.h>

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "bar.h"
#include "client.h"
#include "layout.h"
#include "monitor.h"
#include "state.h"

/* ---- EWMH ---- */
void ewmh_init(void) {
    A_WM_STATE = XInternAtom(dpy, "WM_STATE", False);
    A_WM_DELETE = XInternAtom(dpy, "WM_DELETE_WINDOW", False);
    A_WM_PROTOCOLS = XInternAtom(dpy, "WM_PROTOCOLS", False);
    A_WM_TAKE_FOCUS = XInternAtom(dpy, "WM_TAKE_FOCUS", False);
    A_NET_SUPPORTED = XInternAtom(dpy, "_NET_SUPPORTED", False);
    A_NET_CLIENT_LIST = XInternAtom(dpy, "_NET_CLIENT_LIST", False);
    A_NET_ACTIVE_WINDOW = XInternAtom(dpy, "_NET_ACTIVE_WINDOW", False);
    A_NET_WM_STATE = XInternAtom(dpy, "_NET_WM_STATE", False);
    A_NET_WM_STATE_FS = XInternAtom(dpy, "_NET_WM_STATE_FULLSCREEN", False);
    A_NET_WM_STATE_HIDDEN = XInternAtom(dpy, "_NET_WM_STATE_HIDDEN", False);
    A_NET_WM_STATE_DA = XInternAtom(dpy, "_NET_WM_STATE_DEMANDS_ATTENTION", False);
    A_NET_WM_STATE_MODAL = XInternAtom(dpy, "_NET_WM_STATE_MODAL", False);
    A_NET_WM_WINDOW_TYPE = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE", False);
    A_NET_WM_WINDOW_TYPE_DIALOG = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE_DIALOG", False);
    A_NET_WM_WINDOW_TYPE_DOCK = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE_DOCK", False);
    A_NET_WM_WINDOW_TYPE_TOOLBAR = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE_TOOLBAR", False);
    A_NET_WM_WINDOW_TYPE_SPLASH = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE_SPLASH", False);
    A_NET_WM_WINDOW_TYPE_UTILITY = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE_UTILITY", False);
    A_NET_WM_WINDOW_TYPE_MENU = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE_MENU", False);
    A_NET_WM_WINDOW_TYPE_DROPDOWN = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE_DROPDOWN_MENU", False);
    A_NET_WM_WINDOW_TYPE_POPUP = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE_POPUP_MENU", False);
    A_NET_WM_WINDOW_TYPE_TOOLTIP = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE_TOOLTIP", False);
    A_NET_WM_WINDOW_TYPE_NOTIF = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE_NOTIFICATION", False);
    A_NET_WM_WINDOW_TYPE_COMBO = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE_COMBO", False);
    A_NET_WM_WINDOW_TYPE_DND = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE_DND", False);
    A_NET_CLOSE_WINDOW = XInternAtom(dpy, "_NET_CLOSE_WINDOW", False);
    A_NET_SUPPORTING_WM_CHECK = XInternAtom(dpy, "_NET_SUPPORTING_WM_CHECK", False);
    A_NET_WM_NAME = XInternAtom(dpy, "_NET_WM_NAME", False);
    A_NET_WM_PID = XInternAtom(dpy, "_NET_WM_PID", False);
    A_NET_WM_STRUT = XInternAtom(dpy, "_NET_WM_STRUT", False);
    A_NET_WM_STRUT_PARTIAL = XInternAtom(dpy, "_NET_WM_STRUT_PARTIAL", False);
    A_NET_WORKAREA = XInternAtom(dpy, "_NET_WORKAREA", False);
    A_NET_NUMBER_OF_DESKTOPS = XInternAtom(dpy, "_NET_NUMBER_OF_DESKTOPS", False);
    A_NET_CURRENT_DESKTOP = XInternAtom(dpy, "_NET_CURRENT_DESKTOP", False);
    A_NET_WM_DESKTOP = XInternAtom(dpy, "_NET_WM_DESKTOP", False);
    A_NET_DESKTOP_NAMES = XInternAtom(dpy, "_NET_DESKTOP_NAMES", False);
    Atom utf8 = XInternAtom(dpy, "UTF8_STRING", False);

    checkwin = XCreateSimpleWindow(dpy, root, 0, 0, 1, 1, 0, 0, 0);
    XChangeProperty(dpy, checkwin, A_NET_SUPPORTING_WM_CHECK, XA_WINDOW, 32,
        PropModeReplace, (unsigned char *)&checkwin, 1);
    XChangeProperty(dpy, checkwin, A_NET_WM_NAME, utf8, 8,
        PropModeReplace, (unsigned char *)"daniwm", 6);
    /* EWMH: the WM identifies itself with its PID on the supporting window. */
    {
        unsigned long pid = (unsigned long)getpid();
        XChangeProperty(dpy, checkwin, A_NET_WM_PID, XA_CARDINAL, 32,
            PropModeReplace, (unsigned char *)&pid, 1);
    }
    XChangeProperty(dpy, root, A_NET_SUPPORTING_WM_CHECK, XA_WINDOW, 32,
        PropModeReplace, (unsigned char *)&checkwin, 1);

    Atom sup[] = { A_NET_SUPPORTED, A_NET_CLIENT_LIST, A_NET_ACTIVE_WINDOW,
        A_NET_WM_STATE, A_NET_WM_STATE_FS, A_NET_WM_STATE_HIDDEN,
        A_NET_WM_STATE_DA, A_NET_WM_STATE_MODAL, A_NET_WM_WINDOW_TYPE, A_NET_CLOSE_WINDOW,
        A_NET_SUPPORTING_WM_CHECK, A_NET_WM_NAME, A_NET_WM_PID,
        A_NET_WM_STRUT, A_NET_WM_STRUT_PARTIAL, A_NET_WORKAREA,
        A_NET_NUMBER_OF_DESKTOPS, A_NET_CURRENT_DESKTOP,
        A_NET_WM_DESKTOP, A_NET_DESKTOP_NAMES };
    XChangeProperty(dpy, root, A_NET_SUPPORTED, XA_ATOM, 32,
        PropModeReplace, (unsigned char *)sup, sizeof(sup) / sizeof(sup[0]));
    ewmh_client_list(); /* empty list so pagers/wmctrl never query a missing prop */
    ewmh_desktops();
    update_struts();
}
/* ICCCM WM_STATE mirror: NormalState on manage, WithdrawnState on unmanage.
 * Mirror only; the clients list stays the single source of truth. */
void ewmh_set_wm_state(Client *c, long state) {
    unsigned long s;
    if (!c || A_WM_STATE == None) return;
    s = (unsigned long)state;
    XChangeProperty(dpy, c->win, A_WM_STATE, A_WM_STATE, 32,
        PropModeReplace, (unsigned char *)&s, 1);
}
int ewmh_hasstate(Window w, Atom state) {
    Atom *p = NULL, rt; int rf, found = 0;
    unsigned long n, extra;
    if (XGetWindowProperty(dpy, w, A_NET_WM_STATE, 0, 8, False, XA_ATOM,
        &rt, &rf, &n, &extra, (unsigned char **)&p) == Success && p) {
        for (unsigned long i = 0; i < n; i++) if (p[i] == state) found = 1;
        XFree(p);
    }
    return found;
}
/* rebuild _NET_WM_STATE from client fields (fullscreen, hidden, urgent) */
void ewmh_update_state(Client *c) {
    if (!c || A_NET_WM_STATE == None) return;
    Atom states[4];
    int n = 0;
    if (c->fullscreen) states[n++] = A_NET_WM_STATE_FS;
    if (c->hidden) states[n++] = A_NET_WM_STATE_HIDDEN;
    if (c->urgent) states[n++] = A_NET_WM_STATE_DA;
    if (n > 0)
        XChangeProperty(dpy, c->win, A_NET_WM_STATE, XA_ATOM, 32,
            PropModeReplace, (unsigned char *)states, n);
    else
        XDeleteProperty(dpy, c->win, A_NET_WM_STATE);
}
void set_urgent(Client *c, int urg) {
    if (!c || c->urgent == urg) return;
    c->urgent = urg;
    ewmh_update_state(c);
    drawbar();
}
int ws_has_urgent(int n) {
    for (Client *c = clients; c; c = c->next)
        if (c->ws == n && c->urgent) return 1;
    return 0;
}
int ewmh_isfloating_type(Window w) {
    Atom *p = NULL, rt; int rf, f = 0;
    unsigned long n, extra;
    if (XGetWindowProperty(dpy, w, A_NET_WM_WINDOW_TYPE, 0, 8, False, XA_ATOM,
        &rt, &rf, &n, &extra, (unsigned char **)&p) == Success && p) {
        for (unsigned long i = 0; i < n; i++)
            if (p[i] == A_NET_WM_WINDOW_TYPE_DIALOG || p[i] == A_NET_WM_WINDOW_TYPE_UTILITY ||
                p[i] == A_NET_WM_WINDOW_TYPE_TOOLBAR || p[i] == A_NET_WM_WINDOW_TYPE_SPLASH ||
                p[i] == A_NET_WM_WINDOW_TYPE_MENU || p[i] == A_NET_WM_WINDOW_TYPE_DROPDOWN ||
                p[i] == A_NET_WM_WINDOW_TYPE_POPUP || p[i] == A_NET_WM_WINDOW_TYPE_TOOLTIP ||
                p[i] == A_NET_WM_WINDOW_TYPE_NOTIF || p[i] == A_NET_WM_WINDOW_TYPE_COMBO ||
                p[i] == A_NET_WM_WINDOW_TYPE_DND) f = 1;
        XFree(p);
    }
    /* Modal dialogs (e.g. app update popups flagged NORMAL + MODAL) float too. */
    if (!f && A_NET_WM_STATE_MODAL != None && ewmh_hasstate(w, A_NET_WM_STATE_MODAL)) f = 1;
    return f;
}
void ewmh_client_list(void) {
    int n = 0;
    for (Client *c = clients; c; c = c->next)
        if (c->ws >= 0 && c->ws < NWS) n++;
    Window *ws = malloc(sizeof(Window) * (size_t)(n > 0 ? n : 1));
    if (!ws) return; /* OOM: keep the old property, never deref NULL */
    int i = 0;
    for (Client *c = clients; c; c = c->next)
        if (c->ws >= 0 && c->ws < NWS) ws[i++] = c->win;
    XChangeProperty(dpy, root, A_NET_CLIENT_LIST, XA_WINDOW, 32,
        PropModeReplace, (unsigned char *)ws, n);
    free(ws);
}
void ewmh_active(void) {
    Window w = sel ? sel->win : None;
    XChangeProperty(dpy, root, A_NET_ACTIVE_WINDOW, XA_WINDOW, 32,
        PropModeReplace, (unsigned char *)&w, 1);
}
/* Pager/desktop bridge: NUMBER+CURRENT on root, WM_DESKTOP per client,
 * NAMES as "1\0...\0". Parked scratchpad (ws==NWS) reports sticky
 * 0xFFFFFFFF. Safe to call before ewmh_init (atoms None → no-op), so
 * finalize_nws() can use it during early load_config. */
void ewmh_set_wm_desktop(Client *c) {
    unsigned long d;
    if (!dpy || A_NET_WM_DESKTOP == None || !c) return;
    d = (c->ws >= 0 && c->ws < NWS) ? (unsigned long)c->ws : 0xFFFFFFFFUL;
    XChangeProperty(dpy, c->win, A_NET_WM_DESKTOP, XA_CARDINAL, 32,
        PropModeReplace, (unsigned char *)&d, 1);
}
void ewmh_desktops(void) {
    unsigned long n, cur;
    char names[MAXWS * 68];
    int off = 0;
    Atom utf8;
    if (!dpy || A_NET_NUMBER_OF_DESKTOPS == None) return;
    n = (unsigned long)NWS;
    cur = (unsigned long)curws;
    XChangeProperty(dpy, root, A_NET_NUMBER_OF_DESKTOPS, XA_CARDINAL, 32,
        PropModeReplace, (unsigned char *)&n, 1);
    XChangeProperty(dpy, root, A_NET_CURRENT_DESKTOP, XA_CARDINAL, 32,
        PropModeReplace, (unsigned char *)&cur, 1);
    /* custom ws names (config `ws_names`) or "1".."10" fallback */
    for (int i = 0; i < NWS && off + 2 < (int)sizeof(names); i++) {
        const char *nm = (i >= 0 && i < MAXWS && ws_names[i] && *ws_names[i])
            ? ws_names[i] : NULL;
        int w;
        if (nm) w = snprintf(names + off, sizeof(names) - (size_t)off, "%s", nm);
        else w = snprintf(names + off, sizeof(names) - (size_t)off, "%d", i + 1);
        if (w < 0) break;
        off += w + 1; /* keep NUL separator even on truncation */
    }
    utf8 = XInternAtom(dpy, "UTF8_STRING", False);
    if (utf8 != None && off > 0)
        XChangeProperty(dpy, root, A_NET_DESKTOP_NAMES, utf8, 8,
            PropModeReplace, (unsigned char *)names, (int)off);
    for (Client *c = clients; c; c = c->next) ewmh_set_wm_desktop(c);
}
/* initial desktop hint on manage: CARDINAL < NWS, else -1 */
int ewmh_read_desktop(Window w) {
    Atom rt; int rf; unsigned long n, extra;
    unsigned char *data = NULL;
    long d = -1;
    if (A_NET_WM_DESKTOP == None) return -1;
    if (XGetWindowProperty(dpy, w, A_NET_WM_DESKTOP, 0, 1, False, XA_CARDINAL,
        &rt, &rf, &n, &extra, &data) == Success && data) {
        if (rf == 32 && n == 1) {
            long v = *(long *)data;
            if (v >= 0 && v < NWS) d = v;
        }
        XFree(data);
    }
    return (int)d;
}
void setfullscreen(Client *c, int fs) {
    if (!c || c->fullscreen == fs) return;
    if (fs) {
        if (c->floating) {
            XWindowAttributes wa;
            if (XGetWindowAttributes(dpy, c->win, &wa)) {
                c->fx = wa.x; c->fy = wa.y; c->fw = wa.width; c->fh = wa.height;
            }
        }
        c->fullscreen = 1;
        ewmh_update_state(c);
    } else {
        c->fullscreen = 0;
        ewmh_update_state(c);
        if (c->floating && c->fw > 0 && c->fh > 0)
            XMoveResizeWindow(dpy, c->win, c->fx, c->fy, (unsigned)c->fw, (unsigned)c->fh);
    }
    arrange();
    focus(c);
}

/* ---- docks + EWMH struts ---- */
Dock *find_dock(Window w) {
    for (Dock *d = docks; d; d = d->next)
        if (d->win == w) return d;
    return NULL;
}

static int get_strut(Window w, unsigned long *strut) {
    Atom rt; int rf; unsigned long n, extra;
    unsigned char *data = NULL;
    /* Try _NET_WM_STRUT_PARTIAL (12 cardinals) first */
    if (XGetWindowProperty(dpy, w, A_NET_WM_STRUT_PARTIAL, 0, 12, False, AnyPropertyType,
        &rt, &rf, &n, &extra, &data) == Success && data) {
        if (rf == 32 && n == 12) {
            unsigned long *vals = (unsigned long *)data;
            for (int i = 0; i < 12; i++) strut[i] = vals[i];
            XFree(data);
            return 1;
        }
        XFree(data);
    }
    data = NULL;
    /* Fallback: _NET_WM_STRUT (4 cardinals); ranges span the screen box */
    if (XGetWindowProperty(dpy, w, A_NET_WM_STRUT, 0, 4, False, AnyPropertyType,
        &rt, &rf, &n, &extra, &data) == Success && data) {
        if (rf == 32 && n == 4) {
            long ex0, ey0, ex1, ey1;
            unsigned long *vals = (unsigned long *)data;
            screen_extents(&ex0, &ey0, &ex1, &ey1);
            strut[0] = vals[0];
            strut[1] = vals[1];
            strut[2] = vals[2];
            strut[3] = vals[3];
            strut[4] = (unsigned long)ey0; strut[5]  = ey1 > ey0 ? (unsigned long)(ey1 - 1) : 0;
            strut[6] = (unsigned long)ey0; strut[7]  = ey1 > ey0 ? (unsigned long)(ey1 - 1) : 0;
            strut[8] = (unsigned long)ex0; strut[9]  = ex1 > ex0 ? (unsigned long)(ex1 - 1) : 0;
            strut[10] = (unsigned long)ex0; strut[11] = ex1 > ex0 ? (unsigned long)(ex1 - 1) : 0;
            XFree(data);
            return 1;
        }
        XFree(data);
    }
    return 0;
}

int ewmh_isdock(Window w) {
    Atom *p = NULL, rt; int rf, is_dock = 0;
    unsigned long n, extra;
    if (XGetWindowProperty(dpy, w, A_NET_WM_WINDOW_TYPE, 0, 8, False, XA_ATOM,
        &rt, &rf, &n, &extra, (unsigned char **)&p) == Success && p) {
        for (unsigned long i = 0; i < n; i++)
            if (p[i] == A_NET_WM_WINDOW_TYPE_DOCK) is_dock = 1;
        XFree(p);
    }
    if (!is_dock) {
        unsigned long s[12];
        if (get_strut(w, s)) is_dock = 1;
    }
    return is_dock;
}

void update_struts(void) {
    long sx0, sy0, sx1, sy1;
    screen_extents(&sx0, &sy0, &sx1, &sy1);
    for (int m = 0; m < nmons; m++) {
        mon_struts[m].left = 0;
        mon_struts[m].right = 0;
        mon_struts[m].top = 0;
        mon_struts[m].bottom = 0;
    }

    for (Dock *d = docks; d; d = d->next) {
        if (!d->has_strut) continue;
        unsigned long l = d->strut[0], r = d->strut[1], t = d->strut[2], b = d->strut[3];
        unsigned long l_sy = d->strut[4], l_ey = d->strut[5];
        unsigned long r_sy = d->strut[6], r_ey = d->strut[7];
        unsigned long t_sx = d->strut[8], t_ex = d->strut[9];
        unsigned long b_sx = d->strut[10], b_ex = d->strut[11];

        for (int m = 0; m < nmons; m++) {
            int mx = mons[m].x, my = mons[m].y, mw = mons[m].w, mh = mons[m].h;

            /* Top: reserved [sy0, sy0+t) overlapping this monitor */
            if (t > 0 && (long)t_sx <= (long)(mx + mw - 1) && (long)t_ex >= (long)mx) {
                long edge = sy0 + (long)t;
                if (edge > (long)my) {
                    long diff = edge - (long)my;
                    if (diff > (long)mh) diff = mh;
                    if (diff > mon_struts[m].top) mon_struts[m].top = (int)diff;
                }
            }

            /* Bottom: reserved [sy1-b, sy1) overlapping this monitor */
            if (b > 0 && (long)b_sx <= (long)(mx + mw - 1) && (long)b_ex >= (long)mx) {
                long edge = sy1 - (long)b;
                if (edge < (long)(my + mh)) {
                    long lo = edge > (long)my ? edge : (long)my;
                    long diff = (long)(my + mh) - lo;
                    if (diff < 0) diff = 0;
                    if (diff > (long)mh) diff = mh;
                    if (diff > mon_struts[m].bottom) mon_struts[m].bottom = (int)diff;
                }
            }

            /* Left: reserved [sx0, sx0+l) overlapping this monitor */
            if (l > 0 && (long)l_sy <= (long)(my + mh - 1) && (long)l_ey >= (long)my) {
                long edge = sx0 + (long)l;
                if (edge > (long)mx) {
                    long diff = edge - (long)mx;
                    if (diff > (long)mw) diff = mw;
                    if (diff > mon_struts[m].left) mon_struts[m].left = (int)diff;
                }
            }

            /* Right: reserved [sx1-r, sx1) overlapping this monitor */
            if (r > 0 && (long)r_sy <= (long)(my + mh - 1) && (long)r_ey >= (long)my) {
                long edge = sx1 - (long)r;
                if (edge < (long)(mx + mw)) {
                    long lo = edge > (long)mx ? edge : (long)mx;
                    long diff = (long)(mx + mw) - lo;
                    if (diff < 0) diff = 0;
                    if (diff > (long)mw) diff = mw;
                    if (diff > mon_struts[m].right) mon_struts[m].right = (int)diff;
                }
            }
        }
    }

    /* Update _NET_WORKAREA: CARDINAL[][4] for each workspace */
    if (NWS > 0 && dpy && A_NET_WORKAREA != None) {
        unsigned long max_l = 0, max_r = 0, max_t = 0, max_b = 0;
        for (Dock *d = docks; d; d = d->next) {
            if (!d->has_strut) continue;
            if (d->strut[0] > max_l) max_l = d->strut[0];
            if (d->strut[1] > max_r) max_r = d->strut[1];
            if (d->strut[2] > max_t) max_t = d->strut[2];
            if (d->strut[3] > max_b) max_b = d->strut[3];
        }
        if (bar_on && (unsigned long)S(BAR_H) > max_t) max_t = (unsigned long)S(BAR_H);
        long swidth = sx1 - sx0, sheight = sy1 - sy0;
        if (swidth < 0) swidth = 0;
        if (sheight < 0) sheight = 0;
        unsigned long wax = (unsigned long)(sx0 + (long)max_l);
        unsigned long way = (unsigned long)(sy0 + (long)max_t);
        unsigned long waw = (swidth > (long)(max_l + max_r)) ? (unsigned long)(swidth - (long)(max_l + max_r)) : 0;
        unsigned long wah = (sheight > (long)(max_t + max_b)) ? (unsigned long)(sheight - (long)(max_t + max_b)) : 0;

        unsigned long *wa = malloc(sizeof(unsigned long) * 4 * (size_t)NWS);
        if (wa) {
            for (int i = 0; i < NWS; i++) {
                wa[i * 4 + 0] = wax;
                wa[i * 4 + 1] = way;
                wa[i * 4 + 2] = waw;
                wa[i * 4 + 3] = wah;
            }
            XChangeProperty(dpy, root, A_NET_WORKAREA, XA_CARDINAL, 32,
                PropModeReplace, (unsigned char *)wa, NWS * 4);
            free(wa);
        }
    }
}

void manage_dock(Window w) {
    if (find_dock(w) || w == bar) return;
    Dock *d = calloc(1, sizeof(Dock));
    if (!d) return;
    d->win = w;
    d->has_strut = get_strut(w, d->strut);
    d->next = docks;
    docks = d;

    XSelectInput(dpy, w, PropertyChangeMask | StructureNotifyMask);
    XSetWindowBorderWidth(dpy, w, 0);
    XMapWindow(dpy, w);
    XRaiseWindow(dpy, w);

    update_struts();
    arrange();
}

void unmanage_dock(Window w) {
    Dock **p = &docks;
    while (*p && (*p)->win != w) p = &(*p)->next;
    if (!*p) return;
    Dock *d = *p;
    *p = d->next;
    free(d);

    update_struts();
    arrange();
}

void update_dock_strut(Window w) {
    Dock *d = find_dock(w);
    if (!d) return;
    d->has_strut = get_strut(w, d->strut);
    update_struts();
    arrange();
}
