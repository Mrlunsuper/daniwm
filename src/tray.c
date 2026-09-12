#include "tray.h"

#include <X11/Xatom.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bar.h"
#include "client.h"
#include "ewmh.h"
#include "state.h"

/* Icons are tiny (bar_h-6, capped at 24px). Anything larger is a normal
 * window that must stay tiled — e.g. Telegram's main window if a buggy
 * client/bridge ever sends REQUEST_DOCK with its ID. */
#define TRAY_MAX_ICON 64

/* freedesktop systemtray + XEmbed opcodes */
#define SYSTEM_TRAY_REQUEST_DOCK 0
#define XEMBED_EMBEDDED_NOTIFY   0
#define XEMBED_MAPPED            (1 << 0)

int tray_icon_size(void) {
    int bh = S(BAR_H);
    int sz = bh - 6;
    if (sz < 8) sz = 8;
    if (sz > 24) sz = 24;
    return sz;
}

static int tray_gap(void) {
    int g = S(4);
    if (g < 2) g = 2;
    return g;
}

static void send_embedded_notify(Window icon) {
    XEvent ev;
    memset(&ev, 0, sizeof(ev));
    ev.xclient.type = ClientMessage;
    ev.xclient.window = icon;
    ev.xclient.message_type = A_XEMBED;
    ev.xclient.format = 32;
    ev.xclient.data.l[0] = CurrentTime;
    ev.xclient.data.l[1] = XEMBED_EMBEDDED_NOTIFY;
    ev.xclient.data.l[2] = 0;
    ev.xclient.data.l[3] = (long)(bar ? bar : traywin);
    ev.xclient.data.l[4] = 0;
    XSendEvent(dpy, icon, False, NoEventMask, &ev);
}

static int read_xembed_mapped(Window w, int *mapped) {
    Atom rt = None;
    int rf = 0;
    unsigned long n = 0, extra = 0;
    unsigned char *data = NULL;
    if (A_XEMBED_INFO == None) return 0;
    if (XGetWindowProperty(dpy, w, A_XEMBED_INFO, 0, 2, False,
            A_XEMBED_INFO, &rt, &rf, &n, &extra, &data) != Success)
        return 0;
    if (!data) return 0;
    int ok = (rf == 32 && n >= 2);
    if (ok && mapped) {
        long *v = (long *)data;
        *mapped = (v[1] & XEMBED_MAPPED) != 0;
    }
    XFree(data);
    return ok;
}

static void acquire_selection(void) {
    char name[32];
    snprintf(name, sizeof(name), "_NET_SYSTEM_TRAY_S%d", screen);
    if (A_TRAY_SEL == None)
        A_TRAY_SEL = XInternAtom(dpy, name, False);
    if (A_TRAY_OPCODE == None)
        A_TRAY_OPCODE = XInternAtom(dpy, "_NET_SYSTEM_TRAY_OPCODE", False);
    if (A_XEMBED == None)
        A_XEMBED = XInternAtom(dpy, "_XEMBED", False);
    if (A_XEMBED_INFO == None)
        A_XEMBED_INFO = XInternAtom(dpy, "_XEMBED_INFO", False);
    if (A_TRAY_ORIENT == None)
        A_TRAY_ORIENT = XInternAtom(dpy, "_NET_SYSTEM_TRAY_ORIENTATION", False);
    if (A_MANAGER == None)
        A_MANAGER = XInternAtom(dpy, "MANAGER", False);

    if (!traywin) {
        XSetWindowAttributes wa;
        memset(&wa, 0, sizeof(wa));
        wa.override_redirect = True;
        wa.event_mask = PropertyChangeMask | StructureNotifyMask;
        traywin = XCreateWindow(dpy, root, -1, -1, 1, 1, 0, 0,
            InputOnly, CopyFromParent, CWOverrideRedirect | CWEventMask, &wa);
    }
    XSetSelectionOwner(dpy, A_TRAY_SEL, traywin, CurrentTime);
    if (XGetSelectionOwner(dpy, A_TRAY_SEL) == traywin) {
        long orient = 0; /* horizontal */
        XChangeProperty(dpy, traywin, A_TRAY_ORIENT, XA_CARDINAL, 32,
            PropModeReplace, (unsigned char *)&orient, 1);
        /* announce so clients start docking */
        {
            XEvent ev;
            memset(&ev, 0, sizeof(ev));
            ev.xclient.type = ClientMessage;
            ev.xclient.window = root;
            ev.xclient.message_type = A_MANAGER;
            ev.xclient.format = 32;
            ev.xclient.data.l[0] = CurrentTime;
            ev.xclient.data.l[1] = (long)A_TRAY_SEL;
            ev.xclient.data.l[2] = (long)traywin;
            ev.xclient.data.l[3] = 0;
            ev.xclient.data.l[4] = 0;
            XSendEvent(dpy, root, False, StructureNotifyMask, &ev);
        }
        tray_active = 1;
    } else {
        fprintf(stderr, "daniwm: tray selection already owned, tray inactive\n");
        tray_active = 0;
    }
    XFlush(dpy);
}

