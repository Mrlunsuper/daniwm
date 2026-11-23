#include "keys.h"
#include "overview.h"
#include "animation-settings.h"
#include "layout-animation.h"
#include "animation-lifecycle.h"
#include "workspace-animation.h"
#include "overview-animation.h"
#include "xerr.h"
#include <X11/Xatom.h>
#include <time.h>

#include <X11/Xutil.h>
#include <X11/XF86keysym.h>
#include <X11/keysym.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "bar.h"
#include "client.h"
#include "ewmh.h"
#include "layout.h"
#include "monitor.h"
#include "rename.h"
#include "sysmon.h"

/* ---- rules + scratchpad ---- */
static Client *findscratch(void) {
    for (Client *c = clients; c; c = c->next) {
        XClassHint ch = { 0 };
        if (!XGetClassHint(dpy, c->win, &ch)) continue;
        int m = (ch.res_name && strstr(ch.res_name, "scratchpad")) ||
                (ch.res_class && strstr(ch.res_class, "scratchpad"));
        if (ch.res_name) XFree(ch.res_name);
        if (ch.res_class) XFree(ch.res_class);
        if (m) return c;
    }
    return NULL;
}
/* Store an XID, not a Client pointer: the previous window may close
 * while the scratchpad is open. Validate its workspace before restoring. */
static Window scratch_return;
static void k_scratch(int unused) {
    (void)unused;
    Client *s = findscratch();
    if (!s) {
        scratch_return = sel && sel->ws == curws ? sel->win : None;
        spawn(scratchcmd);
        return;
    }
    XWindowAttributes a;
    if (!XGetWindowAttributes(dpy, s->win, &a)) { unmanage(s->win); return; }
    if (s->ws == curws && a.map_state == IsViewable) {
        s->ws = NWS; /* park: hidden, ignored by all ws loops */
        ewmh_set_wm_desktop(s); /* → sticky 0xFFFFFFFF */
        ewmh_client_list(); /* drop parked window from _NET_CLIENT_LIST */
        animation_unmap(s->win, ANIMATION_HIDE_SCRATCH);
        Client *n = sel != s ? sel : find(scratch_return);
        if (!n || n->ws != curws) n = first_in_ws(curws);
        scratch_return = None;
        sel = n;
        ws_sel[curws] = n;
        arrange();
        if (n) focus(n);
        else {
            XSetInputFocus(dpy, root, RevertToPointerRoot, wm_time());
            ewmh_active();
            drawbar();
        }
    } else {
        scratch_return = sel && sel != s && sel->ws == curws ? sel->win : None;
        s->ws = curws;
        ewmh_set_wm_desktop(s);
        ewmh_client_list(); /* re-add unparked window to _NET_CLIENT_LIST */
        s->mon = mon_by_pointer();
        s->floating = 1;
        s->fullscreen = 0;
        int ax, ay, aw, ah;
        getarea(s->mon, &ax, &ay, &aw, &ah);
        int fw = aw * 2 / 3, fh = ah * 2 / 3;
        s->fx = ax + (aw - fw) / 2; s->fy = ay + (ah - fh) / 2;
        s->fw = fw; s->fh = fh;
        XMoveResizeWindow(dpy, s->win, s->fx, s->fy, (unsigned)fw, (unsigned)fh);
        XMapRaised(dpy, s->win);
        focus(s);
        arrange();
    }
}

/* ---- custom exec binds: bind = mod+x:exec some-cmd --flag ---- */
static char ***exec_list = NULL;
static unsigned nexec = 0, capexec = 0;
void k_exec(int idx) {
    if (idx < 0 || (unsigned)idx >= nexec) return;
    spawn(exec_list[idx]);
}
int push_exec_cmd(char **argv) {
    if (!argv || !argv[0]) return -1;
    if (nexec == capexec) {
        unsigned nc = capexec ? capexec * 2 : 8;
        char ***nl = realloc(exec_list, nc * sizeof(*nl));
        if (!nl) return -1;
        exec_list = nl; capexec = nc;
    }
    exec_list[nexec] = argv;
    return (int)(nexec++);
}

/* ---- keys ---- */
Key *keys = NULL;
unsigned nkeys = 0;
static unsigned capkeys = 0;

