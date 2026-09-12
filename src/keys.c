#include "keys.h"

#include <X11/Xutil.h>
#include <X11/XF86keysym.h>
#include <X11/keysym.h>

#include <stdlib.h>
#include <string.h>

#include "bar.h"
#include "client.h"
#include "ewmh.h"
#include "layout.h"
#include "monitor.h"
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
static void k_scratch(int) {
    Client *s = findscratch();
    if (!s) { spawn(scratchcmd); return; }
    XWindowAttributes a;
    XGetWindowAttributes(dpy, s->win, &a);
    if (s->ws == curws && a.map_state == IsViewable) {
        s->ws = NWS; /* park: hidden, ignored by all ws loops */
        ewmh_set_wm_desktop(s); /* → sticky 0xFFFFFFFF */
        XUnmapWindow(dpy, s->win);
        sel = NULL;
        Client *n = first_in_ws(curws);
        arrange();
        if (n) focus(n);
        else drawbar();
    } else {
        s->ws = curws;
        ewmh_set_wm_desktop(s);
        s->mon = mon_by_pointer();
        s->floating = 1;
        s->fullscreen = 0;
        int ax, ay, aw, ah;
        getarea(s->mon, &ax, &ay, &aw, &ah);
        int fw = aw * 2 / 3, fh = ah * 2 / 3;
        XMoveResizeWindow(dpy, s->win, ax + (aw - fw) / 2, ay + (ah - fh) / 2, (unsigned)fw, (unsigned)fh);
        XMapRaised(dpy, s->win);
        focus(s);
        arrange();
    }
}

/* ---- keys ---- */
Key *keys = NULL;
unsigned nkeys = 0;
static unsigned capkeys = 0;

static void k_focusnext(int) { focus_step(+1); }
static void k_focusprev(int) { focus_step(-1); }
static void k_kill(int)      { kill_sel(); }
static void k_tile(int)      { ws_layout[curws] = L_TILE; arrange(); }
static void k_monocle(int)   { ws_layout[curws] = L_MONOCLE; arrange(); }
static void k_toggle(int)    { ws_layout[curws] = (LAYOUT == L_TILE ? L_MONOCLE : L_TILE); arrange(); }
static void k_spawnterm(int) { spawn(termcmd); }
static void k_spawnmenu(int) { spawn(menucmd); }
static void k_quit(int)      { quit(); }
static void k_float(int)     { toggle_floating_sel(); }
static void k_mfactdec(int)  { MFACT -= 0.025f; if (MFACT < 0.1f) MFACT = 0.1f; arrange(); }
static void k_mfactinc(int)  { MFACT += 0.025f; if (MFACT > 0.9f) MFACT = 0.9f; arrange(); }
static void k_nmasterdec(int){ if (NMASTER > 1) NMASTER--; arrange(); }
static void k_nmasterinc(int){ if (NMASTER < 8) NMASTER++; arrange(); } /* cap 8, like config */
static void k_gap(int)       { gaps_on = !gaps_on; arrange(); }
static void k_gapdec(int)    { gap_outer = gap_outer >= 2 ? gap_outer - 2 : 0; if (gap_inner > 0) gap_inner -= 1; arrange(); }
static void k_gapinc(int)    { gap_outer += 2; gap_inner += 1; arrange(); }
static void k_bar(int) {
    bar_on = !bar_on;
    if (bar_on) XMapWindow(dpy, bar);
    else XUnmapWindow(dpy, bar);
    update_struts();
    arrange();
}
static void k_fullscreen(int) { if (sel) setfullscreen(sel, !sel->fullscreen); }
/* Volume interaction: amixer, fire-and-forget. vol_ts = 0 forces the 1s tick
 * to re-sample so the bar refreshes promptly (no blocking sample here). */
static char *vol_up_am[]   = { "amixer", "set", "Master", "5%+", NULL };
static char *vol_down_am[] = { "amixer", "set", "Master", "5%-", NULL };
static char *vol_mute_am[] = { "amixer", "set", "Master", "toggle", NULL };
static char *vol_up_wp[]   = { "wpctl", "set-volume", "@DEFAULT_AUDIO_SINK@", "5%+", NULL };
static char *vol_down_wp[] = { "wpctl", "set-volume", "@DEFAULT_AUDIO_SINK@", "5%-", NULL };
static char *vol_mute_wp[] = { "wpctl", "set-mute", "@DEFAULT_AUDIO_SINK@", "toggle", NULL };
static char *vol_up_pa[]   = { "pactl", "set-sink-volume", "@DEFAULT_SINK@", "+5%", NULL };
static char *vol_down_pa[] = { "pactl", "set-sink-volume", "@DEFAULT_SINK@", "-5%", NULL };
static char *vol_mute_pa[] = { "pactl", "set-sink-mute", "@DEFAULT_SINK@", "toggle", NULL };
void k_vol_up(int)   { spawn(vol_set_cmd(vol_up_am, vol_up_wp, vol_up_pa)); vol_ts = 0; }
void k_vol_down(int) { spawn(vol_set_cmd(vol_down_am, vol_down_wp, vol_down_pa)); vol_ts = 0; }
void k_vol_mute(int) { spawn(vol_set_cmd(vol_mute_am, vol_mute_wp, vol_mute_pa)); vol_ts = 0; }
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
    XResizeWindow(dpy, sel->win, (unsigned)sel->fw, (unsigned)sel->fh);
    XFlush(dpy);
}
static void k_move_left(int)  { float_moveby(-S(FLOAT_STEP), 0); }
static void k_move_right(int) { float_moveby(S(FLOAT_STEP), 0); }
static void k_move_up(int)    { float_moveby(0, -S(FLOAT_STEP)); }
static void k_move_down(int)  { float_moveby(0, S(FLOAT_STEP)); }
static void k_rsz_w_dec(int)  { float_rszby(-S(RSZ_STEP), 0); }
static void k_rsz_w_inc(int)  { float_rszby(S(RSZ_STEP), 0); }
static void k_rsz_h_dec(int)  { float_rszby(0, -S(RSZ_STEP)); }
static void k_rsz_h_inc(int)  { float_rszby(0, S(RSZ_STEP)); }
const KeyAction actions[] = {
    { "focus_next", k_focusnext }, { "focus_prev", k_focusprev },
    { "kill", k_kill }, { "tile", k_tile }, { "monocle", k_monocle },
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
    { "reload_config", k_reload },
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
    push_key_fn(XK_q, M, k_kill, 0);
    push_key_fn(XK_t, M, k_tile, 0);
    push_key_fn(XK_m, M, k_monocle, 0);
    push_key_fn(XK_space, M, k_toggle, 0);
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
    /* floating move (vim keys) + resize (with shift); repeat = smooth */
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
    push_key_fn(XK_f, M, k_fullscreen, 0);
    push_key_fn(XK_s, M, k_scratch, 0);
}
void keys_reset(void) {
    nkeys = 0;
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
