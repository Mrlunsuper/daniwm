/* main.c - daniwm entry point: init, event loop (module split of daniwm.c).
 * Build with `make` (multi-object, see Makefile); binary lands at ./daniwm. */
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>
#include <X11/XKBlib.h>
#include <X11/extensions/XInput2.h>
#include <X11/extensions/Xrandr.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/time.h>

#include "types.h"
#include "state.h"
#include "monitor.h"
#include "ewmh.h"
#include "bar.h"
#include "client.h"
#include "layout.h"
#include "config.h"
#include "keys.h"
#include "rename.h"
#include "mouse.h"
#include "sysmon.h"
#include "tray.h"
#include "xerr.h"

/* ---- _NET_ACTIVE_WINDOW focus-fight guard ----
 * Spammy pages (window.focus() on blur, competing across two browsers)
 * used to bounce sel A,B,A,B at hundreds of Hz; each flip re-tiles
 * monocle (unmap/map) -> visible flicker + windows never paint.
 * Detect rapid alternation between 2 windows: freeze focus on the
 * current window and mark demanders urgent instead (bar shows it).
 * Freeze extends while the fight persists, expires 2s after the last
 * demand. Keys/buttons bypass it (they call focus() directly). */
#define FIGHT_N 6          /* alternating demands to trigger */
#define FIGHT_MS 1500      /* ... within this window */
#define FIGHT_FREEZE_MS 2000
static Window ff_seq[FIGHT_N];
static long long ff_at[FIGHT_N];
static int ff_n = 0;
static long long ff_freeze_until = 0;
static long long now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000LL + ts.tv_nsec / 1000000LL;
}
/* 1 = last FIGHT_N demands strictly alternate between 2 windows, fast */
static int is_fight(void) {
    Window x = None, y = None;
    if (ff_n < FIGHT_N) return 0;
    if (ff_at[FIGHT_N - 1] - ff_at[0] >= FIGHT_MS) return 0;
    for (int i = 0; i < FIGHT_N; i++) {
        if (ff_seq[i] != x && ff_seq[i] != y) {
            if (x == None) x = ff_seq[i];
            else if (y == None) y = ff_seq[i];
            else return 0; /* 3rd window: not a duel */
        }
        if (i > 0 && ff_seq[i] == ff_seq[i - 1]) return 0;
    }
    return x != None && y != None;
}
/* 1 = swallow this demand (urgent already marked) */
static int fight_check(Client *c) {
    long long now = now_ms();
    if (c == sel) return 0;
    if (ff_n < FIGHT_N) { ff_seq[ff_n] = c->win; ff_at[ff_n] = now; ff_n++; }
    else {
        memmove(ff_seq, ff_seq + 1, (FIGHT_N - 1) * sizeof(Window));
        memmove(ff_at, ff_at + 1, (FIGHT_N - 1) * sizeof(long long));
        ff_seq[FIGHT_N - 1] = c->win; ff_at[FIGHT_N - 1] = now;
    }
    if (now < ff_freeze_until) {
        set_urgent(c, 1);
        if (is_fight()) ff_freeze_until = now + FIGHT_FREEZE_MS;
        return 1;
    }
    if (is_fight()) {
        ff_freeze_until = now + FIGHT_FREEZE_MS;
        set_urgent(c, 1);
        return 1;
    }
    return 0;
}

/* ---- hover focus lock ----
 * Đóng menu chuột phải hay unmap cửa sổ dưới con trỏ đều làm X gửi
 * EnterNotify thật (mode Normal), dù chuột đứng yên. Nhận nó thì hộp thoại
 * vừa mở bị cướp focus ngay (Save as của trình duyệt bị như vậy).
 * Cửa sổ mới map xong giữ focus tới khi chuột DI THẬT. */
static int hover_lock_on = 0;
static int hover_lock_x = 0, hover_lock_y = 0;
static void hover_lock_arm(void) {
    Window r, ch; int rx, ry, wx, wy; unsigned m;
    if (!XQueryPointer(dpy, root, &r, &ch, &rx, &ry, &wx, &wy, &m)) return;
    hover_lock_on = 1;
    hover_lock_x = rx; hover_lock_y = ry;
}
/* 1 = bỏ qua crossing này. Chuột rời chỗ cũ thì mở khoá luôn, nên hover
 * hoạt động lại ngay từ cú di chuột đầu tiên. */