static void k_focusnext(int unused) { (void)unused; focus_step(+1); }
static void k_focusprev(int unused) { (void)unused; focus_step(-1); }
static void k_kill(int unused)      { (void)unused; kill_sel(); }
static void k_tile(int unused)      { (void)unused; ws_layout[curws] = L_TILE; arrange(); }
static void k_monocle(int unused)   { (void)unused; ws_layout[curws] = L_MONOCLE; arrange(); }
static void k_toggle(int unused)    { (void)unused; ws_layout[curws] = (LAYOUT == L_TILE ? L_MONOCLE : L_TILE); arrange(); }
static void k_spawnterm(int unused) { (void)unused; spawn(termcmd); }
static void k_spawnmenu(int unused) { (void)unused; spawn(menucmd); }
static void k_quit(int unused)      { (void)unused; quit(); }
static void k_restart(int unused)   { (void)unused; restart(); }
static void k_float(int unused)     { (void)unused; toggle_floating_sel(); }
static void k_mfactdec(int unused)  { (void)unused; MFACT -= 0.025f; if (MFACT < 0.1f) MFACT = 0.1f; arrange(); }
static void k_mfactinc(int unused)  { (void)unused; MFACT += 0.025f; if (MFACT > 0.9f) MFACT = 0.9f; arrange(); }
static void k_nmasterdec(int unused){ (void)unused; if (NMASTER > 1) NMASTER--; arrange(); }
static void k_nmasterinc(int unused){ (void)unused; if (NMASTER < 8) NMASTER++; arrange(); } /* cap 8, like config */
static void k_gap(int unused)       { (void)unused; gaps_on = !gaps_on; arrange(); }
static void k_gapdec(int unused)    { (void)unused; gap_outer = gap_outer >= 2 ? gap_outer - 2 : 0; if (gap_inner > 0) gap_inner -= 1; arrange(); }
static void k_gapinc(int unused) { /* capped like the config range (#8) */
    (void)unused;
    if (gap_outer < 64) gap_outer += 2;
    if (gap_inner < 64) gap_inner += 1;
    arrange();
}
static void k_bar(int unused) {
    (void)unused;
    bar_on = !bar_on;
    if (bar_on) XMapWindow(dpy, bar);
    else XUnmapWindow(dpy, bar);
    update_struts();
    arrange();
}
static void k_fullscreen(int unused) { (void)unused; if (sel) setfullscreen(sel, !sel->fullscreen); }
static Atom animation_hide_atom;
static unsigned long monitor_epoch = 1;
/* Settings only, not a geometry/capture protocol. The WM never waits for
 * an acknowledgment or restarts a compositor on animation-setting reload. */
