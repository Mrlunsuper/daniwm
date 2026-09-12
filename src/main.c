#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>
#include <X11/XKBlib.h>
#include <X11/extensions/Xrandr.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <sys/select.h>

#include "types.h"
#include "state.h"
#include "monitor.h"
#include "ewmh.h"
#include "bar.h"
#include "client.h"
#include "layout.h"
#include "config.h"
#include "keys.h"
#include "mouse.h"
#include "sysmon.h"

static int xerror_other_wm(Display *d, XErrorEvent *e) {
    (void)d; (void)e;
    fprintf(stderr, "daniwm: another WM is already running\n");
    exit(1);
    return -1;
}
static int xerror_ignore(Display *d, XErrorEvent *e) { (void)d; (void)e; return 0; }

int main(void) {
    signal(SIGCHLD, SIG_IGN); /* auto-reap spawn()ed children, no zombies */
    dpy = XOpenDisplay(NULL);
    if (!dpy) { fprintf(stderr, "daniwm: cannot open display\n"); return 1; }
    screen = DefaultScreen(dpy);
    root = RootWindow(dpy, screen);
    sw = DisplayWidth(dpy, screen);
    sh = DisplayHeight(dpy, screen);

    load_config(NULL);

    XSetErrorHandler(xerror_other_wm);
    XSelectInput(dpy, root, SubstructureRedirectMask | SubstructureNotifyMask
                 | EnterWindowMask | KeyPressMask | ButtonPressMask);
    XSync(dpy, False);
    XSetErrorHandler(xerror_ignore);
    ewmh_init();

    /* RandR hotplug: re-tile on output connect/disconnect, no restart.
     * Xinerama emulation sits on top of RandR, so re-querying it
     * after RRNotify picks up the new layout. */
    if (XRRQueryExtension(dpy, &rr_event_base, &rr_error_base)) {
        rr_present = 1;
        XRRSelectInput(dpy, root, RRScreenChangeNotifyMask | RRCrtcChangeNotifyMask | RROutputChangeNotifyMask);
    }

    initmons();
    update_struts(); /* recompute with real monitor geometry + extents */

    /* bar (mon 0) */
    {
        XSetWindowAttributes wa = { .override_redirect = True,
            .background_pixel = BAR_BG, .event_mask = ExposureMask | ButtonPressMask };
        bar = XCreateWindow(dpy, root, mons[0].x, mons[0].y, (unsigned)barw, (unsigned)S(BAR_H), 0,
            CopyFromParent, InputOutput, CopyFromParent,
            CWOverrideRedirect | CWBackPixel | CWEventMask, &wa);
        XSelectInput(dpy, bar, ExposureMask | ButtonPressMask);
        if (bar_on) XMapWindow(dpy, bar);
        bar_style();
    }

    grabkeys();

    Window r, p, *kids = NULL; unsigned int nk = 0;
    if (XQueryTree(dpy, root, &r, &p, &kids, &nk))
        for (unsigned i = 0; i < nk; i++) {
            XWindowAttributes a;
            if (kids[i] == bar) continue;
            if (XGetWindowAttributes(dpy, kids[i], &a) && !a.override_redirect && a.map_state == IsViewable)
                manage(kids[i]);
        }
    if (kids) XFree(kids);
    arrange();
    /* autostart (non-blocking) */
    {
        const char *xdg = getenv("XDG_CONFIG_HOME");
        const char *home = getenv("HOME");
        char path[512] = "";
        char fallback_autostart[512] = "";
        if (xdg && *xdg) snprintf(path, sizeof(path), "%s/daniwm/autostart.sh", xdg);
        else if (home && *home) snprintf(path, sizeof(path), "%s/.config/daniwm/autostart.sh", home);
        if (home && *home) snprintf(fallback_autostart, sizeof(fallback_autostart), "%s/.config/tilewm/autostart.sh", home);
        const char *run = (path[0] && !access(path, X_OK)) ? path : (fallback_autostart[0] && !access(fallback_autostart, X_OK) ? fallback_autostart : NULL);
        if (run) {
            pid_t pid = fork();
            if (pid == -1) {
                perror("daniwm: fork autostart");
            } else if (pid == 0) {
                if (dpy) close(ConnectionNumber(dpy));
                setsid();
                signal(SIGCHLD, SIG_DFL);
                execl(run, run, NULL);
                _exit(1);
            }
        }
    }

    int xfd = ConnectionNumber(dpy);
    int select_errs = 0;
    for (;;) {
        while (!XPending(dpy)) {
            /* 1s tick for clock */
            fd_set rfds; FD_ZERO(&rfds); FD_SET(xfd, &rfds);
            struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
            int ret = select(xfd + 1, &rfds, NULL, NULL, &tv);
            if (ret < 0) {
                if (errno == EINTR) continue;
                fprintf(stderr, "daniwm: select: %s\n", strerror(errno));
                if (++select_errs > 100) {
                    fprintf(stderr, "daniwm: too many select errors, exiting\n");
                    exit(1);
                }
                break; /* fall through to blocking XNextEvent */
            }
            select_errs = 0;
            if (ret == 0) { sys_vol_update(); drawbar(); }
            else break;
        }
        XEvent ev;
        XNextEvent(dpy, &ev);
        if (rr_present && (ev.type == rr_event_base + RRScreenChangeNotify ||
                            ev.type == rr_event_base + RRNotify)) {
            XRRUpdateConfiguration(&ev);
            on_monitors_changed();
            continue; /* not break: we are before switch(), break would exit for(;;) */
        }
        switch (ev.type) {
        case MapRequest: {
            XMapRequestEvent *e = &ev.xmaprequest;
            if (e->window == bar) break;
            if (find_dock(e->window)) {
                XMapWindow(dpy, e->window);
                break;
            }
            Client *c = find(e->window);
            if (c) {
                if (c->ws == curws && LAYOUT == L_TILE) XMapWindow(dpy, e->window);
                if (c->ws == curws) { focus(c); arrange(); }
            } else manage(e->window);
            break;
        }
        case UnmapNotify: {
            XUnmapEvent *e = &ev.xunmap;
            if (e->window == bar) break;
            if (find_dock(e->window)) {
                unmanage_dock(e->window);
                break;
            }
            Client *c = find(e->window);
            if (c && e->send_event) unmanage(e->window);
            break;
        }
        case ClientMessage: {
            XClientMessageEvent *e = &ev.xclient;
            Client *c = find(e->window);
            if (e->message_type == A_NET_ACTIVE_WINDOW) {
                if (c) {
                    if (c->ws != curws) view(c->ws);
                    focus(c);
                    arrange();
                }
            } else if (e->message_type == A_NET_CLOSE_WINDOW) {
                if (c) kill_client(c);
            } else if (e->message_type == A_NET_CURRENT_DESKTOP) {
                long n = e->data.l[0];
                if (n >= 0 && n < NWS) view((int)n);
            } else if (e->message_type == A_NET_WM_DESKTOP && c) {
                long n = e->data.l[0];
                if (n >= 0 && n < NWS) move_to(c, (int)n);
            } else if (e->message_type == A_NET_WM_STATE && c) {
                long act = e->data.l[0];
                Atom a1 = (Atom)e->data.l[1], a2 = (Atom)e->data.l[2];
                if (a1 == A_NET_WM_STATE_FS || a2 == A_NET_WM_STATE_FS)
                    setfullscreen(c, (act == 1) || (act == 2 && !c->fullscreen));
                if (a1 == A_NET_WM_STATE_DA || a2 == A_NET_WM_STATE_DA) {
                    int urg = (act == 1) || (act == 2 && !c->urgent);
                    if (c != sel) set_urgent(c, urg);
                }
            }
            break;
        }
        case DestroyNotify:
            if (ev.xdestroywindow.window != bar) {
                if (find_dock(ev.xdestroywindow.window))
                    unmanage_dock(ev.xdestroywindow.window);
                else
                    unmanage(ev.xdestroywindow.window);
            }
            break;
        case ConfigureRequest: {
            XConfigureRequestEvent *e = &ev.xconfigurerequest;
            if (e->window == bar) { /* keep bar fixed */
                XMoveResizeWindow(dpy, bar, mons[0].x, mons[0].y, (unsigned)barw, (unsigned)S(BAR_H));
                break;
            }
            XWindowChanges wc = {
                .x = e->x, .y = e->y, .width = e->width, .height = e->height,
                .border_width = e->border_width,
                .sibling = e->above, .stack_mode = e->detail
            };
            if (find_dock(e->window)) {
                wc.border_width = 0;
                XConfigureWindow(dpy, e->window, (unsigned int)e->value_mask, &wc);
                update_dock_strut(e->window);
                arrange();
                break;
            }
            Client *c = find(e->window);
            if (!c || c->floating) {
                if (c && c->floating) {
                    if (e->value_mask & CWX) c->fx = wc.x;
                    if (e->value_mask & CWY) c->fy = wc.y;
                    if (e->value_mask & CWWidth) c->fw = wc.width;
                    if (e->value_mask & CWHeight) c->fh = wc.height;
                }
                XConfigureWindow(dpy, e->window, (unsigned int)e->value_mask, &wc);
            } else
                XConfigureWindow(dpy, e->window, (e->value_mask & (CWSibling | CWStackMode)), &wc);
            if (c) arrange();
            break;
        }
        case EnterNotify: {
            XCrossingEvent *e = &ev.xcrossing;
            if (e->window == bar || drag.win != None || find_dock(e->window)) break;
            Client *c = find(e->window);
            if (c && c != sel && c->ws == curws && e->mode != NotifyGrab) focus(c);
            break;
        }
        case PropertyNotify: {
            XPropertyEvent *pe = &ev.xproperty;
            if (find_dock(pe->window)) {
                if (pe->atom == A_NET_WM_STRUT || pe->atom == A_NET_WM_STRUT_PARTIAL)
                    update_dock_strut(pe->window);
            } else {
                Client *c = find(pe->window);
                if (c) {
                    if (pe->atom == A_NET_WM_STRUT || pe->atom == A_NET_WM_STRUT_PARTIAL) {
                        Window dw = pe->window;
                        unmanage(dw);
                        manage_dock(dw);
                    } else if (pe->atom == XA_WM_HINTS) {
                        XWMHints *wmh = XGetWMHints(dpy, pe->window);
                        if (wmh) {
                            if ((wmh->flags & XUrgencyHint) && c != sel)
                                set_urgent(c, 1);
                            XFree(wmh);
                        }
                    } else {
                        drawbar();
                    }
                }
            }
            break;
        }
        case Expose:
            if (ev.xexpose.window == bar) drawbar();
            break;
        case KeyPress: {
            XKeyEvent *e = &ev.xkey;
            KeySym ks = XkbKeycodeToKeysym(dpy, (KeyCode)e->keycode, 0, 0);
            for (unsigned i = 0; i < nkeys; i++) {
                if (keys[i].keysym == ks &&
                    (e->state & (Mod4Mask | Mod1Mask | ControlMask | ShiftMask)) == (keys[i].mod & (Mod4Mask | Mod1Mask | ControlMask | ShiftMask))) {
                    keys[i].fn(keys[i].arg);
                    break;
                }
            }
            break;
        }
        case ButtonPress: {
            XButtonEvent *e = &ev.xbutton;
            if (e->window == bar) {
                if (e->button == Button4) { k_vol_up(0); }        /* scroll up: louder */
                else if (e->button == Button5) { k_vol_down(0); } /* scroll down: quieter */
                else if (e->button == Button2 || e->button == Button3) { k_vol_mute(0); } /* mid/right: mute */
                else {
                    int wsw = S(WS_W); if (wsw < 1) wsw = 1;
                    int n = e->x / wsw;
                    if (n >= 0 && n < NWS) view(n);
                }
            } else if (find_dock(e->window) || find_dock(e->subwindow)) {
                break;
            } else {
                /* grabbed presses report window == client; plain clicks
                 * propagate from root with subwindow == client */
                Client *c = e->window == root ? find(e->subwindow) : find(e->window);
                if (c && c->ws == curws) {
                    focus(c);
                    if ((e->state & MOD) && (e->button == Button1 || e->button == Button3))
                        drag_start(c, e->button == Button3 ? 2 : 1, e->x_root, e->y_root);
                }
            }
            break;
        }
        case MotionNotify: {
            /* coalesce backlog: only the latest pointer pos matters,
             * otherwise fast drags lag behind the cursor */
            XEvent ne;
            while (XCheckMaskEvent(dpy, PointerMotionMask, &ne)) ev = ne;
            XMotionEvent *e = &ev.xmotion;
            drag_motion(e->x_root, e->y_root);
            break;
        }
        case ButtonRelease: {
            XButtonEvent *e = &ev.xbutton;
            if (drag.win != None) drag_end(e->x_root, e->y_root);
            break;
        }
        default: break;
        }
    }
    return 0;
}