static int hover_locked(const XCrossingEvent *e) {
    if (!hover_lock_on) return 0;
    if (e->x_root != hover_lock_x || e->y_root != hover_lock_y) {
        hover_lock_on = 0;
        return 0;
    }
    return 1;
}

/* ---- XI2 raw button observation: click-to-raise on floating windows ----
 * Plain clicks on a floating window behind another float never used to
 * bring it forward: the WM cannot select ButtonPress on foreign windows
 * (BadAccess), passive grabs would starve the app (ReplayPointer re-triggers
 * the same grab on Xorg, so the app never gets the press). The one clean
 * way to OBSERVE a click without touching its delivery is an XI2 raw
 * event: raw events are broadcast to every client that selects them on
 * the device, and the app still receives the normal core press.
 * On XI_RawButtonPress we ask the pointer for the top-level window under
 * it (XQueryPointer.child is exactly the X11 click target) and, if it is
 * a floating/fullscreen client on the current workspace, focus it —
 * focus() raises floats (focus_ex raise=1). Tiled windows never stack:
 * they only get focus_noraise() when focus = click|both. Mod+click is
 * skipped too (that is the drag grab path). The app's click is delivered untouched in all cases. */
static int xi2_ok = 0;
static int xi2_opcode = 0;
static void xi2_init(void) {
    int xi_ev = 0, xi_err = 0;
    int maj = 2, min = 0;
    if (XQueryExtension(dpy, "XInputExtension", &xi2_opcode, &xi_ev, &xi_err) != True)
        return;
    if (XIQueryVersion(dpy, &maj, &min) != Success)
        return;
    int ndev = 0;
    XIDeviceInfo *info = XIQueryDevice(dpy, XIAllDevices, &ndev);
    if (!info) return;
    unsigned char mask[4] = { 0 }; /* XI_EVENT_MASK_LEN; covers raw events */
    XISetMask(mask, XI_RawButtonPress);
    /* Raw events are per-device: select on every master pointer so clicks
     * on any pointer (multi-pointer setups) raise floats. */
    for (int i = 0; i < ndev; i++) {
        if (info[i].use != XIMasterPointer) continue;
        XIEventMask em = { .deviceid = info[i].deviceid,
            .mask_len = (int)sizeof(mask), .mask = mask };
        trap_errors(dpy);
        if (XISelectEvents(dpy, root, &em, 1) == Success)
            xi2_ok = 1;
        untrap_errors(dpy);
    }
    XIFreeDeviceInfo(info);
    if (!xi2_ok)
        fprintf(stderr, "daniwm: XI2 raw selection failed, click-to-raise off\n");
}
/* plain-click raise for the window under the pointer; returns after focus */
static void xi2_raw_click(int button) {
    Window r, child;
    int rx, ry, wx, wy;
    unsigned m;
    if (drag.win != None) return;            /* WM drag in progress */
    if (button < Button1 || button > Button3) return; /* wheel/extra: no */
    if (!XQueryPointer(dpy, root, &r, &child, &rx, &ry, &wx, &wy, &m)) return;
    if (!child || m & MOD) return;           /* Mod+aButton = drag grab path */
    Client *c = find(child);
    if (!c || c->ws != curws || c->ws < 0 || c->ws >= NWS) return;
    if (!c->floating && !c->fullscreen) {
        /* tiled: nothing to raise, but click-focus must move focus (#2) */
        if (FOCUS_MODE != 0 && c != sel) focus_noraise(c);
        return;
    }
    focus(c);                                /* raises floats (raise=1) */
}

static int xerror_other_wm(Display *d, XErrorEvent *e) {
    (void)d; (void)e;
    fprintf(stderr, "daniwm: another WM is already running\n");
    exit(1);
    return -1;
}
static int xerror_ignore(Display *d, XErrorEvent *e) { (void)d; (void)e; return 0; }
/* X I/O errors (server killed/restarted) bypass XErrorHandler and the
 * select() loop; without this the default handler exits silently. */
static int xio_fatal(Display *d) {
    (void)d;
    fprintf(stderr, "daniwm: X I/O error, exiting\n");
    _exit(1);
    return 0;
}

/* tasklist click: Button1 focuses, Button2 closes (browser-tab style).
 * Returns 1 when the click landed on a task button: the caller must not
 * fall through to workspace view or tray checks. */