void animation_settings_publish(void) {
    if (!dpy || !checkwin) return;
    if (!animation_hide_atom) animation_hide_atom = XInternAtom(dpy, ANIMATION_REASON_ATOM, False);
    unsigned long values[ANIMATION_SETTINGS_WORDS] = {
        ANIMATION_SETTINGS_VERSION, checkwin, COMPOSITOR && ANIMATIONS,
        (unsigned long)REDUCE_MOTION, (unsigned long)ANIMATION_OPEN_MS
    };
    XChangeProperty(dpy, root, XInternAtom(dpy, ANIMATION_SETTINGS_ATOM, False),
        XA_CARDINAL, 32, PropModeReplace, (unsigned char *)values, ANIMATION_SETTINGS_WORDS);
    unsigned long closing[] = {1, checkwin, (unsigned long)ANIMATION_CLOSE_MS};
    XChangeProperty(dpy, root, XInternAtom(dpy, ANIMATION_CLOSE_ATOM, False),
        XA_CARDINAL, 32, PropModeReplace, (unsigned char *)closing, 3);
    unsigned long layout[] = {LAYOUT_ANIMATION_SETTINGS_VERSION, checkwin,
        (unsigned long)ANIMATION_LAYOUT_MS};
    XChangeProperty(dpy, root, XInternAtom(dpy, LAYOUT_ANIMATION_SETTINGS_ATOM, False),
        XA_CARDINAL, 32, PropModeReplace, (unsigned char *)layout, 3);
    unsigned long workspace[] = {WORKSPACE_ANIMATION_VERSION, checkwin,
        (unsigned long)ANIMATION_WORKSPACE_MS};
    XChangeProperty(dpy, root, XInternAtom(dpy, WORKSPACE_SETTINGS_ATOM, False),
        XA_CARDINAL, 32, PropModeReplace, (unsigned char *)workspace, 3);
    unsigned long overview[] = {OVERVIEW_ANIMATION_VERSION, checkwin,
        (unsigned long)ANIMATION_OVERVIEW_MS};
    XChangeProperty(dpy, root, XInternAtom(dpy, OVERVIEW_SETTINGS_ATOM, False),
        XA_CARDINAL, 32, PropModeReplace, (unsigned char *)overview, 3);
    XChangeProperty(dpy, root, XInternAtom(dpy, MONITOR_EPOCH_ATOM, False),
        XA_CARDINAL, 32, PropModeReplace, (unsigned char *)&monitor_epoch, 1);
    XFlush(dpy);
}
/* Mark the overview window for dani-comp before it maps; None clears it. */
void animation_overview_role(Window overlay) {
    if (!dpy || !checkwin) return;
    Atom atom = XInternAtom(dpy, OVERVIEW_ROLE_ATOM, False);
    if (!overlay) { XDeleteProperty(dpy, root, atom); return; }
    unsigned long role[] = {OVERVIEW_ANIMATION_VERSION, checkwin, overlay};
    XChangeProperty(dpy, root, atom, XA_CARDINAL, 32, PropModeReplace,
        (unsigned char *)role, 3);
}
/* Monitor geometry changed: any workspace image from before is stale. */
void animation_monitor_epoch_bump(void) {
    if (++monitor_epoch == 0) ++monitor_epoch;
    if (!dpy || !checkwin) return;
    XChangeProperty(dpy, root, XInternAtom(dpy, MONITOR_EPOCH_ATOM, False),
        XA_CARDINAL, 32, PropModeReplace, (unsigned char *)&monitor_epoch, 1);
}
/* Before a workspace switch publishes the desktop: tell dani-comp which
 * viewable clients leave. Never reads from or waits for the compositor;
 * any doubt means no PREPARE, so the switch stays instant. */
void animation_workspace_prepare(int old, int new) {
    static Atom atom;
    static uint32_t sequence;
    if (!dpy || !checkwin || !COMPOSITOR || !ANIMATIONS || REDUCE_MOTION ||
        ANIMATION_WORKSPACE_MS <= 0) return;
    unsigned long words[WORKSPACE_ANIMATION_WORDS];
    unsigned long count = 0;
    for (Client *c = clients; c; c = c->next) {
        if ((c->ws == old || c->ws == new) && c->fullscreen) return;
        if (c->ws != old || c->hidden) continue;
        if (count == WORKSPACE_ANIMATION_MEMBERS) return;
        words[WORKSPACE_ANIMATION_HEADER + count++] = c->win;
    }
    if (!atom) atom = XInternAtom(dpy, WORKSPACE_ANIMATION_ATOM, False);
    if (++sequence == 0) ++sequence;
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    words[0] = WORKSPACE_ANIMATION_VERSION; words[1] = checkwin; words[2] = monitor_epoch;
    words[3] = sequence; words[4] = (unsigned long)old; words[5] = (unsigned long)new;
    words[6] = (uint32_t)(now.tv_sec * 1000 + now.tv_nsec / 1000000);
    words[7] = count;
    XChangeProperty(dpy, root, atom, XA_CARDINAL, 32, PropModeReplace,
        (unsigned char *)words, (int)(WORKSPACE_ANIMATION_HEADER + count));
    XEvent event = {0};
    event.xclient.type = ClientMessage; event.xclient.window = root;
    event.xclient.message_type = atom; event.xclient.format = 32;
    event.xclient.data.l[0] = WORKSPACE_ANIMATION_VERSION;
    event.xclient.data.l[1] = (long)checkwin;
    event.xclient.data.l[2] = (long)sequence;
    XSendEvent(dpy, root, False, SubstructureNotifyMask, &event);
}
/* Same-connection ordering guarantees this notification is enqueued before
 * the unmap event. No capture, acknowledgment, XSync or server grab. */
