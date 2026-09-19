#include "client.h"

#include <X11/Xutil.h>

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <unistd.h>

#include "bar.h"
#include "ewmh.h"
#include "layout.h"
#include "monitor.h"
#include "mouse.h"
#include "state.h"
#include "tray.h"

/* ---- helpers ---- */
Client *find(Window w) {
    for (Client *c = clients; c; c = c->next)
        if (c->win == w) return c;
    return NULL;
}

int ws_occupied(int n) {
    for (Client *c = clients; c; c = c->next)
        if (c->ws == n) return 1;
    return 0;
}

Client *first_in_ws(int n) {
    for (Client *c = clients; c; c = c->next)
        if (c->ws == n) return c;
    return NULL;
}

int count_tiled(void) {
    int n = 0;
    for (Client *c = clients; c; c = c->next)
    if (c->ws == curws && !c->floating && !c->fullscreen) n++;
    return n;
}

void attach(Client *c) {
    c->next = NULL;
    c->ws = curws;
    if (!clients) clients = c;
    else { Client *t = clients; while (t->next) t = t->next; t->next = c; }
}

void detach(Client *c) {
    Client **p = &clients;
    while (*p && *p != c) p = &(*p)->next;
    if (*p) *p = c->next;
    for (int i = 0; i < NWS; i++)
        if (ws_sel[i] == c)
            ws_sel[i] = NULL;
    if (sel == c) sel = NULL; /* unmanage() handles focus recovery after free */
}

/* swap list positions of a and b: tiling order follows the list, so this
 * swaps their tiles (bspwm-style swap drag). Pointers (sel/ws_sel) name
 * clients, not slots, so they stay valid. Adjacent-aware, missing-safe. */
void swap_order(Client *a, Client *b) {
    Client **pa = NULL, **pb = NULL, **p;
    Client *ta, *tb;
    if (!a || !b || a == b) return;
    for (p = &clients; *p; p = &(*p)->next) {
        if (*p == a) pa = p;
        if (*p == b) pb = p;
    }
    if (!pa || !pb) return;
    ta = a->next; tb = b->next;
    if (ta == b) { a->next = tb; b->next = a; *pa = b; }
    else if (tb == a) { b->next = ta; a->next = b; *pb = a; }
    else { *pa = b; b->next = ta; *pb = a; a->next = tb; }
}

/* ---- actions ---- */
/* Stacking: docks/panels always on top of normal windows, fullscreen above
 * everything (covers bar/panels). Tiled needs no raise (non-overlapping). */
void keep_docks_on_top(void) {
    for (Dock *d = docks; d; d = d->next) XRaiseWindow(dpy, d->win);
    for (Client *c = clients; c; c = c->next)
        if (c->ws == curws && c->fullscreen) XRaiseWindow(dpy, c->win);
}
/* ICCCM WM_HINTS input gate: NoInput windows keep sel/active but never
 * receive XSetInputFocus. Missing hints or missing InputHint means input. */
int client_wants_input(Window w) {
    XWMHints *h = XGetWMHints(dpy, w);
    int want = 1;
    if (h) {
        if (h->flags & InputHint) want = h->input ? 1 : 0;
        XFree(h);
    }
    return want;
}
/* ICCCM WM_TAKE_FOCUS: endorsing clients take input themselves
 * (LocallyActive). Send ClientMessage instead of XSetInputFocus to avoid
 * double-focus. Atoms cached statically here until T-M5A centralizes them.
 * TODO(T-M5B): thread a real event timestamp instead of CurrentTime. */