void tray_init(void) {
    if (!tray_on) { tray_active = 0; return; }
    acquire_selection();
}

void tray_enable(int on) {
    if (on && !tray_active) {
        acquire_selection();
        drawbar();
    } else if (!on && tray_active) {
        if (A_TRAY_SEL != None)
            XSetSelectionOwner(dpy, A_TRAY_SEL, None, CurrentTime);
        /* orphan icons back to root, hidden; they re-dock elsewhere or exit */
        for (TrayIcon *t = trayicons; t; t = t->next) {
            XReparentWindow(dpy, t->win, root, 0, 0);
            XUnmapWindow(dpy, t->win);
        }
        tray_active = 0;
        drawbar();
    }
}

int tray_active_now(void) {
    return tray_on && tray_active && bar;
}

int tray_has(Window w) {
    for (TrayIcon *t = trayicons; t; t = t->next)
        if (t->win == w) return 1;
    return 0;
}

/* pre-dock fallback: icon windows advertise _XEMBED_INFO before REQUEST_DOCK,
 * so a MapRequest racing the opcode still routes to the tray, not manage().
 * Size-guarded: large windows are never icons, even if they carry the prop. */
int tray_is_icon_window(Window w) {
    Atom rt = None;
    int rf = 0;
    unsigned long n = 0, extra = 0;
    unsigned char *data = NULL;
    XWindowAttributes a;
    if (!dpy || w == None || w == bar || w == traywin || w == root) return 0;
    if (A_XEMBED_INFO == None) return 0;
    if (XGetWindowProperty(dpy, w, A_XEMBED_INFO, 0, 2, False,
            A_XEMBED_INFO, &rt, &rf, &n, &extra, &data) != Success)
        return 0;
    int is = (data && rf == 32 && n >= 2);
    if (data) XFree(data);
    if (!is) return 0;
    /* main windows are hundreds of px; icons are <= 32px. 64px is generous
     * and keeps real icons (24px helper, nm-applet, volumeicon) passing. */
    if (!XGetWindowAttributes(dpy, w, &a)) return 0;
    if (a.width > TRAY_MAX_ICON || a.height > TRAY_MAX_ICON) return 0;
    return 1;
}

int tray_handle_opcode(XClientMessageEvent *e) {
    if (A_TRAY_OPCODE == None || e->message_type != A_TRAY_OPCODE) return 0;
    if (e->data.l[1] != SYSTEM_TRAY_REQUEST_DOCK) return 1; /* BEGIN/CANCEL: ack, ignore */
    tray_add((Window)e->data.l[2]);
    return 1;
}

void tray_add(Window icon) {
    XWindowAttributes a;
    if (icon == None || !tray_on) return;
    if (tray_has(icon)) return;
    if (!XGetWindowAttributes(dpy, icon, &a)) return;
    if (!bar) return;
    /* never steal normal windows: a dock opcode carrying a main-window ID
     * (buggy client/bridge, e.g. Telegram reports) used to shrink the whole
     * app into the bar. Large windows always stay tiled. */
    if (a.width > TRAY_MAX_ICON || a.height > TRAY_MAX_ICON) {
        fprintf(stderr, "daniwm: tray: ignore dock for large window 0x%lx (%dx%d)\n",
            icon, a.width, a.height);
        return;
    }
    if (find_dock(icon)) return;
    if (find(icon)) {
        /* raced: small icon got manage()d before the opcode arrived.
         * Migrate it to the tray; a large managed window is never an icon. */
        if (!tray_is_icon_window(icon)) return;
        unmanage(icon);
    }

    TrayIcon *t = calloc(1, sizeof(*t));
    if (!t) return;
    t->win = icon;
    t->mapped = 1;
    t->next = NULL;
    if (!trayicons) trayicons = t;
    else { TrayIcon *l = trayicons; while (l->next) l = l->next; l->next = t; }

    XSelectInput(dpy, icon, StructureNotifyMask | PropertyChangeMask | ResizeRedirectMask);
    XSetWindowBorderWidth(dpy, icon, 0);

    int mapped = 1;
    if (read_xembed_mapped(icon, &mapped)) t->mapped = mapped;

    int sz = tray_icon_size();
    XReparentWindow(dpy, icon, bar, 0, 0);
    XMoveResizeWindow(dpy, icon, 0, 0, (unsigned)sz, (unsigned)sz);
    send_embedded_notify(icon);
    if (t->mapped) XMapWindow(dpy, icon);

    tray_layout_icons();
    drawbar();
}