void animation_unmap(Window window, unsigned reason) {
    static uint32_t sequence;
    if (COMPOSITOR && ANIMATIONS && !REDUCE_MOTION &&
        (ANIMATION_CLOSE_MS > 0 || ANIMATION_OPEN_MS > 0 || ANIMATION_WORKSPACE_MS > 0) &&
        checkwin && animation_hide_atom) {
        /* RFC1982-style serial ordering; zero is reserved. */
        if (++sequence == 0) ++sequence;
        {
            XEvent event = {0};
            event.xclient.type = ClientMessage; event.xclient.window = root;
            event.xclient.message_type = animation_hide_atom; event.xclient.format = 32;
            event.xclient.data.l[0] = ANIMATION_REASON_VERSION;
            event.xclient.data.l[1] = (long)checkwin;
            event.xclient.data.l[2] = (long)window;
            event.xclient.data.l[3] = (long)sequence;
            event.xclient.data.l[4] = (long)reason;
            XSendEvent(dpy, root, False, SubstructureNotifyMask, &event);
        }
    }
    XUnmapWindow(dpy, window);
}
static Window compositor_owner(void) {
    char cm[32];
    snprintf(cm, sizeof(cm), "_NET_WM_CM_S%d", screen);
    return XGetSelectionOwner(dpy, XInternAtom(dpy, cm, False));
}
void compositor_start(void) {
    if (!COMPOSITOR || compositor_owner() != None) return;
    pid_t pid = fork();
    if (pid == -1) return;
    if (pid == 0) {
        if (dpy) close(ConnectionNumber(dpy));
        setsid();
        char self[1152], sibling[1152];
        snprintf(self, sizeof(self), "%s", progpath[0] ? progpath : "dani-comp");
        char *slash = strrchr(self, '/');
        if (slash) { size_t dl = (size_t)(slash - self) + 1; memcpy(sibling, self, dl); snprintf(sibling + dl, sizeof(sibling) - dl, "dani-comp"); }
        else snprintf(sibling, sizeof(sibling), "dani-comp");
        execl(sibling, "dani-comp", "--shadow", "--no-fade", "--dim", "1", "--wm-settings", NULL);
        execlp("dani-comp", "dani-comp", "--shadow", "--no-fade", "--dim", "1", "--wm-settings", NULL);
        _exit(1);
    }
}
static void k_compositor(int unused) {
    (void)unused;
    Window owner = compositor_owner();
    if (!owner) {
        /* Explicit action may start it even when automatic startup is off;
         * compositor=0 still keeps WM-controlled fades disabled. */
        int enabled = COMPOSITOR;
        COMPOSITOR = 1;
        compositor_start();
        COMPOSITOR = enabled;
        return;
    }
    /* Never pkill every dani-comp process or disturb a foreign compositor.
     * A version marker permits a stop request addressed to this owner only. */
    Atom control = XInternAtom(dpy, ANIMATION_CONTROL_ATOM, False);
    Atom type; int format; unsigned long n = 0, extra = 0;
    unsigned char *data = NULL;
    trap_errors(dpy);
    int ok = XGetWindowProperty(dpy, owner, control, 0, 1, False, XA_CARDINAL,
        &type, &format, &n, &extra, &data) == Success && type == XA_CARDINAL &&
        format == 32 && n == 1 && !extra && data &&
        *(unsigned long *)data == ANIMATION_SETTINGS_VERSION;
    if (data) XFree(data);
    if (ok) {
        XEvent event = {0};
        event.xclient.type = ClientMessage; event.xclient.window = owner;
        event.xclient.message_type = control; event.xclient.format = 32;
        event.xclient.data.l[0] = ANIMATION_SETTINGS_VERSION;
        XSendEvent(dpy, owner, False, NoEventMask, &event);
    }
    untrap_errors(dpy);
}
/* Volume interaction: fire-and-forget setters. vol_ts = 0 forces the 1s
 * tick to re-sample so the bar refreshes promptly (no blocking sample here).
 * k_vol_delta() applies an arbitrary accumulated step in ONE spawn: the
 * wheel path coalesces a scroll burst into a single call, so a fast flick
 * doesn't fork an amixer/wpctl/pactl per queued ButtonPress. */