static int client_takes_focus(Window w) {
    static Atom protos = None, take = None;
    Atom *list = NULL;
    int n = 0, found = 0;
    if (protos == None) protos = XInternAtom(dpy, "WM_PROTOCOLS", False);
    if (take == None) take = XInternAtom(dpy, "WM_TAKE_FOCUS", False);
    if (protos == None || take == None) return 0;
    if (!XGetWMProtocols(dpy, w, &list, &n) || !list) return 0;
    for (int i = 0; i < n; i++)
        if (list[i] == take) { found = 1; break; }
    XFree(list);
    return found;
}
static void send_take_focus(Client *c) {
    static Atom protos = None, take = None;
    XEvent ev;
    if (protos == None) protos = XInternAtom(dpy, "WM_PROTOCOLS", False);
    if (take == None) take = XInternAtom(dpy, "WM_TAKE_FOCUS", False);
    if (protos == None || take == None) return;
    memset(&ev, 0, sizeof(ev));
    ev.xclient.type = ClientMessage;
    ev.xclient.window = c->win;
    ev.xclient.message_type = protos;
    ev.xclient.format = 32;
    ev.xclient.data.l[0] = (long)take;
    ev.xclient.data.l[1] = CurrentTime; /* TODO(T-M5B): real timestamp */
    XSendEvent(dpy, c->win, False, NoEventMask, &ev);
}
/* focus_ex: raise=1 restacks floating/fullscreen on top (explicit actions:
 * Mod+click/drag, keys, manage, pager requests). raise=0 is the
 * hover/sloppy path: auto-raise on Enter would trap a small floating
 * window nested inside a bigger one — crossing the big window raises it
 * over the small one, so the pointer can never reach the small window.
 * Hover therefore never restacks; raise with Mod+click or the keyboard.
 * (Plain clicks go straight to the app: observing them would starve the
 * app of button events, so the WM deliberately stays blind to them.) */
static void focus_ex(Client *c, int raise) {
    if (!c) return;
    sel = c;
    ws_sel[curws] = c;
    if (c->urgent) set_urgent(c, 0);
    if (LAYOUT == L_MONOCLE) arrange(); /* show only sel */
    else {
        for (Client *t = clients; t; t = t->next)
            if (t->ws == curws)
                XSetWindowBorder(dpy, t->win, (t == sel) ? BORDER_FOCUS : BORDER_NORMAL);
        drawbar();
    }
    if (raise && (c->floating || c->fullscreen)) {
        XRaiseWindow(dpy, c->win);
        keep_docks_on_top();
    }
    if (client_takes_focus(c->win)) {
        /* LocallyActive/GloballyActive: client takes input itself after
         * the message; never double-focus with XSetInputFocus. */
        send_take_focus(c);
        ewmh_active();
        return;
    }
    if (client_wants_input(c->win))
        XSetInputFocus(dpy, c->win, RevertToPointerRoot, CurrentTime);
    ewmh_active();
}
void focus(Client *c) {
    focus_ex(c, 1);
}
void focus_noraise(Client *c) {
    focus_ex(c, 0);
}

void focus_step(int dir) {
    Client *first = first_in_ws(curws);
    if (!first) return;
    if (!sel || sel->ws != curws) { focus(first); return; }
    if (dir > 0) {
        /* next in same ws, wrap */
        Client *t = sel->next;
        while (t && t->ws != curws) t = t->next;
        focus(t ? t : first);
    } else {
        Client *prev = NULL;
        for (Client *t = clients; t && t != sel; t = t->next)
            if (t->ws == curws) prev = t;
        if (!prev) { /* wrap to last */
            for (Client *t = clients; t; t = t->next)
                if (t->ws == curws) prev = t;
        }
        focus(prev);
    }
}

void view(int n) {
    if (n < 0 || n >= NWS || n == curws) return;
    ws_sel[curws] = sel;
    int old = curws;
    prevws = old;
    curws = n;
    sel = ws_sel[n] && find(ws_sel[n]->win) && ws_sel[n]->ws == n ? ws_sel[n] : first_in_ws(n);
    ewmh_desktops();
    arrange();  /* maps new workspace's windows */
    for (Client *c = clients; c; c = c->next)
        if (c->ws == old) XUnmapWindow(dpy, c->win);
    if (sel) focus(sel);
    else {
        XSetInputFocus(dpy, root, RevertToPointerRoot, CurrentTime);
        ewmh_active();
        drawbar();
    }
}