static int bar_task_click(int x, unsigned button, Time t) {
    if (button != Button1 && button != Button2) return 0;
    for (int i = 0; i < task_nhit; i++) {
        if (x >= task_hit_x0[i] && x <= task_hit_x1[i]) {
            Client *c = find(task_hit_win[i]);
            if (c && c->ws == curws) {
                /* find() validated; trap makes a recycled stale XID loud. */
                trap_errors(dpy);
                if (button == Button1) focus(c);
                else kill_client_ex(c, t);
                untrap_errors(dpy);
            }
            return 1;
        }
    }
    return 0;
}

/* true when the window carries the sticky (0xFFFFFFFF) desktop hint —
 * i.e. a parked scratchpad left behind by a pre-restart instance. */
static int is_sticky(Window w) {
    Atom rt; int rf; unsigned long n, extra;
    unsigned char *data = NULL;
    int sticky = 0;
    if (A_NET_WM_DESKTOP == None) return 0;
    if (XGetWindowProperty(dpy, w, A_NET_WM_DESKTOP, 0, 1, False, XA_CARDINAL,
        &rt, &rf, &n, &extra, &data) == Success && data) {
        /* Xlib sign-extends format-32 data into long: mask to 32 bits (#3) */
        if (rf == 32 && n == 1 &&
            ((*(unsigned long *)data) & 0xFFFFFFFFUL) == 0xFFFFFFFFUL) sticky = 1;
        XFree(data);
    }
    return sticky;
}

/* ICCCM WM_STATE: -1 = no property */
static long read_wm_state(Window w) {
    Atom rt; int rf; unsigned long n, extra;
    unsigned char *data = NULL;
    long st = -1;
    if (A_WM_STATE == None) return -1;
    if (XGetWindowProperty(dpy, w, A_WM_STATE, 0, 2, False, A_WM_STATE,
        &rt, &rf, &n, &extra, &data) == Success && data) {
        if (rf == 32 && n >= 1) st = *(long *)data;
        XFree(data);
    }
    return st;
}

/* systemd/D-Bus env propagation (fix portal + notifications).
 * WM custom mà quên bước này thì systemd user services không thấy DISPLAY,
 * xdg-desktop-portal D-Bus activate fail. Fork non-blocking, auto-reap
 * nhờ SIGCHLD=SIG_IGN. NOTE Fedora 44: graphical-session.target có
 * RefuseManualStart=yes nên không start tay được; portal đã có override
 * ở ~/.config/systemd/user/xdg-desktop-portal.service để chạy không cần
 * target đó. Flameshot trên X11 dùng legacy capture (xem flameshot.ini). */
static void session_init(void) {
    if (!getenv("XDG_CURRENT_DESKTOP") || !*getenv("XDG_CURRENT_DESKTOP"))
        setenv("XDG_CURRENT_DESKTOP", "daniwm", 1);
    if (!getenv("XDG_SESSION_TYPE") || !*getenv("XDG_SESSION_TYPE"))
        setenv("XDG_SESSION_TYPE", "x11", 1);
    pid_t pid = fork();
    if (pid == -1) { perror("daniwm: fork session_init"); return; }
    if (pid == 0) {
        setsid();
        signal(SIGCHLD, SIG_DFL);
        /* propagate X env vào systemd user + dbus activation */
        execl("/bin/sh", "sh", "-c",
            "dbus-update-activation-environment --systemd DISPLAY XDG_CURRENT_DESKTOP XDG_SESSION_TYPE WAYLAND_DISPLAY >/dev/null 2>&1; "
            "systemctl --user import-environment DISPLAY XDG_CURRENT_DESKTOP XDG_SESSION_TYPE WAYLAND_DISPLAY >/dev/null 2>&1",
            NULL);
        _exit(1);
    }
}