static char *vol_mute_am[] = { "amixer", "set", "Master", "toggle", NULL };
static char *vol_mute_wp[] = { "wpctl", "set-mute", "@DEFAULT_AUDIO_SINK@", "toggle", NULL };
static char *vol_mute_pa[] = { "pactl", "set-sink-mute", "@DEFAULT_SINK@", "toggle", NULL };
void k_vol_delta(int delta) {
    if (!delta) return;
    int n = delta > 0 ? delta : -delta;
    char step_pw[16], step_pa[16]; /* amixer/wpctl: "N%+" ; pactl: "+N%" */
    snprintf(step_pw, sizeof step_pw, "%d%%%c", n, delta > 0 ? '+' : '-');
    snprintf(step_pa, sizeof step_pa, "%c%d%%", delta > 0 ? '+' : '-', n);
    char *am[] = { "amixer", "set", "Master", step_pw, NULL };
    char *wp[] = { "wpctl", "set-volume", "@DEFAULT_AUDIO_SINK@", step_pw, NULL };
    char *pa[] = { "pactl", "set-sink-volume", "@DEFAULT_SINK@", step_pa, NULL };
    spawn(vol_set_cmd(am, wp, pa));
    vol_ts = 0;
}
void k_vol_up(int unused)   { (void)unused; k_vol_delta(5); }
void k_vol_down(int unused) { (void)unused; k_vol_delta(-5); }
void k_vol_mute(int unused) { (void)unused; spawn(vol_set_cmd(vol_mute_am, vol_mute_wp, vol_mute_pa)); vol_ts = 0; }
/* Keyboard float move/resize: 20px steps, key repeat = smooth.
 * Tiled windows promote to floating first (same as mouse drag). */
static void float_promote(void) {
    XWindowAttributes wa;
    if (!sel || sel->fullscreen || sel->floating) return;
    if (XGetWindowAttributes(dpy, sel->win, &wa)) {
        sel->fx = wa.x; sel->fy = wa.y; sel->fw = wa.width; sel->fh = wa.height;
    }
    sel->floating = 1;
    XRaiseWindow(dpy, sel->win);
    keep_docks_on_top();
}
static void float_moveby(int dx, int dy) {
    if (!sel || sel->fullscreen) return;
    float_promote();
    if (!sel->floating) return;
    sel->fx += dx; sel->fy += dy;
    sel->mon = mon_at(sel->fx + sel->fw / 2, sel->fy + sel->fh / 2);
    XMoveWindow(dpy, sel->win, sel->fx, sel->fy);
    XFlush(dpy);
}
static void float_rszby(int dw, int dh) {
    if (!sel || sel->fullscreen) return;
    float_promote();
    if (!sel->floating) return;
    sel->fw += dw; if (sel->fw < 50) sel->fw = 50;
    sel->fh += dh; if (sel->fh < 50) sel->fh = 50;
    clamp_float_geom(&sel->fx, &sel->fy, &sel->fw, &sel->fh);
    XResizeWindow(dpy, sel->win, (unsigned)sel->fw, (unsigned)sel->fh);
    XFlush(dpy);
}
/* Directional move for tiled windows (master-stack aware):
 *   up/down  reorder inside the current column (swap with the neighbour)
 *   left     stack -> master (swap with the first master)
 *   right    master -> top of the stack
 * Index of the wanted tiling position, or NULL when the move is a no-op
 * (edge of the column, stack with no stack column, etc.). */
static Client *tiled_at_index(Client *c, int want) {
    int n = 0;
    for (Client *t = clients; t; t = t->next)
        if (t->ws == c->ws && t->mon == c->mon && !t->floating && !t->fullscreen) {
            if (n == want) return t;
            n++;
        }
    return NULL;
}
static Client *tiled_neighbor(Client *c, int dir) {
    int n = 0, idx = -1;
    for (Client *t = clients; t; t = t->next)
        if (t->ws == c->ws && t->mon == c->mon && !t->floating && !t->fullscreen) {
            if (t == c) idx = n;
            n++;
        }
    if (idx < 0 || n < 2) return NULL;
    int nm = NMASTER < n ? NMASTER : n;
    int col0 = (idx < nm) ? 0 : nm;   /* first index of c's column */
    int col1 = (idx < nm) ? nm : n;   /* one past c's column */
    int want = -1;
    switch (dir) {
    case 0: if (idx > col0) want = idx - 1; break;          /* up   */
    case 1: if (idx < col1 - 1) want = idx + 1; break;      /* down */
    case 2: if (idx >= nm) want = 0; break;                 /* left */
    case 3: if (idx < nm && n > nm) want = nm; break;       /* right */
    }
    return (want < 0) ? NULL : tiled_at_index(c, want);
}
/* dir: 0 up, 1 down, 2 left, 3 right. Floating keeps the 20px move;
 * tiled swaps tiling position instead (no promote). */