void send_to(int n) {
    if (!sel || n < 0 || n >= NWS || n == curws) return;
    Client *s = sel;
    int old = curws;
    Client *next = NULL; /* focus fallback cho ws cũ */
    for (Client *t = clients; t; t = t->next)
        if (t != s && t->ws == old) { next = t; break; }
    ws_sel[old] = next;
    s->ws = n;
    ws_sel[n] = s;
    ewmh_set_wm_desktop(s);
    prevws = old;
    curws = n;
    sel = s;
    ewmh_desktops();
    arrange(); /* map new workspace windows first, like view() */
    for (Client *c = clients; c; c = c->next)
        if (c->ws == old) XUnmapWindow(dpy, c->win);
    focus(s); /* follow: nhảy theo luôn */
}
/* external pager move: same as send_to but stays on current ws */
void move_to(Client *c, int n) {
    int old;
    if (!c || n < 0 || n >= NWS || n == c->ws) return;
    old = c->ws;
    if (old >= 0 && old < NWS && ws_sel[old] == c) {
        Client *nx = NULL;
        for (Client *t = clients; t; t = t->next)
            if (t != c && t->ws == old) { nx = t; break; }
        ws_sel[old] = nx;
    }
    c->ws = n;
    if (!ws_sel[n]) ws_sel[n] = c;
    ewmh_set_wm_desktop(c);
    if (old == curws && n != curws) {
        XUnmapWindow(dpy, c->win);
        if (sel == c) {
            Client *nx = first_in_ws(curws);
            sel = NULL;
            arrange();
            if (nx) focus(nx);
            else {
                XSetInputFocus(dpy, root, RevertToPointerRoot, CurrentTime);
                ewmh_active();
                drawbar();
            }
        } else arrange();
    } else if (n == curws) {
        XMapWindow(dpy, c->win);
        arrange();
        focus(c);
    } else arrange();
}

/* Super+Tab: back-and-forth between current and last workspace.
 * view() swaps prevws/curws, so repeated toggles bounce back. */
void ws_toggle(int unused) {
    (void)unused;
    if (prevws < 0 || prevws >= NWS || prevws == curws) return;
    view(prevws);
}

void kill_client(Client *c) {
    Atom *protos = NULL, del;
    int n = 0, i, has_delete = 0;
    XEvent ev;
    if (!c) return;
    del = XInternAtom(dpy, "WM_DELETE_WINDOW", False);
    if (XGetWMProtocols(dpy, c->win, &protos, &n)) {
        for (i = 0; i < n; i++)
            if (protos[i] == del) { has_delete = 1; break; }
    }
    if (protos) XFree(protos);
    if (!has_delete) { XKillClient(dpy, c->win); return; }
    memset(&ev, 0, sizeof(ev));
    ev.xclient.type = ClientMessage;
    ev.xclient.window = c->win;
    ev.xclient.message_type = XInternAtom(dpy, "WM_PROTOCOLS", True);
    ev.xclient.format = 32;
    ev.xclient.data.l[0] = (long)del;
    ev.xclient.data.l[1] = CurrentTime;
    XSendEvent(dpy, c->win, False, NoEventMask, &ev);
}
/* double-kill within 2s force-kills hung windows (L4) */
static Window last_kill_win = None;
static time_t last_kill_time = 0;
void kill_sel(void) {
    if (!sel) return;
    time_t now = time(NULL);
    if (sel->win == last_kill_win && now - last_kill_time <= 2) {
        XKillClient(dpy, sel->win);
        last_kill_win = None;
        return;
    }
    last_kill_win = sel->win;
    last_kill_time = now;
    kill_client(sel);
}

void spawn(char **argv) {
    if (!argv || !argv[0]) return;
    pid_t pid = fork();
    if (pid == -1) { perror("daniwm: fork"); return; }
    if (pid == 0) {
        if (dpy) close(ConnectionNumber(dpy));
        setsid();
        signal(SIGCHLD, SIG_DFL);
        execvp(argv[0], (char *const *)argv);
        fprintf(stderr, "daniwm: exec %s failed\n", argv[0]);
        _exit(1);
    }
}

void toggle_floating_sel(void) {
    if (!sel) return;
    sel->floating = !sel->floating;
    if (sel->floating) {
        if (sel->fw <= 0 || sel->fh <= 0) {
            XWindowAttributes wa;
            if (XGetWindowAttributes(dpy, sel->win, &wa)) {
                sel->fx = wa.x; sel->fy = wa.y; sel->fw = wa.width; sel->fh = wa.height;
            }
        } else {
            XMoveResizeWindow(dpy, sel->win, sel->fx, sel->fy, (unsigned)sel->fw, (unsigned)sel->fh);
        }
        XRaiseWindow(dpy, sel->win);
        keep_docks_on_top();
    }
    arrange();
    focus(sel);
}