/* monotonic ms for wheel-volume flush scheduling */
static long long ms_now(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

int main(int argc, char **argv) {

    if (argc > 0 && argv && argv[0] && *argv[0])
        snprintf(progpath, sizeof(progpath), "%s", argv[0]);
    session_init();
    signal(SIGCHLD, SIG_IGN); /* auto-reap spawn()ed children, no zombies */
    XSetIOErrorHandler(xio_fatal);
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
    /* restart beacon: re-exec keeps the PID and the X server may recycle the
     * supporting-window ID, so external watchers (and test-restart.sh) use
     * this ever-changing stamp to detect a fresh instance. */
    {
        struct timeval tv;
        gettimeofday(&tv, NULL);
        Atom beat = XInternAtom(dpy, "_DANIWM_HEARTBEAT", False);
        if (beat != None) {
            unsigned long v[2] = { (unsigned long)tv.tv_sec, (unsigned long)tv.tv_usec };
            XChangeProperty(dpy, root, beat, XA_CARDINAL, 32,
                PropModeReplace, (unsigned char *)v, 2);
        }
    }

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
            .background_pixel = BAR_BG,
            .event_mask = ExposureMask | ButtonPressMask | SubstructureNotifyMask };
        bar = XCreateWindow(dpy, root, mons[0].x, mons[0].y, (unsigned)barw, (unsigned)S(BAR_H), 0,
            CopyFromParent, InputOutput, CopyFromParent,
            CWOverrideRedirect | CWBackPixel | CWEventMask, &wa);
        XSelectInput(dpy, bar, ExposureMask | ButtonPressMask | SubstructureNotifyMask);
        if (bar_on) XMapWindow(dpy, bar);
        bar_style();
    }

    tray_init();

    grabkeys();

    /* XI2 raw-button observation: click-to-raise on floating windows
     * (plain clicks are otherwise invisible to the WM on this X stack). */
    xi2_init();

    Window r, p, *kids = NULL; unsigned int nk = 0;
    /* restart-in-place: the previous instance published its workspace on
     * root before exec; pick it up so we land on the same desktop. */
    {
        Atom rt; int rf; unsigned long n, extra;
        unsigned char *data = NULL;
        if (XGetWindowProperty(dpy, root, A_NET_CURRENT_DESKTOP, 0, 1, False, XA_CARDINAL,
            &rt, &rf, &n, &extra, &data) == Success && data) {
            if (rf == 32 && n == 1) {
                long d = *(long *)data;
                if (d >= 0 && d < NWS) { curws = (int)d; prevws = (int)d; }
            }
            XFree(data);
        }
    }
    if (XQueryTree(dpy, root, &r, &p, &kids, &nk))
        for (unsigned i = 0; i < nk; i++) {
            XWindowAttributes a;
            if (kids[i] == bar || kids[i] == traywin) continue;
            if (!XGetWindowAttributes(dpy, kids[i], &a) || a.override_redirect) continue;
            if (a.map_state == IsViewable) { manage(kids[i]); continue; }
            /* Hidden window carrying our desktop hint was managed before the
             * restart (lives on another workspace): adopt it back. Windows
             * without the hint are foreign/withdrawn helpers — leave them. */
            /* withdrawn (or never managed) windows stay put (#4) */
            long wst = read_wm_state(kids[i]);
            if (wst != NormalState && wst != IconicState) continue;
            /* read before manage(): it rewrites _NET_WM_DESKTOP to curws */
            int sticky = is_sticky(kids[i]);
            if (ewmh_read_desktop(kids[i]) < 0 && !sticky) continue;
            manage(kids[i]);
            /* hidden window with the sticky (0xFFFFFFFF) desktop hint is the
             * parked scratchpad from a pre-restart life: park it again
             * instead of mapping it onto this workspace. */
            if (sticky) {
                Client *c = find(kids[i]);
                if (c) {
                    c->ws = NWS;
                    if (sel == c) sel = NULL; /* manage() focused it */
                    if (ws_sel[curws] == c) ws_sel[curws] = NULL;
                    XUnmapWindow(dpy, c->win);
                    ewmh_set_wm_desktop(c); /* back to sticky */
                    ewmh_client_list();
                }
            }
        }
    if (kids) XFree(kids);
    arrange();
    /* restart-in-place: restore the pre-restart focus. */
    {
        Atom rt; int rf; unsigned long n, extra;
        unsigned char *data = NULL;
        if (XGetWindowProperty(dpy, root, A_NET_ACTIVE_WINDOW, 0, 1, False, XA_WINDOW,
            &rt, &rf, &n, &extra, &data) == Success && data) {
            if (rf == 32 && n == 1) {
                Client *c = find(*(Window *)data);
                if (c && c->ws == curws) focus(c);
            }
            XFree(data);
        }
    }
    /* autostart (non-blocking) */
    {
        const char *xdg = getenv("XDG_CONFIG_HOME");
        const char *home = getenv("HOME");
        char path[512] = "";
        char fallback_autostart[512] = "";
        if (xdg && *xdg) snprintf(path, sizeof(path), "%s/daniwm/autostart.sh", xdg);
        else if (home && *home) snprintf(path, sizeof(path), "%s/.config/daniwm/autostart.sh", home);
        if (home && *home) snprintf(fallback_autostart, sizeof(fallback_autostart), "%s/.config/tilewm/autostart.sh", home);
        /* pick the first existing regular executable (no access()+execl
         * TOCTOU gap: exec permission is re-checked by execl itself). */
        const char *run = NULL;
        {
            struct stat st;
            if (path[0] && stat(path, &st) == 0 && S_ISREG(st.st_mode) && (st.st_mode & 0111))
                run = path;
            else if (fallback_autostart[0] && stat(fallback_autostart, &st) == 0 &&
                S_ISREG(st.st_mode) && (st.st_mode & 0111))
                run = fallback_autostart;
        }
        if (run) {
            struct stat st;
            if (stat(run, &st) == 0 && (st.st_mode & 0022))
                fprintf(stderr, "daniwm: autostart %s is world-writable, check ownership\n", run);
            pid_t pid = fork();
            if (pid == -1) {
                perror("daniwm: fork autostart");
            } else if (pid == 0) {
                if (dpy) close(ConnectionNumber(dpy));
                setsid();
                signal(SIGCHLD, SIG_DFL);
                execl(run, run, NULL);
                fprintf(stderr, "daniwm: autostart exec %s failed: %s\n", run, strerror(errno));
                _exit(1);
            }
        }
    }

    rename_init(); /* self-pipe for async ws_rename results */
    int xfd = ConnectionNumber(dpy);
    int rfd = rename_fd();
    int select_errs = 0;

    /* ---- wheel-volume coalescing ----
     * A fast flick queues several Button4/5 events; spawning one amixer
     * per notch (old behavior) forks a storm and races the setters. Instead
     * accumulate the signed % and flush ONE command (k_vol_delta) once the
     * burst quiets down, then paint an optimistic bar right away while the
     * next 1s tick reconciles with the real sampled value. */
    static int vol_pend = 0;          /* accumulated %, signed; 0 = idle */
    static long long vol_due_ms = 0;  /* CLOCK_MONOTONIC ms when flush is due */
    enum { VOL_STEP = 5, VOL_FLUSH_MS = 60 };
    for (;;) {
        while (!XPending(dpy)) {
            /* 1s tick for clock; shorter sleep while a vol batch is due */
            fd_set rfds; FD_ZERO(&rfds); FD_SET(xfd, &rfds);
            int nfds = xfd + 1;
            if (rfd >= 0) { FD_SET(rfd, &rfds); if (rfd + 1 > nfds) nfds = rfd + 1; }
            int vfd = sys_vol_fd();
            if (vfd >= 0) { FD_SET(vfd, &rfds); if (vfd + 1 > nfds) nfds = vfd + 1; }
            struct timeval tv;
            if (vol_pend) {
                long long rem = vol_due_ms - ms_now();
                if (rem < 0) rem = 0;
                tv.tv_sec = rem / 1000; tv.tv_usec = (long)(rem % 1000) * 1000;
            } else { tv.tv_sec = 1; tv.tv_usec = 0; }
            int ret = select(nfds, &rfds, NULL, NULL, &tv);
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
            if (ret == 0) {
                sys_vol_update(); tray_poll();
                if (vol_pend && ms_now() >= vol_due_ms) {
                    int d = vol_pend; vol_pend = 0;
                    k_vol_delta(d);       /* one spawn for the whole batch */
                    sys_vol_adjust(d);    /* optimistic bar, reconciled next tick */
                }
                drawbar();
            }
            else {
                if (rfd >= 0 && FD_ISSET(rfd, &rfds)) rename_poll();
                if (vfd >= 0 && FD_ISSET(vfd, &rfds)) {
                    sys_vol_read();
                    if (sys_vol_fd() < 0) drawbar(); /* fresh sample */
                }
                if (!XPending(dpy)) continue; /* pipe-only wakeup, no X events */
                break;
            }
        }
        XEvent ev;
        XNextEvent(dpy, &ev);
        last_evtime = 0; /* timestamp only lives for one dispatch (audit-0930 #1) */
        if (rr_present && (ev.type == rr_event_base + RRScreenChangeNotify ||
                            ev.type == rr_event_base + RRNotify)) {
            XRRUpdateConfiguration(&ev);
            on_monitors_changed();
            continue; /* not break: we are before switch(), break would exit for(;;) */
        }
        switch (ev.type) {
        case MapRequest: {
            XMapRequestEvent *e = &ev.xmaprequest;
            if (e->window == bar || e->window == traywin || tray_has(e->window)) break;
            if (tray_on && tray_is_icon_window(e->window)) { tray_add(e->window); break; }
            if (find_dock(e->window)) {
                XMapWindow(dpy, e->window);
                break;
            }
            Client *c = find(e->window);
            if (c) {
                /* Known remap never steals sel: new manages focus via
                 * manage(), but a remapped helper must not yank focus or
                 * monocle visibility. Hover/pager decides focus. */
                if (c->ws == curws && LAYOUT == L_TILE) XMapWindow(dpy, e->window);
                arrange();
                ewmh_active();
            } else manage(e->window);
            hover_lock_arm();
            break;
        }
        case MapNotify:
            if (tray_has(ev.xmap.window)) { tray_handle_map(ev.xmap.window); break; }
            break;
        case UnmapNotify: {
            XUnmapEvent *e = &ev.xunmap;
            if (e->window == bar) break;
            if (tray_has(e->window)) { tray_handle_unmap(e->window); break; }
            if (find_dock(e->window)) {
                unmanage_dock(e->window);
                break;
            }
            Client *c = find(e->window);
            if (!c) break;
            if (e->send_event) { unmanage(e->window); break; }
            /* Plain client-initiated hide: unmanage unless the unmap is
             * WM-initiated (monocle-hidden or ws-hidden windows stay
             * managed). Attribute probe guards spurious events. */
            if (c->hidden || c->ws != curws) break;
            {
                XWindowAttributes a;
                if (XGetWindowAttributes(dpy, e->window, &a) &&
                    a.map_state != IsUnmapped) break;
            }
            unmanage(e->window);
            break;
        }
        case SelectionClear: {
            XSelectionClearEvent *e = &ev.xselectionclear;
            tray_handle_selection_clear(e->selection);
            break;
        }
        case ResizeRequest:
            if (tray_has(ev.xresizerequest.window)) {
                tray_handle_resize(ev.xresizerequest.window);
                break;
            }
            break;
        case ClientMessage: {
            XClientMessageEvent *e = &ev.xclient;
            if (tray_handle_opcode(e)) break;
            Client *c = find(e->window);
            if (e->message_type == A_NET_ACTIVE_WINDOW) {
                if (e->data.l[1]) last_evtime = (Time)e->data.l[1]; /* pager's user time */
                if (c) {
                    /* Parked scratchpad (ws == NWS sentinel) is unmapped:
                     * focusing it would set input to an invisible window.
                     * Ignore the request (pager should unpark first). */
                    if (c->ws >= NWS) break;
                    if (fight_check(c)) break; /* focus duel: urgent only */
                    if (c == sel) {
                        /* already focused: re-assert input, skip the
                         * expensive path (arrange/bar/ewmh spew). */
                        if (client_wants_input(c->win)) {
                            trap_errors(dpy);
                            XSetInputFocus(dpy, c->win, RevertToPointerRoot, wm_time());
                            untrap_errors(dpy);
                        }
                        break;
                    }
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
            if (ev.xdestroywindow.window != bar && ev.xdestroywindow.window != traywin) {
                if (tray_has(ev.xdestroywindow.window)) {
                    tray_remove(ev.xdestroywindow.window);
                    break;
                }
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
                tray_layout_icons();
                break;
            }
            if (e->window == traywin || tray_has(e->window)) {
                tray_handle_resize(e->window);
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
                    clamp_float_geom(&c->fx, &c->fy, &c->fw, &c->fh);
                    /* Never ask the server for a 0/huge window. */
                    if (wc.width < 1 || wc.width > sw * 2) wc.width = c->fw;
                    if (wc.height < 1 || wc.height > sh * 2) wc.height = c->fh;
                }
                XConfigureWindow(dpy, e->window, (unsigned int)e->value_mask, &wc);
            } else {
                XConfigureWindow(dpy, e->window, (e->value_mask & (CWSibling | CWStackMode)), &wc);
                /* ICCCM §4.1.5: tiled windows reject the client's requested
                 * geometry, so send a synthetic ConfigureNotify with the
                 * actual geometry to keep the client in sync. */
                XWindowAttributes ca;
                if (XGetWindowAttributes(dpy, e->window, &ca)) {
                    XEvent cn;
                    memset(&cn, 0, sizeof(cn));
                    cn.xconfigure.type = ConfigureNotify;
                    cn.xconfigure.display = dpy;
                    cn.xconfigure.event = e->window;
                    cn.xconfigure.window = e->window;
                    cn.xconfigure.x = ca.x;
                    cn.xconfigure.y = ca.y;
                    cn.xconfigure.width = ca.width;
                    cn.xconfigure.height = ca.height;
                    cn.xconfigure.border_width = ca.border_width;
                    cn.xconfigure.above = None;
                    cn.xconfigure.override_redirect = False;
                    XSendEvent(dpy, e->window, False, StructureNotifyMask, &cn);
                }
            }
            if (c) arrange();
            break;
        }
        case EnterNotify: {
            XCrossingEvent *e = &ev.xcrossing;
            if (e->window == bar || drag.win != None || find_dock(e->window)) break;
            Client *c = find(e->window);
            /* Chỉ theo con trỏ khi người dùng thật sự di chuột. Mở/đóng
             * grab (menu chuột phải) hay map/unmap cửa sổ dưới con trỏ đều
             * sinh EnterNotify giả. Nhận nó thì menu đóng lại sẽ cướp focus
             * của hộp thoại vừa mở (Save as bị dim vì lý do này).
             * NotifyInferior = con trỏ từ cửa sổ con ra cha, không phải đổi
             * cửa sổ. */
            if (e->mode != NotifyNormal || e->detail == NotifyInferior) break;
            if (hover_locked(e)) break;
            if (FOCUS_MODE == 1) break; /* focus=click: no hover focus */
            last_evtime = e->time; /* hover focus uses its own crossing time */
            /* hover/sloppy focus must not restack: auto-raise here would
             * lift a big floating window over a nested small one as the
             * pointer crosses it, making the small one unreachable.
             * Explicit actions still raise: Mod+click/drag, keys, and
             * plain clicks on floats (observed via XI2 raw events in
             * xi2_raw_click, which never touches the app's press). */
            if (c && c != sel && c->ws == curws) focus_noraise(c);
            break;
        }
        case PropertyNotify: {
            XPropertyEvent *pe = &ev.xproperty;
            if (tray_has(pe->window)) {
                tray_handle_property(pe->window, pe->atom);
                break;
            }
            if (find_dock(pe->window)) {
                if (pe->atom == A_NET_WM_STRUT || pe->atom == A_NET_WM_STRUT_PARTIAL)
                    update_dock_strut(pe->window);
            } else {
                Client *c = find(pe->window);
                if (c) {
                    if (pe->atom == A_NET_WM_STATE) {
                        /* T-M1: direct property writes converge to the same
                         * fullscreen field as the ClientMessage path.
                         * Compare-before-act guards our own ewmh_update_state
                         * writes: they already match, so no loop. */
                        int fs_prop = ewmh_hasstate(c->win, A_NET_WM_STATE_FS);
                        int da_prop = ewmh_hasstate(c->win, A_NET_WM_STATE_DA);
                        if (fs_prop != c->fullscreen) {
                            setfullscreen(c, fs_prop);
                        } else if (da_prop && !c->urgent && c != sel) {
                            set_urgent(c, 1);
                        } else if (!da_prop && c->urgent) {
                            /* DA removal clears unless XUrgencyHint holds. */
                            XWMHints *wmh = XGetWMHints(dpy, c->win);
                            int hint_urg = wmh ? !!(wmh->flags & XUrgencyHint) : 0;
                            if (wmh) XFree(wmh);
                            if (!hint_urg) set_urgent(c, 0);
                        }
                        break;
                    }
                    if (pe->atom == A_NET_WM_STRUT || pe->atom == A_NET_WM_STRUT_PARTIAL) {
                        Window dw = pe->window;
                        unmanage(dw);
                        manage_dock(dw);
                    } else if (pe->atom == XA_WM_HINTS) {
                        /* Mirror hint (set or clear); focused never urgent.
                         * DA-driven urgency is handled in the _NET_WM_STATE
                         * arm above; last write wins on mixed sources. */
                        XWMHints *wmh = XGetWMHints(dpy, pe->window);
                        if (wmh) {
                            int hint_urg = !!(wmh->flags & XUrgencyHint);
                            XFree(wmh);
                            set_urgent(c, hint_urg && c != sel ? 1 : 0);
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
            last_evtime = e->time; /* key-driven focus/kill uses this press */
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
            last_evtime = e->time; /* click-driven focus uses this press */
            if (e->window == bar) {
                /* mute (trái/giữa/phải) chỉ khi bấm trúng cụm volume,
                 * bấm trượt chỗ khác = no-op (trước đây phải/trái bấm đâu cũng mute) */
                int on_vol = (vol_hit_x0 >= 0 && e->x >= vol_hit_x0 && e->x <= vol_hit_x1);
                /* scroll over ws cells cycles workspaces (wraps);
                 * scroll anywhere else = volume, coalesced: notches are
                 * accumulated and applied in ONE backend call once the
                 * burst quiets down (see vol_pend flush in the select
                 * loop) — a fast flick no longer forks per queued event */
                if (e->button == Button4) {
                    if (ws_hit((int)e->x) >= 0) view((curws - 1 + NWS) % NWS);
                    else { vol_pend += VOL_STEP; vol_due_ms = ms_now() + VOL_FLUSH_MS; }
                } else if (e->button == Button5) {
                    if (ws_hit((int)e->x) >= 0) view((curws + 1) % NWS);
                    else { vol_pend -= VOL_STEP; vol_due_ms = ms_now() + VOL_FLUSH_MS; }
                }
                else if (e->button == Button2 || e->button == Button3) {
                    if (on_vol) {
                        if (e->button == Button3) { /* phải: mở pavucontrol */
                            char *pa[] = { "pavucontrol", NULL };
                            spawn(pa);
                        } else k_vol_mute(0); /* giữa: mute */
                    }
                    else bar_task_click(e->x, e->button, e->time); /* mid: close task */
                }
                else {
                    /* left-click on the volume segment mutes; anywhere
                     * else falls through to workspace view as before */
                    if (on_vol) {
                        k_vol_mute(0);
                        break;
                    }
                    /* tasklist: a matched click never falls through. */
                    if (bar_task_click(e->x, e->button, e->time)) break;
                    /* v2: tray container background is dead zone — clicks
                     * there (not on an icon window) must never fall through
                     * to workspace view */
                    { int bx0, bx1; if (tray_box(&bx0, &bx1) && e->x >= bx0) break; }
                    /* variable-width ws cells (named ws grow): hit-test
                     * shares ws_hit() with drawbar so click == pixels */
                    int n = ws_hit((int)e->x);
                    if (n >= 0 && n < NWS) view(n);
                }
            } else if (find_dock(e->window) || find_dock(e->subwindow)) {
                break;
            } else {
                /* grabbed presses report window == client; plain clicks
                 * propagate from root with subwindow == client. Plain
                 * clicks on floating windows are observed via XI2 raw
                 * events instead (see xi_raw_click()), so the app keeps
                 * every button event untouched. */
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
        case GenericEvent: {
            /* XI2 raw button press (broadcast, app untouched):
             * click-to-raise for floating windows. */
            XGenericEventCookie *cook = &ev.xcookie;
            if (cook->extension == xi2_opcode &&
                cook->evtype == XI_RawButtonPress &&
                XGetEventData(dpy, cook)) {
                XIRawEvent *re = (XIRawEvent *)cook->data;
                last_evtime = re->time;
                xi2_raw_click(re->detail);
                XFreeEventData(dpy, cook);
            }
            break;
        }
        case FocusIn: {
            XFocusChangeEvent *e = &ev.xfocus;
            if (e->mode != NotifyNormal || e->detail == NotifyInferior) break;
            Client *c = find(e->window);
            if (!c || c->ws != curws || c->ws < 0 || c->ws >= NWS) break;
            if (c == sel) { ewmh_active(); break; }
            sel = c;
            ws_sel[curws] = c;
            if (c->urgent) set_urgent(c, 0);
            for (Client *t = clients; t; t = t->next)
                if (t->ws == curws)
                    XSetWindowBorder(dpy, t->win, (t == sel) ? BORDER_FOCUS : BORDER_NORMAL);
            drawbar();
            ewmh_active();
            break;
        }
        case FocusOut: {
            /* Do not steal back: focus may rest on root/None or an
             * override_redirect helper. Keep sel as-is; FocusIn (or
             * keys/buttons/pager) re-syncs when input lands again. */
            break;
        }
        case MappingNotify: {
            /* doi keymap (setxkbmap, doi layout, autostart race luc khoi dong):
             * keycode cua phim co the doi -> grab lai toan bo, khong thi
             * vai bind am tham mat grab (thay ro tren Xvfb: thieu p/q). */
            XMappingEvent *e = &ev.xmapping;
            XRefreshKeyboardMapping(e);
            if (e->request == MappingKeyboard || e->request == MappingModifier)
                grabkeys();
            break;
        }
        default: break;
        }
    }
    return 0;
}