static void move_dir(int dir) {
    if (!sel || sel->fullscreen) return;
    if (sel->floating) {
        float_moveby(dir == 2 ? -S(FLOAT_STEP) : dir == 3 ? S(FLOAT_STEP) : 0,
                     dir == 0 ? -S(FLOAT_STEP) : dir == 1 ? S(FLOAT_STEP) : 0);
        return;
    }
    Client *t = tiled_neighbor(sel, dir);
    if (!t) return;
    swap_order(sel, t);
    arrange();
    XFlush(dpy);
}
static void k_move_left(int unused)  { (void)unused; move_dir(2); }
static void k_move_right(int unused) { (void)unused; move_dir(3); }
static void k_move_up(int unused)    { (void)unused; move_dir(0); }
static void k_move_down(int unused)  { (void)unused; move_dir(1); }
static void k_rsz_w_dec(int unused)  { (void)unused; float_rszby(-S(RSZ_STEP), 0); }
static void k_rsz_w_inc(int unused)  { (void)unused; float_rszby(S(RSZ_STEP), 0); }
static void k_rsz_h_dec(int unused)  { (void)unused; float_rszby(0, -S(RSZ_STEP)); }
static void k_rsz_h_inc(int unused)  { (void)unused; float_rszby(0, S(RSZ_STEP)); }
const KeyAction actions[] = {
    { "focus_next", k_focusnext }, { "focus_prev", k_focusprev },
    { "zoom", zoom }, { "ws_toggle", ws_toggle }, { "kill", k_kill }, { "tile", k_tile }, { "monocle", k_monocle },
    { "overview_toggle", overview_toggle }, { "overview_show", overview_show }, { "overview_hide", overview_hide },
    { "toggle", k_toggle }, { "spawn_term", k_spawnterm },
    { "spawn_menu", k_spawnmenu }, { "quit", k_quit }, { "float", k_float },
    { "mfact_dec", k_mfactdec }, { "mfact_inc", k_mfactinc },
    { "nmaster_dec", k_nmasterdec }, { "nmaster_inc", k_nmasterinc },
    { "move_left", k_move_left }, { "move_right", k_move_right },
    { "move_up", k_move_up }, { "move_down", k_move_down },
    { "resize_w_dec", k_rsz_w_dec }, { "resize_w_inc", k_rsz_w_inc },
    { "resize_h_dec", k_rsz_h_dec }, { "resize_h_inc", k_rsz_h_inc },
    { "gap", k_gap }, { "gap_dec", k_gapdec }, { "gap_inc", k_gapinc },
    { "bar", k_bar },
    { "fullscreen", k_fullscreen }, { "scratch", k_scratch },
    { "vol_up", k_vol_up }, { "vol_down", k_vol_down }, { "vol_mute", k_vol_mute },
    { "reload_config", k_reload }, { "restart", k_restart },
    { "compositor", k_compositor },
    { "ws_rename", k_wsrename },
};
const unsigned nactions = sizeof(actions) / sizeof(actions[0]);