/* zoom (dwm-style): focused tiled window becomes master of its (ws, mon).
 * Already master -> swap with 2nd tiled (toggle). Floating/fullscreen: no-op. */
void zoom(int unused) {
    Client *first = NULL, *second = NULL, *target;
    Client **pp;
    (void)unused;
    if (!sel || sel->floating || sel->fullscreen || sel->ws != curws) return;
    for (Client *c = clients; c; c = c->next) {
        if (c->ws != curws || c->mon != sel->mon || c->floating || c->fullscreen) continue;
        if (!first) first = c;
        else { second = c; break; }
    }
    if (!first || !second) return; /* 0-1 tiled: nothing to swap */
    target = (sel == first) ? second : sel;
    if (target->ws != curws || target->mon != sel->mon ||
        target->floating || target->fullscreen) return;
    if (target == first) return;
    pp = &clients;
    while (*pp && *pp != target) pp = &(*pp)->next;
    if (!*pp) return;
    *pp = target->next; /* unlink */
    pp = &clients;
    while (*pp && *pp != first) pp = &(*pp)->next;
    if (!*pp) { /* first vanished mid-op: re-append to keep list valid */
        pp = &clients;
        while (*pp) pp = &(*pp)->next;
        target->next = NULL;
        *pp = target;
    } else {
        target->next = first;
        *pp = target;
    }
    ewmh_client_list();
    arrange();
    focus(sel);
}

void quit(void) {
    tray_enable(0); /* un-embed tray icons back to root before disconnect */
    XCloseDisplay(dpy);
    exit(0);
}

/* restart-in-place: replace this process with a fresh daniwm binary without
 * killing clients. Windows stay mapped (reparented to root on disconnect);
 * the new instance re-manages them, restoring workspace via _NET_WM_DESKTOP,
 * fullscreen via _NET_WM_STATE, curws via _NET_CURRENT_DESKTOP, focus via
 * _NET_ACTIVE_WINDOW, and the parked scratchpad via its sticky hint.
 * Floating state and per-workspace layout/mfact reset to defaults. */
void restart(void) {
    tray_enable(0); /* un-embed tray icons back to root before disconnect */
    XSync(dpy, False);
    if (dpy) XCloseDisplay(dpy);
    /* /proc/self/exe always points at the running file, so a freshly `make`d
     * binary is picked up even when we were started via a relative path. */
    char exe[1024] = "";
    ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (n > 0) { exe[n] = '\0'; execl(exe, exe, (char *)NULL); }
    if (progpath[0]) execl(progpath, progpath, (char *)NULL);
    execlp("daniwm", "daniwm", (char *)NULL);
    fprintf(stderr, "daniwm: restart exec failed: %s\n", strerror(errno));
    _exit(1);
}

/* ---- rules + scratchpad ---- */
static void matchrules(Window w, int *floating, int *ws) {
    XClassHint ch = { 0 };
    char *name = NULL;
    int gotclass = XGetClassHint(dpy, w, &ch);
    if (!XFetchName(dpy, w, &name)) name = NULL; /* Xlib may leave it untouched on failure */
    for (unsigned i = 0; i < nrules; i++) {
        int cm = !rules[i].cls ||
            (gotclass && ((ch.res_class && strstr(ch.res_class, rules[i].cls)) ||
                          (ch.res_name && strstr(ch.res_name, rules[i].cls))));
        int tm = !rules[i].title || (name && strstr(name, rules[i].title));
        if (cm && tm) {
            *floating = rules[i].floating;
            if (rules[i].ws >= 0 && rules[i].ws < NWS) *ws = rules[i].ws;
        }
    }
    if (ch.res_class) XFree(ch.res_class);
    if (ch.res_name) XFree(ch.res_name);
    if (name) XFree(name);
}