void tray_remove(Window w) {
    TrayIcon **p = &trayicons;
    while (*p && (*p)->win != w) p = &(*p)->next;
    if (!*p) return;
    TrayIcon *t = *p;
    *p = t->next;
    free(t);
    tray_layout_icons();
    drawbar();
}

void tray_handle_map(Window w) {
    for (TrayIcon *t = trayicons; t; t = t->next)
        if (t->win == w) {
            t->mapped = 1;
            XMapWindow(dpy, w);
            tray_layout_icons();
            drawbar();
            return;
        }
}

void tray_handle_unmap(Window w) {
    for (TrayIcon *t = trayicons; t; t = t->next)
        if (t->win == w) {
            /* _XEMBED_INFO decides hide-vs-remove: unmapped flag -> hide,
             * no info -> keep mapped (plain hide, dwm-style) */
            int m = 0;
            if (read_xembed_mapped(w, &m)) t->mapped = m;
            tray_layout_icons();
            drawbar();
            return;
        }
}

void tray_handle_property(Window w, Atom a) {
    if (a != A_XEMBED_INFO) return;
    for (TrayIcon *t = trayicons; t; t = t->next)
        if (t->win == w) {
            int m = t->mapped;
            if (read_xembed_mapped(w, &m)) {
                t->mapped = m;
                if (m) XMapWindow(dpy, w);
                else XUnmapWindow(dpy, w);
                tray_layout_icons();
                drawbar();
            }
            return;
        }
}

void tray_handle_resize(Window w) {
    int sz;
    if (!tray_has(w)) return;
    sz = tray_icon_size();
    XMoveResizeWindow(dpy, w, 0, 0, (unsigned)sz, (unsigned)sz);
    tray_layout_icons();
}

void tray_handle_selection_clear(Atom selatom) {
    if (A_TRAY_SEL != None && selatom == A_TRAY_SEL) {
        tray_active = 0; /* another manager took over */
        drawbar();
    }
}

int tray_width_px(void) {
    int n = 0, sz, g;
    if (!tray_on || !tray_active) return 0;
    for (TrayIcon *t = trayicons; t; t = t->next)
        if (t->mapped) n++;
    if (n == 0) return 0;
    sz = tray_icon_size();
    g = tray_gap();
    return n * sz + (n - 1) * g;
}

void tray_layout_icons(void) {
    int bar_h, sz, g, x, y;
    if (!bar || !tray_on || !tray_active) return;
    bar_h = S(BAR_H);
    if (bar_h < 8) bar_h = 8;
    sz = tray_icon_size();
    g = tray_gap();
    /* same S(8) right margin as the text block; the clock↔tray separator
     * drawn in drawbar() ends exactly where the first icon starts, so its
     * trailing spaces double as the gutter (matches inter-module gaps). */
    x = barw - S(BAR_PAD_R);
    y = (bar_h - sz) / 2;
    if (y < 0) y = 0;
    /* iterate from the end so the first-docked icon stays rightmost,
     * matching dwm/stalonetray order */
    int n = 0;
    for (TrayIcon *t = trayicons; t; t = t->next)
        if (t->mapped) n++;
    /* collect mapped icons in order, then place right-to-left */
    Window *ws = NULL;
    if (n > 0) {
        ws = malloc(sizeof(Window) * (size_t)n);
        if (!ws) return;
        int k = 0;
        for (TrayIcon *t = trayicons; t; t = t->next)
            if (t->mapped) ws[k++] = t->win;
        for (int j = 0; j < n; j++) {
            x -= sz;
            XMoveResizeWindow(dpy, ws[j], x, y, (unsigned)sz, (unsigned)sz);
            XMapWindow(dpy, ws[j]);
            x -= g;
        }
        free(ws);
    }
    XFlush(dpy);
}