void push_key_fn(KeySym ks, unsigned int mod, void (*fn)(int), int arg) {
    if (nkeys == capkeys) {
        unsigned nc = capkeys ? capkeys * 2 : 32;
        Key *nk = realloc(keys, nc * sizeof(*nk));
        if (!nk) return;
        keys = nk; capkeys = nc;
    }
    keys[nkeys].keysym = ks; keys[nkeys].mod = mod; keys[nkeys].fn = fn; keys[nkeys].arg = arg; nkeys++;
}
void add_default_keys(void) {
    unsigned int M = MOD;
    push_key_fn(XK_j, M, k_focusnext, 0);
    push_key_fn(XK_k, M, k_focusprev, 0);
    push_key_fn(XK_z, M, zoom, 0);
    push_key_fn(XK_Tab, M, ws_toggle, 0);
    push_key_fn(XK_q, M, k_kill, 0);
    push_key_fn(XK_t, M, k_tile, 0);
    push_key_fn(XK_m, M, k_monocle, 0);
    push_key_fn(XK_space, M, k_toggle, 0);
    push_key_fn(XK_o, M, overview_toggle, 0);
    push_key_fn(XK_Return, M, k_spawnterm, 0);
    push_key_fn(XK_p, M, k_spawnmenu, 0);
    push_key_fn(XK_h, M, k_mfactdec, 0);
    push_key_fn(XK_l, M, k_mfactinc, 0);
    push_key_fn(XK_u, M, k_nmasterdec, 0);
    push_key_fn(XK_i, M, k_nmasterinc, 0);
    push_key_fn(XK_g, M, k_gap, 0);
    push_key_fn(XK_minus, M, k_gapdec, 0);
    push_key_fn(XK_equal, M, k_gapinc, 0);
    push_key_fn(XK_b, M, k_bar, 0);
    /* directional move (vim keys): tiled = swap, floating = 20px; repeat = smooth */
    push_key_fn(XK_h, M | ControlMask, k_move_left, 0);
    push_key_fn(XK_j, M | ControlMask, k_move_down, 0);
    push_key_fn(XK_k, M | ControlMask, k_move_up, 0);
    push_key_fn(XK_l, M | ControlMask, k_move_right, 0);
    push_key_fn(XK_h, M | ControlMask | ShiftMask, k_rsz_w_dec, 0);
    push_key_fn(XK_l, M | ControlMask | ShiftMask, k_rsz_w_inc, 0);
    push_key_fn(XK_k, M | ControlMask | ShiftMask, k_rsz_h_dec, 0);
    push_key_fn(XK_j, M | ControlMask | ShiftMask, k_rsz_h_inc, 0);
    /* laptop media keys, no modifier */
    push_key_fn(XF86XK_AudioRaiseVolume, 0, k_vol_up, 0);
    push_key_fn(XF86XK_AudioLowerVolume, 0, k_vol_down, 0);
    push_key_fn(XF86XK_AudioMute, 0, k_vol_mute, 0);
    for (int i = 0; i < NWS; i++) {
        KeySym ks = (i < 9) ? (KeySym)(XK_1 + i) : XK_0;
        push_key_fn(ks, M, view, i);
        push_key_fn(ks, M | ShiftMask, send_to, i);
    }
    push_key_fn(XK_space, M | ShiftMask, k_float, 0);
    push_key_fn(XK_e, M | ShiftMask, k_quit, 0);
    push_key_fn(XK_r, M | ShiftMask, k_reload, 0);
    push_key_fn(XK_r, M | ControlMask, k_restart, 0);
    push_key_fn(XK_f, M, k_fullscreen, 0);
    push_key_fn(XK_s, M, k_scratch, 0);
    push_key_fn(XK_n, M | ShiftMask, k_wsrename, 0);
    /* power menu (dani-run --power). argv heap-alloc vi keys_reset() free
     * tung string + array khi reload (khong dung static/literal). */
    {
        char **av = malloc(3 * sizeof(*av));
        if (av) {
            av[0] = strdup("dani-run");
            av[1] = strdup("--power");
            av[2] = NULL;
            if (!av[0] || !av[1]) {
                free(av[0]); free(av[1]); free(av);
            } else {
                int idx = push_exec_cmd(av);
                if (idx < 0) { free(av[0]); free(av[1]); free(av); }
                else push_key_fn(XK_p, M | ShiftMask, k_exec, idx);
            }
        }
    }
}
void keys_reset(void) {
    nkeys = 0;
    for (unsigned i = 0; i < nexec; i++) {
        for (char **p = exec_list[i]; *p; p++) free(*p);
        free(exec_list[i]);
    }
    nexec = 0;
}
void grabkeys(void) {
    unsigned int masks[] = { 0, LockMask, Mod2Mask, LockMask|Mod2Mask };
    XUngrabKey(dpy, AnyKey, AnyModifier, root);
    for (unsigned i = 0; i < nkeys; i++) {
        KeyCode code = XKeysymToKeycode(dpy, keys[i].keysym);
        if (!code) continue;
        for (unsigned m = 0; m < sizeof(masks)/sizeof(masks[0]); m++)
            XGrabKey(dpy, code, keys[i].mod | masks[m], root, True, GrabModeAsync, GrabModeAsync);
    }
}