/* scratchpad windows always start centered at 2/3 of the work area.
 * At MapRequest time xterm/alacritty still report a tiny placeholder
 * geometry (1x1), so using a.width/a.height here makes the first
 * show tiny while later toggles (k_scratch) force 2/3. */
static int isscratchpad(Window w) {
    XClassHint ch = { 0 };
    char *name = NULL;
    int m = 0;
    if (XGetClassHint(dpy, w, &ch)) {
        m = (ch.res_name && strstr(ch.res_name, "scratchpad")) ||
            (ch.res_class && strstr(ch.res_class, "scratchpad"));
        if (ch.res_name) XFree(ch.res_name);
        if (ch.res_class) XFree(ch.res_class);
        if (m) return 1;
    }
    if (XFetchName(dpy, w, &name)) {
        if (name && strstr(name, "scratchpad")) m = 1;
        if (name) XFree(name);
    }
    return m;
}

/* ---- manage ---- */
void manage(Window w) {
    if (w == bar || w == traywin || tray_has(w)) return;
    if (tray_on && tray_is_icon_window(w)) { tray_add(w); return; }
    XWindowAttributes a;
    if (!XGetWindowAttributes(dpy, w, &a) || a.override_redirect) return;
    if (find(w) || find_dock(w)) return;
    if (ewmh_isdock(w)) {
        manage_dock(w);
        return;
    }
    Client *c = calloc(1, sizeof(Client));
    if (!c) return; /* OOM: leave the window unmanaged, WM keeps running */
    c->win = w;
    c->mon = mon_by_pointer();
    int rulefloat = 0, rulews = curws;
    matchrules(w, &rulefloat, &rulews);
    { int d = ewmh_read_desktop(w); if (d >= 0 && d < NWS) rulews = d; }
    Window trans = None;
    c->fx = a.x; c->fy = a.y; c->fw = a.width; c->fh = a.height;
    c->cfact = 1.0f;
    if (XGetTransientForHint(dpy, w, &trans) || ewmh_isfloating_type(w) || rulefloat) {
        c->floating = 1;
        int ax, ay, aw, ah;
        getarea(c->mon, &ax, &ay, &aw, &ah);
        int fw, fh;
        if (isscratchpad(w)) { fw = aw * 2 / 3; fh = ah * 2 / 3; }
        else { fw = a.width > 0 ? a.width : aw / 2; fh = a.height > 0 ? a.height : ah / 2; }
        c->fx = ax + (aw - fw) / 2;
        c->fy = ay + (ah - fh) / 2;
        c->fw = fw;
        c->fh = fh;
        XMoveResizeWindow(dpy, w, c->fx, c->fy, (unsigned)c->fw, (unsigned)c->fh);
    }
    if (ewmh_hasstate(w, A_NET_WM_STATE_FS)) c->fullscreen = 1;
    if (ewmh_hasstate(w, A_NET_WM_STATE_DA)) c->urgent = 1;
    else {
        XWMHints *wmh = XGetWMHints(dpy, w);
        if (wmh) {
            if (wmh->flags & XUrgencyHint) c->urgent = 1;
            XFree(wmh);
        }
    }
    XSelectInput(dpy, w, EnterWindowMask | FocusChangeMask | PropertyChangeMask | StructureNotifyMask);
    grabbuttons(c);
    XSetWindowBorderWidth(dpy, w, (unsigned)S(BORDER));
    attach(c);
    ewmh_set_wm_state(c, NormalState);
    c->ws = rulews;
    ewmh_client_list();
    ewmh_set_wm_desktop(c);
    if (c->ws == curws) {
        XMapWindow(dpy, w);
        focus(c);
    }
    arrange();
}

void unmanage(Window w) {
    Client *c = find(w);
    if (!c) return;
    if (drag.win == w) {
        drag.win = None;
        drag.mode = 0;
        XUngrabPointer(dpy, CurrentTime);
    }
    detach(c);
    ewmh_set_wm_state(c, WithdrawnState);
    free(c);
    ewmh_client_list();
    arrange();
    if (sel) focus(sel);
    else if (first_in_ws(curws)) focus(first_in_ws(curws));
    else {
        XSetInputFocus(dpy, root, RevertToPointerRoot, CurrentTime);
        ewmh_active();
        drawbar();
    }
}
