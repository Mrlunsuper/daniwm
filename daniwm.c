/* daniwm - minimal X11 tiling WM: tiling + monocle, bar, workspaces, gaps.
 * Build: gcc -O2 -o daniwm daniwm.c -lX11
 * Run  : echo "exec /path/to/daniwm" >> ~/.xinitrc  (or test: Xephyr :1 & DISPLAY=:1 ./daniwm)
 */
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/XKBlib.h>
#include <X11/Xatom.h>
#include <X11/keysym.h>
#include <X11/XF86keysym.h>
#include <X11/Xft/Xft.h>
#include <X11/extensions/Xinerama.h>
#include <X11/extensions/Xrandr.h>
#include <X11/cursorfont.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <time.h>
#include <wordexp.h>
#include <sys/select.h>
#include <signal.h>
#include <errno.h>

/* ---- config (mutable; defaults restored on reload, then file applied) ---- */
typedef struct { char *cls; char *title; int floating; int ws; } Rule;
static Rule *rules = NULL;
static unsigned nrules = 0, caprules = 0;
static char **termcmd = NULL;
static char **menucmd = NULL;
static char **scratchcmd = NULL;
static unsigned int MOD = Mod4Mask;
static int BORDER = 2;
static unsigned long BORDER_FOCUS  = 0x7aa2f7;
static unsigned long BORDER_NORMAL = 0x333333;
static unsigned long BAR_BG  = 0x1e1e2e;
static unsigned long BAR_FG  = 0xcdd6f4;
static unsigned long BAR_ACC = 0x7aa2f7;
static unsigned long BAR_DIM = 0x6c7086;
/* per-component overrides; 0 + h_*_set=0 → fall back to the base colors */
static unsigned long C_WS_ACT, C_WS_ACT_TX, C_WS_OCC, C_WS_EMP, C_MODE, C_TITLE, C_SYS;
static int h_ws_act, h_ws_act_tx, h_ws_occ, h_ws_emp, h_mode, h_title, h_sys;
static int BAR_H = 24;
static int WS_W = 40;  /* width per workspace box in bar */
static float ui_scale = 1.0f; /* manual HiDPI scale: multiplies chrome only */
/* scale raw UI px -> device px; v<=0 stays 0 so border 0 / gap 0 keep meaning */
static int S(int v) { if (v <= 0) return 0; return (int)((float)v * ui_scale + 0.5f); }
static char *font_name = NULL; /* X font (XLFD) for bar, default "fixed" */
static int bar_on = 1;
static int gaps_on = 1;
static int gap_outer = 10;
static int gap_inner = 8;
static float def_mfact = 0.55f;
static int def_nmaster = 1;
#define MAXWS 10
static int NWS = 5; /* live workspace count, 1..MAXWS; arrays below sized NWS */

/* ---- state ---- */
typedef struct Client Client;
struct Client {
    Window win;
    int ws;       /* workspace index 0..NWS-1 */
    int floating;
    int fullscreen; /* EWMH fullscreen: fill area, no gaps/border */
    int hidden;     /* monocle: unmapped but managed (_NET_WM_STATE_HIDDEN) */
    int urgent;     /* demands attention: EWMH or WM_HINTS urgency */
    int mon;        /* monitor index */
    int fx, fy, fw, fh; /* saved floating geometry */
    Client *next;
};

typedef enum { L_TILE = 0, L_MONOCLE = 1 } Layout;

static Display *dpy;
static Window root, bar, checkwin;
static Pixmap barpm = None;
static GC bargc;
/* bar text via Xft (UTF-8, fontconfig); shapes still use the core GC */
static XftFont *barfont = NULL;
static XftFont *barfont_fbs[4];
static int bar_nfb = 0; /* fallbacks for glyphs missing in barfont */
static XftDraw *barxd = NULL;
typedef struct { XftColor bg, ws_act, ws_acttx, ws_occ, ws_emp, mode, title, sys; } BarColors;
static BarColors barcol;
static int barcol_ok = 0;
/* sysmon state */
static unsigned long long cpu_prev_total = 0, cpu_prev_idle = 0;
static char vol_cache[16] = "";
static time_t vol_ts = 0;
static int screen, sw, sh;
static Client *clients = NULL;
static Client *sel = NULL;
static Client **ws_sel = NULL;
static int curws = 0;
static Layout *ws_layout = NULL;
static float *ws_mfact = NULL;
static int *ws_nmaster = NULL;
static int nws_alloc = 0; /* array size currently allocated */
#define MAXMONS 16
static struct { int x, y, w, h; } mons[MAXMONS];
static int nmons = 1;
/* RandR hotplug: event base when XRRQueryExtension succeeds, else off */
static int rr_event_base = 0, rr_error_base = 0, rr_present = 0;
static void initmons(void);
static int mon_at(int x, int y);
static void getarea(int m, int *ax, int *ay, int *aw, int *ah);
static void bar_style(void);
static void update_struts(void);
static void arrange(void);
static void on_monitors_changed(void);
typedef struct { int left, right, top, bottom; } StrutMargin;
static StrutMargin mon_struts[MAXMONS];

typedef struct Dock Dock;
struct Dock {
    Window win;
    int has_strut;
    unsigned long strut[12];
    Dock *next;
};
static Dock *docks = NULL;
static int barw = 0; /* bar width (= mons[0].w) */
/* EWMH atoms + prototypes (impl before manage) */
static Atom A_NET_SUPPORTED, A_NET_CLIENT_LIST, A_NET_ACTIVE_WINDOW,
    A_NET_WM_STATE, A_NET_WM_STATE_FS, A_NET_WM_STATE_HIDDEN,
    A_NET_WM_STATE_DA, A_NET_WM_WINDOW_TYPE,
    A_NET_WM_WINDOW_TYPE_DIALOG, A_NET_WM_WINDOW_TYPE_DOCK,
    A_NET_WM_WINDOW_TYPE_TOOLBAR, A_NET_WM_WINDOW_TYPE_SPLASH,
    A_NET_WM_WINDOW_TYPE_UTILITY, A_NET_CLOSE_WINDOW,
    A_NET_SUPPORTING_WM_CHECK, A_NET_WM_NAME,
    A_NET_WM_STRUT, A_NET_WM_STRUT_PARTIAL, A_NET_WORKAREA,
    A_NET_NUMBER_OF_DESKTOPS, A_NET_CURRENT_DESKTOP,
    A_NET_WM_DESKTOP, A_NET_DESKTOP_NAMES;
static void ewmh_init(void);
static void ewmh_client_list(void);
static void ewmh_active(void);
static void ewmh_desktops(void);
static void ewmh_set_wm_desktop(Client *c);
static void setfullscreen(Client *c, int fs);
static void ewmh_update_state(Client *c);
static void set_urgent(Client *c, int urg);
static int ws_has_urgent(int n);
static void screen_extents(long *x0, long *y0, long *x1, long *y1);
static void update_struts(void);
static Dock *find_dock(Window w);
static void manage_dock(Window w);
static void unmanage_dock(Window w);
static void update_dock_strut(Window w);

#define LAYOUT (ws_layout[curws])
#define MFACT  (ws_mfact[curws])
#define NMASTER (ws_nmaster[curws])

static void arrange(void);
static void drawbar(void);
static void focus(Client *c);
static void view(int n);
static void send_to(int n);
static void kill_sel(void);
static void spawn(char **argv);
static void quit(void);
static void grabbuttons(Client *c);

/* ---- helpers ---- */
static Client *find(Window w) {
    for (Client *c = clients; c; c = c->next)
        if (c->win == w) return c;
    return NULL;
}

static int ws_occupied(int n) {
    for (Client *c = clients; c; c = c->next)
        if (c->ws == n) return 1;
    return 0;
}

static Client *first_in_ws(int n) {
    for (Client *c = clients; c; c = c->next)
        if (c->ws == n) return c;
    return NULL;
}

static int count_tiled(void) {
    int n = 0;
    for (Client *c = clients; c; c = c->next)
    if (c->ws == curws && !c->floating && !c->fullscreen) n++;
    return n;
}

static void attach(Client *c) {
    c->next = NULL;
    c->ws = curws;
    if (!clients) clients = c;
    else { Client *t = clients; while (t->next) t = t->next; t->next = c; }
}

static void detach(Client *c) {
    Client **p = &clients;
    while (*p && *p != c) p = &(*p)->next;
    if (*p) *p = c->next;
    for (int i = 0; i < NWS; i++)
        if (ws_sel[i] == c)
            ws_sel[i] = NULL;
    if (sel == c) sel = NULL; /* unmanage() handles focus recovery after free */
}

/* ---- monitors (Xinerama; fallback: whole screen) ---- */
static void initmons(void) {
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
static int mon_at(int x, int y) {
    for (int i = 0; i < nmons; i++)
        if (x >= mons[i].x && x < mons[i].x + mons[i].w &&
            y >= mons[i].y && y < mons[i].y + mons[i].h) return i;
    return 0;
}
static int mon_by_pointer(void) {
    Window r, c;
    int x, y, wx, wy;
    unsigned m;
    if (XQueryPointer(dpy, root, &r, &c, &x, &y, &wx, &wy, &m))
        return mon_at(x, y);
    return 0;
}
/* usable area of monitor m: minus bar (mon 0), struts, and outer gaps.
 * BAR/gaps are raw config values scaled by ui_scale; struts stay physical. */
static void getarea(int m, int *ax, int *ay, int *aw, int *ah) {
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
static void on_monitors_changed(void) {
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
        }
    }
    if (bar) XMoveResizeWindow(dpy, bar, mons[0].x, mons[0].y, (unsigned)barw, (unsigned)S(BAR_H));
    bar_style(); /* rebuilds pixmap at new barw */
    update_struts();
    arrange();
}
/* ---- layouts (per monitor) ---- */
static void tile_mon(int m) {
    int n = 0;
    for (Client *c = clients; c; c = c->next)
        if (c->ws == curws && c->mon == m && !c->floating && !c->fullscreen) n++;
    if (n == 0) return;
    int ax, ay, aw, ah;
    getarea(m, &ax, &ay, &aw, &ah);
    int g = gaps_on ? S(gap_inner) : 0;
    int nm = NMASTER < n ? NMASTER : n;
    int mw = (n > nm) ? (int)((float)aw * MFACT) : aw;
    int i = 0, my = ay, sy = ay;
    int bw = S(BORDER);
    for (Client *c = clients; c; c = c->next) {
        if (c->ws != curws || c->mon != m || c->floating || c->fullscreen) continue;
        if (i < nm) {
            int h = (ay + ah - my) / (nm - i);
            int ww = mw - 2 * bw - g, wh = h - 2 * bw - g;
            if (ww < 1) ww = 1;
            if (wh < 1) wh = 1;
            XMoveResizeWindow(dpy, c->win,
                ax + g / 2, my + g / 2,
                (unsigned)ww, (unsigned)wh);
            my += h;
        } else {
            int ns = n - nm, si = i - nm;
            int h = (ay + ah - sy) / (ns - si);
            int ww = aw - mw - 2 * bw - g, wh = h - 2 * bw - g;
            if (ww < 1) ww = 1;
            if (wh < 1) wh = 1;
            XMoveResizeWindow(dpy, c->win,
                ax + mw + g / 2, sy + g / 2,
                (unsigned)ww, (unsigned)wh);
            sy += h;
        }
        XMapWindow(dpy, c->win);
        i++;
    }
}
static void tile(void) {
    for (int m = 0; m < nmons; m++) tile_mon(m);
    /* tiling: all windows visible, clear any monocle-set hidden state */
    for (Client *c = clients; c; c = c->next)
        if (c->ws == curws && !c->floating && !c->fullscreen && c->hidden) {
            c->hidden = 0; ewmh_update_state(c);
        }
}
static void monocle_mon(int m) {
    int ax, ay, aw, ah;
    getarea(m, &ax, &ay, &aw, &ah);
    Client *show = NULL;
    if (sel && sel->ws == curws && sel->mon == m &&
        !sel->floating && !sel->fullscreen) show = sel;
    if (!show)
        for (Client *c = clients; c; c = c->next)
            if (c->ws == curws && c->mon == m && !c->floating && !c->fullscreen) { show = c; break; }
    for (Client *c = clients; c; c = c->next) {
        if (c->ws != curws || c->mon != m || c->floating || c->fullscreen) continue;
        if (c == show) {
            XMoveResizeWindow(dpy, c->win, ax, ay, (unsigned)(aw - 2 * S(BORDER)), (unsigned)(ah - 2 * S(BORDER)));
            XMapWindow(dpy, c->win);
            if (c->hidden) { c->hidden = 0; ewmh_update_state(c); }
        } else {
            XUnmapWindow(dpy, c->win);
            if (!c->hidden) { c->hidden = 1; ewmh_update_state(c); }
        }
    }
    for (Client *c = clients; c; c = c->next)
        if (c->ws == curws && c->mon == m && c->floating) XMapRaised(dpy, c->win);
}
static void monocle(void) {
    for (int m = 0; m < nmons; m++) monocle_mon(m);
}
static void arrange(void) {
    if (LAYOUT == L_MONOCLE) monocle();
    else tile();
    for (int m = 0; m < nmons; m++) {
        int ax, ay, aw, ah;
        getarea(m, &ax, &ay, &aw, &ah);
        for (Client *c = clients; c; c = c->next) {
            if (c->ws != curws || c->mon != m) continue;
            if (c->fullscreen) { /* overlay: whole monitor, covers bar */
                XSetWindowBorderWidth(dpy, c->win, 0);
                XMoveResizeWindow(dpy, c->win, mons[m].x, mons[m].y, (unsigned)mons[m].w, (unsigned)mons[m].h);
                XMapWindow(dpy, c->win);
                XRaiseWindow(dpy, c->win);
                continue;
            }
            if (c->floating) {
                XMapRaised(dpy, c->win);
            }
            XSetWindowBorderWidth(dpy, c->win, (unsigned)S(BORDER));
            XSetWindowBorder(dpy, c->win, (c == sel) ? BORDER_FOCUS : BORDER_NORMAL);
        }
    }
    for (Dock *d = docks; d; d = d->next)
        XRaiseWindow(dpy, d->win);
    for (Client *c = clients; c; c = c->next) {
        if (c->ws == curws && c->fullscreen)
            XRaiseWindow(dpy, c->win);
    }
    XSync(dpy, False);
    drawbar();
}

/* ---- sysmon: cpu/mem/bat/vol, no extra deps ---- */
static int sys_cpu(void) { /* 0..100, -1 = unknown */
    FILE *f = fopen("/proc/stat", "r");
    if (!f) return -1;
    unsigned long long u, n, s, id, io, ir, so, st;
    if (fscanf(f, "cpu %llu %llu %llu %llu %llu %llu %llu %llu",
        &u, &n, &s, &id, &io, &ir, &so, &st) != 8) { fclose(f); return -1; }
    fclose(f);
    unsigned long long idle = id + io;
    unsigned long long total = u + n + s + id + io + ir + so + st;
    unsigned long long dt = total - cpu_prev_total, di = idle - cpu_prev_idle;
    cpu_prev_total = total; cpu_prev_idle = idle;
    if (dt == 0) return -1;
    return (int)(100 * (dt - di) / dt);
}
static int sys_mem(void) { /* used % 0..100, -1 = unknown */
    FILE *f = fopen("/proc/meminfo", "r");
    if (!f) return -1;
    long total = 0, avail = 0;
    char k[64]; long v;
    while (fscanf(f, "%63s %ld", k, &v) == 2) {
        if (!strcmp(k, "MemTotal:")) total = v;
        else if (!strcmp(k, "MemAvailable:")) avail = v;
        if (total && avail) break;
        /* skip rest of line (kB unit) */
        int ch; while ((ch = fgetc(f)) != '\n' && ch != EOF) {}
    }
    fclose(f);
    if (total <= 0) return -1;
    return (int)(100 * (total - avail) / total);
}
static int sys_bat(char *chg, size_t n) { /* capacity %, chg="+"/"-"/" " */
    const char *bats[] = { "BAT0", "BAT1", NULL };
    for (int i = 0; bats[i]; i++) {
        char pcap[128], pstat[128];
        snprintf(pcap, sizeof(pcap), "/sys/class/power_supply/%s/capacity", bats[i]);
        snprintf(pstat, sizeof(pstat), "/sys/class/power_supply/%s/status", bats[i]);
        FILE *f = fopen(pcap, "r");
        if (!f) continue;
        int cap = -1;
        if (fscanf(f, "%d", &cap) != 1) { fclose(f); continue; }
        fclose(f);
        char st[32] = "";
        f = fopen(pstat, "r");
        if (f) { if (!fgets(st, sizeof(st), f)) st[0] = 0; fclose(f); }
        if (chg && n) snprintf(chg, n, "%s", strstr(st, "Charg") ? "+" : (strstr(st, "Discharg") ? "-" : ""));
        return cap;
    }
    return -1;
}
/* Volume: cached string, never blocks. The blocking sample runs only
 * from the 1s tick via sys_vol_update(); drawbar() just reads the cache.
 * First second after startup shows no volume until the first tick fills it.
 * Backend: amixer → wpctl (PipeWire) → pactl (Pulse), first success sticks
 * (vol_backend) so later ticks probe only one tool; total failure backs
 * off 30s. vol_set_cmd() picks the matching setter for keys/bar clicks. */
static int vol_backend = 0; /* 0=unknown, 1=amixer, 2=wpctl, 3=pactl */
static const char *sys_vol(void) { /* "40%" / "MUTE" / "" */
    return vol_cache[0] ? vol_cache : "";
}
static int vol_try_amixer(void) {
    FILE *p = popen("amixer get Master 2>/dev/null | grep -oE '\\[[0-9]+%\\]|\\[(on|off)\\]' | head -n2", "r");
    char pct[16] = "", st[16] = "";
    char line[32];
    if (!p) return 0;
    while (fgets(line, sizeof(line), p)) {
        if (strchr(line, '%')) snprintf(pct, sizeof(pct), "%.10s", line + 1);
        else if (line[1] == 'o' || line[1] == 'O') snprintf(st, sizeof(st), "%.8s", line + 1);
    }
    pclose(p);
    if (!pct[0]) return 0;
    char *e = strchr(pct, '%');
    if (e) e[1] = 0;
    if ((st[0] == 'o' && !strstr(st, "on")) || strstr(st, "off"))
        snprintf(vol_cache, sizeof(vol_cache), "MUTE");
    else snprintf(vol_cache, sizeof(vol_cache), "%.10s", pct);
    return 1;
}
static int vol_try_wpctl(void) {
    /* "Volume: 0.75" or "Volume: 0.75 [MUTED]" */
    FILE *p = popen("wpctl get-volume @DEFAULT_AUDIO_SINK@ 2>/dev/null", "r");
    char line[64];
    if (!p) return 0;
    if (!fgets(line, sizeof(line), p)) { pclose(p); return 0; }
    pclose(p);
    float v = -1;
    if (sscanf(line, "Volume: %f", &v) != 1 || v < 0) return 0;
    if (strstr(line, "MUTED")) snprintf(vol_cache, sizeof(vol_cache), "MUTE");
    else snprintf(vol_cache, sizeof(vol_cache), "%d%%", (int)(v * 100 + 0.5f));
    return 1;
}
static int vol_try_pactl(void) {
    FILE *p = popen("pactl get-sink-volume @DEFAULT_SINK@ 2>/dev/null", "r");
    char line[256];
    int pct = -1;
    if (!p) return 0;
    while (fgets(line, sizeof(line), p)) {
        char *pc = strchr(line, '%');
        if (pc) { char *q = pc; while (q > line && isdigit((unsigned char)q[-1])) q--; pct = atoi(q); break; }
    }
    pclose(p);
    if (pct < 0) return 0;
    p = popen("pactl get-sink-mute @DEFAULT_SINK@ 2>/dev/null", "r");
    if (p) {
        if (fgets(line, sizeof(line), p) && strstr(line, "yes")) pct = -2;
        pclose(p);
    }
    if (pct == -2) snprintf(vol_cache, sizeof(vol_cache), "MUTE");
    else snprintf(vol_cache, sizeof(vol_cache), "%d%%", pct);
    return 1;
}
static void sys_vol_update(void) {
    time_t now = time(NULL);
    int ok = 0;
    if (now < vol_ts) return;
    /* preferred backend first, then the rest */
    if (vol_backend == 2) ok = vol_try_wpctl() ? 2 : 0;
    else if (vol_backend == 3) ok = vol_try_pactl() ? 3 : 0;
    else if (vol_backend == 1) ok = vol_try_amixer() ? 1 : 0;
    if (!ok) {
        if (vol_backend != 1 && vol_try_amixer()) ok = 1;
        else if (vol_backend != 2 && vol_try_wpctl()) ok = 2;
        else if (vol_backend != 3 && vol_try_pactl()) ok = 3;
    }
    if (ok) { vol_backend = ok; vol_ts = now + 2; }
    else { vol_cache[0] = 0; vol_backend = 0; vol_ts = now + 30; }
}
static char **vol_set_cmd(char **am, char **wp, char **pa) {
    return vol_backend == 2 ? wp : vol_backend == 3 ? pa : am;
}
/* ---- bar ---- */
static void xft_alloc(unsigned long hex, XftColor *c) {
    XRenderColor rc = {
        (unsigned short)(((hex >> 16) & 0xff) * 257),
        (unsigned short)(((hex >> 8) & 0xff) * 257),
        (unsigned short)((hex & 0xff) * 257),
        0xffff
    };
    if (!XftColorAllocValue(dpy, DefaultVisual(dpy, screen), DefaultColormap(dpy, screen), &rc, c)) {
        c->pixel = WhitePixel(dpy, screen);
        c->color = rc;
    }
}
static int bar_runs(const char *s, XftColor *c, int x, int y);
static void bar_text(XftColor *c, int x, int y, const char *s) {
    if (!barxd || !barfont || !s || !*s) return;
    bar_runs(s, c, x, y);
}
static int bar_textw(const char *s) {
    if (!barfont || !s || !*s) return s ? (int)strlen(s) * 6 : 0;
    return bar_runs(s, NULL, 0, 0);
}
/* font holding ucs4 u: main first, then first fallback that has it */
static XftFont *bar_glyph_font(FcChar32 u) {
    /* fast path: ASCII almost always in the primary font */
    if (u < 0x80 && barfont) return barfont;
    if (barfont && XftCharExists(dpy, barfont, u)) return barfont;
    for (int i = 0; i < bar_nfb; i++)
        if (barfont_fbs[i] && XftCharExists(dpy, barfont_fbs[i], u)) return barfont_fbs[i];
    return barfont;
}
static int utf8_decode(const unsigned char *s, size_t left, FcChar32 *out) {
    unsigned char b0 = s[0];
    if (b0 < 0x80) { *out = b0; return 1; }
    if ((b0 >> 5) == 0x6 && left >= 2 && (s[1] & 0xC0) == 0x80) {
        *out = ((FcChar32)(b0 & 0x1f) << 6) | (s[1] & 0x3f); return 2;
    }
    if ((b0 >> 4) == 0xE && left >= 3 && (s[1] & 0xC0) == 0x80 && (s[2] & 0xC0) == 0x80) {
        *out = ((FcChar32)(b0 & 0x0f) << 12) | ((FcChar32)(s[1] & 0x3f) << 6) | (s[2] & 0x3f); return 3;
    }
    if ((b0 >> 3) == 0x1E && left >= 4 && (s[1] & 0xC0) == 0x80 &&
        (s[2] & 0xC0) == 0x80 && (s[3] & 0xC0) == 0x80) {
        *out = ((FcChar32)(b0 & 0x07) << 18) | ((FcChar32)(s[1] & 0x3f) << 12) |
               ((FcChar32)(s[2] & 0x3f) << 6) | (s[3] & 0x3f); return 4;
    }
    *out = 0xFFFD; return 1;
}
/* walk runs of same-font chars; draw when c != NULL, else just measure.
 * Missing glyphs fall back to barfont_fb so Vietnamese + symbols render
 * with any base font. Measure and draw share this path, so alignment is exact. */
static int bar_runs(const char *s, XftColor *c, int x, int y) {
    const unsigned char *p = (const unsigned char *)s;
    size_t left = strlen(s);
    int xoff = 0;
    if (!barfont || !*s) return 0;
    while (left > 0) {
        FcChar32 u;
        int cl = utf8_decode(p, left, &u);
        XftFont *f = bar_glyph_font(u);
        /* extend run while the font stays the same */
        const unsigned char *q = p + cl;
        size_t qleft = left - (size_t)cl;
        while (qleft > 0) {
            FcChar32 u2;
            int cl2 = utf8_decode(q, qleft, &u2);
            if (bar_glyph_font(u2) != f) break;
            q += cl2; qleft -= (size_t)cl2;
        }
        if (c) XftDrawStringUtf8(barxd, c, f, x + xoff, y, (const FcChar8 *)p, (int)(q - p));
        {
            XGlyphInfo e;
            XftTextExtentsUtf8(dpy, f, (const FcChar8 *)p, (int)(q - p), &e);
            xoff += e.xOff;
        }
        left = qleft; p = q;
    }
    return xoff;
}
/* bytes to copy (<= cap-1) ending on a UTF-8 char boundary */
static size_t utf8_fit_len(const unsigned char *s, size_t n, size_t cap) {
    size_t m = n > cap - 1 ? cap - 1 : n;
    if (m > 0 && cap > 1) {
        size_t j = m;
        while (j > 0 && (s[j - 1] & 0xC0) == 0x80) j--;
        if (j < m) {
            unsigned char lead = s[j];
            size_t want;
            if (j == 0 && (lead & 0xC0) == 0x80) want = m + 1; /* broken head: drop */
            else if (lead < 0x80) want = 1;
            else if ((lead >> 5) == 0x6) want = 2;
            else if ((lead >> 4) == 0xE) want = 3;
            else want = 4;
            if (m - j < want) m = j;
        }
    }
    return m;
}
/* UTF-8 title: _NET_WM_NAME first, XFetchName fallback; never cuts a char */
static int get_title(Window w, char *buf, size_t cap) {
    static Atom a_name = None, a_utf8 = None;
    Atom rt; int rf; unsigned long n, extra;
    unsigned char *data = NULL;
    size_t len = 0;
    if (cap == 0) return 0;
    buf[0] = 0;
    if (a_name == None) {
        a_name = XInternAtom(dpy, "_NET_WM_NAME", False);
        a_utf8 = XInternAtom(dpy, "UTF8_STRING", False);
    }
    if (a_name != None &&
        XGetWindowProperty(dpy, w, a_name, 0, 256, False, a_utf8,
            &rt, &rf, &n, &extra, &data) == Success && data) {
        if (rf == 8 && n > 0) {
            len = utf8_fit_len(data, n, cap);
            memcpy(buf, data, len);
            buf[len] = 0;
        }
        XFree(data);
    }
    if (!len) {
        char *nm = NULL;
        if (XFetchName(dpy, w, &nm) && nm && *nm) {
            len = utf8_fit_len((unsigned char *)nm, strlen(nm), cap);
            memcpy(buf, nm, len);
            buf[len] = 0;
        }
        if (nm) XFree(nm);
    }
    return (int)len;
}
static void drawbar(void) {
    if (!bar_on || !bar) return;
    int bar_h = S(BAR_H), wsw = S(WS_W);
    if (bar_h < 8) bar_h = 8;
    if (wsw < 8) wsw = 8;
    if (!barpm)
        barpm = XCreatePixmap(dpy, root, (unsigned)barw, (unsigned)bar_h, (unsigned)DefaultDepth(dpy, screen));
    if (!barpm) return;
    if (!barxd) {
        barxd = XftDrawCreate(dpy, barpm,
            DefaultVisual(dpy, screen), DefaultColormap(dpy, screen));
        if (!barxd) return;
    }

    /* component colors: explicit override, else base color */
    unsigned long c_ws_act   = h_ws_act   ? C_WS_ACT   : BAR_ACC;
    unsigned long c_ws_acttx = h_ws_act_tx ? C_WS_ACT_TX : BAR_BG;
    unsigned long c_ws_occ   = h_ws_occ   ? C_WS_OCC   : BAR_FG;
    unsigned long c_ws_emp   = h_ws_emp   ? C_WS_EMP   : BAR_DIM;
    unsigned long c_mode     = h_mode     ? C_MODE     : BAR_FG;
    unsigned long c_title    = h_title    ? C_TITLE    : BAR_FG;
    unsigned long c_sys      = h_sys      ? C_SYS      : BAR_DIM;

    /* text baseline from font metrics so bar_h != 24 stays vertically centered */
    int baseline = 16;
    if (barfont) baseline = (bar_h + barfont->ascent - barfont->descent) / 2;
    if (baseline < 4) baseline = 4;
    if (baseline > bar_h - 2) baseline = bar_h - 2;

    /* clear background of pixmap */
    XSetForeground(dpy, bargc, BAR_BG);
    XFillRectangle(dpy, barpm, bargc, 0, 0, (unsigned)barw, (unsigned)bar_h);

    int n = count_tiled();
    /* workspace boxes */
    for (int i = 0; i < NWS; i++) {
        int x = i * wsw;
        char label[16];
        snprintf(label, sizeof(label), "%s%d", ws_occupied(i) ? "*" : " ", i + 1);
        if (i == curws) {
            XSetForeground(dpy, bargc, c_ws_act);
            XFillRectangle(dpy, barpm, bargc, x, 0, (unsigned)wsw, (unsigned)bar_h);
            XSetForeground(dpy, bargc, c_ws_acttx);
            bar_text(&barcol.ws_acttx, x + S(12), baseline, label);
        } else {
            int urg = ws_has_urgent(i);
            if (urg) {
                XSetForeground(dpy, bargc, c_ws_act);
                bar_text(&barcol.ws_act, x + S(12), baseline, label);
            } else {
                XSetForeground(dpy, bargc, ws_occupied(i) ? c_ws_occ : c_ws_emp);
                bar_text(ws_occupied(i) ? &barcol.ws_occ : &barcol.ws_emp, x + S(12), baseline, label);
            }
        }
    }
    XSetForeground(dpy, bargc, c_ws_emp);
    XDrawLine(dpy, barpm, bargc, NWS * wsw, 2, NWS * wsw, bar_h - 3);

    /* layout + counts + gaps */
    char mode[64];
    snprintf(mode, sizeof(mode), "[%c] %dn%s", LAYOUT == L_TILE ? 'T' : 'M', n, gaps_on ? "" : " G-");
    XSetForeground(dpy, bargc, c_mode);
    bar_text(&barcol.mode, NWS * wsw + S(10), baseline, mode);

    /* focused title (UTF-8) */
    if (sel) {
        char t[128];
        if (get_title(sel->win, t, sizeof(t) - 16) > 0) {
            int tx = NWS * wsw + S(110);
            XSetForeground(dpy, bargc, c_title);
            bar_text(&barcol.title, tx, baseline, t);
        }
    }
    /* modules + clock, right-aligned */
    char right[128] = "";
    {
        char seg[96] = "";
        time_t now = time(NULL);
        static time_t sys_last_sec = 0;
        static int cached_cpu = -1;
        static int cached_mem = -1;
        static int cached_bat = -1;
        static char cached_bat_chg[4] = "";
        if (now != sys_last_sec) {
            cached_cpu = sys_cpu();
            cached_mem = sys_mem();
            cached_bat = sys_bat(cached_bat_chg, sizeof(cached_bat_chg));
            sys_last_sec = now;
        }
        const char *vol = sys_vol();
        if (cached_cpu >= 0) snprintf(seg + strlen(seg), sizeof(seg) - strlen(seg), "C %d%%  ", cached_cpu);
        if (cached_mem >= 0) snprintf(seg + strlen(seg), sizeof(seg) - strlen(seg), "M %d%%  ", cached_mem);
        if (cached_bat >= 0) snprintf(seg + strlen(seg), sizeof(seg) - strlen(seg), "B %d%s  ", cached_bat, cached_bat_chg);
        if (vol && *vol) snprintf(seg + strlen(seg), sizeof(seg) - strlen(seg), "V %s  ", vol);
        struct tm *tm = localtime(&now);
        char clk[32];
        if (tm) strftime(clk, sizeof(clk), "%H:%M", tm);
        else     snprintf(clk, sizeof(clk), "--:--");
        snprintf(right, sizeof(right), "%s%s", seg, clk);
    }
    XSetForeground(dpy, bargc, c_sys);
    int rw = bar_textw(right);
    bar_text(&barcol.sys, barw - rw - S(8), baseline, right);

    /* copy double-buffer to bar */
    XCopyArea(dpy, barpm, bar, bargc, 0, 0, (unsigned)barw, (unsigned)bar_h, 0, 0);
    XFlush(dpy);
}

/* ---- actions ---- */
/* Stacking: docks/panels always on top of normal windows, fullscreen above
 * everything (covers bar/panels). Tiled needs no raise (non-overlapping). */
static void keep_docks_on_top(void) {
    for (Dock *d = docks; d; d = d->next) XRaiseWindow(dpy, d->win);
    for (Client *c = clients; c; c = c->next)
        if (c->ws == curws && c->fullscreen) XRaiseWindow(dpy, c->win);
}
static void focus(Client *c) {
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
    if (c->floating || c->fullscreen) {
        XRaiseWindow(dpy, c->win);
        keep_docks_on_top();
    }
    XSetInputFocus(dpy, c->win, RevertToPointerRoot, CurrentTime);
    ewmh_active();
}

static void focus_step(int dir) {
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

static void view(int n) {
    if (n < 0 || n >= NWS || n == curws) return;
    ws_sel[curws] = sel;
    int old = curws;
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

static void send_to(int n) {
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
    for (Client *c = clients; c; c = c->next)
        if (c->ws == old) XUnmapWindow(dpy, c->win);
    curws = n;
    sel = s;
    ewmh_desktops();
    arrange();
    focus(s); /* follow: nhảy theo luôn */
}
/* external pager move: same as send_to but stays on current ws */
static void move_to(Client *c, int n) {
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

static void kill_client(Client *c) {
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
static void kill_sel(void) {
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

static void spawn(char **argv) {
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

static void toggle_floating_sel(void) {
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

static void quit(void) { XCloseDisplay(dpy); exit(0); }

/* ---- EWMH ---- */
static void ewmh_init(void) {
    A_NET_SUPPORTED = XInternAtom(dpy, "_NET_SUPPORTED", False);
    A_NET_CLIENT_LIST = XInternAtom(dpy, "_NET_CLIENT_LIST", False);
    A_NET_ACTIVE_WINDOW = XInternAtom(dpy, "_NET_ACTIVE_WINDOW", False);
    A_NET_WM_STATE = XInternAtom(dpy, "_NET_WM_STATE", False);
    A_NET_WM_STATE_FS = XInternAtom(dpy, "_NET_WM_STATE_FULLSCREEN", False);
    A_NET_WM_STATE_HIDDEN = XInternAtom(dpy, "_NET_WM_STATE_HIDDEN", False);
    A_NET_WM_STATE_DA = XInternAtom(dpy, "_NET_WM_STATE_DEMANDS_ATTENTION", False);
    A_NET_WM_WINDOW_TYPE = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE", False);
    A_NET_WM_WINDOW_TYPE_DIALOG = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE_DIALOG", False);
    A_NET_WM_WINDOW_TYPE_DOCK = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE_DOCK", False);
    A_NET_WM_WINDOW_TYPE_TOOLBAR = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE_TOOLBAR", False);
    A_NET_WM_WINDOW_TYPE_SPLASH = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE_SPLASH", False);
    A_NET_WM_WINDOW_TYPE_UTILITY = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE_UTILITY", False);
    A_NET_CLOSE_WINDOW = XInternAtom(dpy, "_NET_CLOSE_WINDOW", False);
    A_NET_SUPPORTING_WM_CHECK = XInternAtom(dpy, "_NET_SUPPORTING_WM_CHECK", False);
    A_NET_WM_NAME = XInternAtom(dpy, "_NET_WM_NAME", False);
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
    XChangeProperty(dpy, root, A_NET_SUPPORTING_WM_CHECK, XA_WINDOW, 32,
        PropModeReplace, (unsigned char *)&checkwin, 1);

    Atom sup[] = { A_NET_SUPPORTED, A_NET_CLIENT_LIST, A_NET_ACTIVE_WINDOW,
        A_NET_WM_STATE, A_NET_WM_STATE_FS, A_NET_WM_STATE_HIDDEN,
        A_NET_WM_STATE_DA, A_NET_WM_WINDOW_TYPE, A_NET_CLOSE_WINDOW,
        A_NET_SUPPORTING_WM_CHECK, A_NET_WM_NAME,
        A_NET_WM_STRUT, A_NET_WM_STRUT_PARTIAL, A_NET_WORKAREA,
        A_NET_NUMBER_OF_DESKTOPS, A_NET_CURRENT_DESKTOP,
        A_NET_WM_DESKTOP, A_NET_DESKTOP_NAMES };
    XChangeProperty(dpy, root, A_NET_SUPPORTED, XA_ATOM, 32,
        PropModeReplace, (unsigned char *)sup, sizeof(sup) / sizeof(sup[0]));
    ewmh_client_list(); /* empty list so pagers/wmctrl never query a missing prop */
    ewmh_desktops();
    update_struts();
}
static int ewmh_hasstate(Window w, Atom state) {
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
static void ewmh_update_state(Client *c) {
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
static void set_urgent(Client *c, int urg) {
    if (!c || c->urgent == urg) return;
    c->urgent = urg;
    ewmh_update_state(c);
    drawbar();
}
static int ws_has_urgent(int n) {
    for (Client *c = clients; c; c = c->next)
        if (c->ws == n && c->urgent) return 1;
    return 0;
}
static int ewmh_isfloating_type(Window w) {
    Atom *p = NULL, rt; int rf, f = 0;
    unsigned long n, extra;
    if (XGetWindowProperty(dpy, w, A_NET_WM_WINDOW_TYPE, 0, 8, False, XA_ATOM,
        &rt, &rf, &n, &extra, (unsigned char **)&p) == Success && p) {
        for (unsigned long i = 0; i < n; i++)
            if (p[i] == A_NET_WM_WINDOW_TYPE_DIALOG || p[i] == A_NET_WM_WINDOW_TYPE_UTILITY ||
                p[i] == A_NET_WM_WINDOW_TYPE_TOOLBAR || p[i] == A_NET_WM_WINDOW_TYPE_SPLASH) f = 1;
        XFree(p);
    }
    return f;
}
static void ewmh_client_list(void) {
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
static void ewmh_active(void) {
    Window w = sel ? sel->win : None;
    XChangeProperty(dpy, root, A_NET_ACTIVE_WINDOW, XA_WINDOW, 32,
        PropModeReplace, (unsigned char *)&w, 1);
}
/* Pager/desktop bridge: NUMBER+CURRENT on root, WM_DESKTOP per client,
 * NAMES as "1\0...\0". Parked scratchpad (ws==NWS) reports sticky
 * 0xFFFFFFFF. Safe to call before ewmh_init (atoms None → no-op), so
 * finalize_nws() can use it during early load_config. */
static void ewmh_set_wm_desktop(Client *c) {
    unsigned long d;
    if (!dpy || A_NET_WM_DESKTOP == None || !c) return;
    d = (c->ws >= 0 && c->ws < NWS) ? (unsigned long)c->ws : 0xFFFFFFFFUL;
    XChangeProperty(dpy, c->win, A_NET_WM_DESKTOP, XA_CARDINAL, 32,
        PropModeReplace, (unsigned char *)&d, 1);
}
static void ewmh_desktops(void) {
    unsigned long n, cur;
    char names[MAXWS * 4];
    int off = 0;
    Atom utf8;
    if (!dpy || A_NET_NUMBER_OF_DESKTOPS == None) return;
    n = (unsigned long)NWS;
    cur = (unsigned long)curws;
    XChangeProperty(dpy, root, A_NET_NUMBER_OF_DESKTOPS, XA_CARDINAL, 32,
        PropModeReplace, (unsigned char *)&n, 1);
    XChangeProperty(dpy, root, A_NET_CURRENT_DESKTOP, XA_CARDINAL, 32,
        PropModeReplace, (unsigned char *)&cur, 1);
    for (int i = 0; i < NWS && off + 4 < (int)sizeof(names); i++)
        off += snprintf(names + off, sizeof(names) - (size_t)off, "%d", i + 1) + 1;
    utf8 = XInternAtom(dpy, "UTF8_STRING", False);
    if (utf8 != None && off > 0)
        XChangeProperty(dpy, root, A_NET_DESKTOP_NAMES, utf8, 8,
            PropModeReplace, (unsigned char *)names, (int)off);
    for (Client *c = clients; c; c = c->next) ewmh_set_wm_desktop(c);
}
/* initial desktop hint on manage: CARDINAL < NWS, else -1 */
static int ewmh_read_desktop(Window w) {
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
static void setfullscreen(Client *c, int fs) {
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
static Dock *find_dock(Window w) {
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

/* Bounding box of all monitors in root coords. Falls back to 0,0,sw,sh
 * before initmons() (mismatched origins are what this fixes). */
static void screen_extents(long *x0, long *y0, long *x1, long *y1) {
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
static int ewmh_isdock(Window w) {
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

static void update_struts(void) {
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

static void manage_dock(Window w) {
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

static void unmanage_dock(Window w) {
    Dock **p = &docks;
    while (*p && (*p)->win != w) p = &(*p)->next;
    if (!*p) return;
    Dock *d = *p;
    *p = d->next;
    free(d);

    update_struts();
    arrange();
}

static void update_dock_strut(Window w) {
    Dock *d = find_dock(w);
    if (!d) return;
    d->has_strut = get_strut(w, d->strut);
    update_struts();
    arrange();
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
/* ---- manage ---- */
static void manage(Window w) {
    if (w == bar) return;
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
    if (XGetTransientForHint(dpy, w, &trans) || ewmh_isfloating_type(w) || rulefloat) {
        c->floating = 1;
        int ax, ay, aw, ah;
        getarea(c->mon, &ax, &ay, &aw, &ah);
        int fw = a.width > 0 ? a.width : aw / 2;
        int fh = a.height > 0 ? a.height : ah / 2;
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
    c->ws = rulews;
    ewmh_client_list();
    ewmh_set_wm_desktop(c);
    if (c->ws == curws) {
        XMapWindow(dpy, w);
        focus(c);
    }
    arrange();
}

/* ---- mouse: Mod+Left move, Mod+Right resize ----
 * Passive grabs live on client windows (grabbuttons). Plain Mod+click
 * without motion only focuses. Fullscreen never drags.
 * - Mod+Left move: promotes a tiled window to floating once pointer moves
 *   past a 4px deadzone. Dropping on another monitor re-tiles on that monitor;
 *   same-monitor drop stays floating.
 * - Mod+Right resize: in tiling mode (L_TILE), drags the master/stack split
 *   (resizes mfact) while keeping windows tiled; on floating windows or in
 *   monocle mode, resizes the window geometry. */
typedef struct { Window win; int mode; int px, py, x, y, w, h, promoted, tiled0, mon0; float mfact0; } Drag;
static Drag drag = { 0 };
static Cursor cur_move = None, cur_resize = None, cur_hsplit = None;

static void unmanage(Window w) {
    Client *c = find(w);
    if (!c) return;
    if (drag.win == w) {
        drag.win = None;
        drag.mode = 0;
        XUngrabPointer(dpy, CurrentTime);
    }
    detach(c);
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

static void grabbuttons(Client *c) {
    unsigned int masks[] = { 0, LockMask, Mod2Mask, LockMask | Mod2Mask };
    XUngrabButton(dpy, AnyButton, AnyModifier, c->win);
    for (unsigned m = 0; m < sizeof(masks) / sizeof(masks[0]); m++)
        for (int b = Button1; b <= Button3; b += 2) /* left + right only */
            XGrabButton(dpy, (unsigned int)b, MOD | masks[m], c->win, False,
                ButtonPressMask, GrabModeAsync, GrabModeAsync, None, None);
}
static void drag_start(Client *c, int mode, int px, int py) {
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
}
static void drag_motion(int px, int py) {
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
        float new_mfact = drag.mfact0 + (float)dx / (float)aw;
        if (new_mfact < 0.1f) new_mfact = 0.1f;
        if (new_mfact > 0.9f) new_mfact = 0.9f;
        MFACT = new_mfact;
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
static void drag_end(int px, int py) {
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

/* ---- keys + config file ---- */
typedef struct { KeySym keysym; unsigned int mod; void (*fn)(int); int arg; } Key;
static Key *keys = NULL;
static unsigned nkeys = 0, capkeys = 0;
static void load_config(const char *path);
static void grabkeys(void);
static void finalize_nws(void);
static void bar_style(void);
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
static void k_vol_up(int)   { spawn(vol_set_cmd(vol_up_am, vol_up_wp, vol_up_pa)); vol_ts = 0; }
static void k_vol_down(int) { spawn(vol_set_cmd(vol_down_am, vol_down_wp, vol_down_pa)); vol_ts = 0; }
static void k_vol_mute(int) { spawn(vol_set_cmd(vol_mute_am, vol_mute_wp, vol_mute_pa)); vol_ts = 0; }
/* Keyboard float move/resize: 20px steps, key repeat = smooth.
 * Tiled windows promote to floating first (same as mouse drag). */
#define FLOAT_STEP 20
#define RSZ_STEP 20
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
static void k_reload(int);
static const struct { const char *name; void (*fn)(int); } actions[] = {
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
static char *xstrdup(const char *s) {
    if (!s) return NULL;
    size_t n = strlen(s) + 1;
    char *p = malloc(n);
    if (p) memcpy(p, s, n);
    return p;
}
static char *trim(char *s) {
    while (*s && isspace((unsigned char)*s)) s++;
    char *e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1])) *--e = 0;
    return s;
}
/* cut trailing "# comment" after a value; a '#' at position 0 is a hex
 * color (#rrggbb), keep it. Call after trim(). */
static void strip_comment(char *v) {
    if (!v) return;
    char *p = (*v == '#') ? v + 1 : v;
    char *h = strchr(p, '#');
    if (h) { *h = 0; trim(v); }
}
static void push_key_fn(KeySym ks, unsigned int mod, void (*fn)(int), int arg) {
    if (nkeys == capkeys) {
        unsigned nc = capkeys ? capkeys * 2 : 32;
        Key *nk = realloc(keys, nc * sizeof(*nk));
        if (!nk) return;
        keys = nk; capkeys = nc;
    }
    keys[nkeys].keysym = ks; keys[nkeys].mod = mod; keys[nkeys].fn = fn; keys[nkeys].arg = arg; nkeys++;
}
static void push_rule(char *cls, char *title, int floating, int ws) {
    if (nrules == caprules) {
        unsigned nc = caprules ? caprules * 2 : 8;
        Rule *nr = realloc(rules, nc * sizeof(*nr));
        if (!nr) { free(cls); free(title); return; }
        rules = nr; caprules = nc;
    }
    rules[nrules].cls = cls; rules[nrules].title = title;
    rules[nrules].floating = floating; rules[nrules].ws = ws; nrules++;
}
static void free_argv(char **argv) {
    if (!argv) return;
    for (char **p = argv; *p; p++) free(*p);
    free(argv);
}
static char **split_argv(const char *s) {
    wordexp_t w;
    if (wordexp(s, &w, WRDE_NOCMD) != 0) return NULL;
    if (w.we_wordc == 0) { wordfree(&w); return NULL; }
    char **out = calloc(w.we_wordc + 1, sizeof(*out));
    if (!out) { wordfree(&w); return NULL; }
    for (size_t i = 0; i < w.we_wordc; i++) {
        out[i] = xstrdup(w.we_wordv[i]);
        if (!out[i]) { free_argv(out); wordfree(&w); return NULL; } /* all-or-nothing: no arg holes */
    }
    wordfree(&w);
    return out;
}
static void set_cmd(char ***slot, const char *s) {
    char **v = split_argv(s);
    if (!v || !v[0]) { free_argv(v); return; }
    free_argv(*slot);
    *slot = v;
}
static int parse_bool(const char *s, int *out) {
    if (!strcasecmp(s, "1") || !strcasecmp(s, "true") || !strcasecmp(s, "yes") || !strcasecmp(s, "on")) { *out = 1; return 1; }
    if (!strcasecmp(s, "0") || !strcasecmp(s, "false") || !strcasecmp(s, "no") || !strcasecmp(s, "off")) { *out = 0; return 1; }
    return 0;
}
static int parse_hex(const char *s, unsigned long *out) {
    while (isspace((unsigned char)*s)) s++;
    if (*s == '#') s++;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s += 2;
    if (!*s) return 0;
    char *e = NULL;
    unsigned long v = strtoul(s, &e, 16);
    if (!e || *e) return 0;
    *out = v & 0xffffff;
    return 1;
}
static int mod_from_name(const char *s, unsigned int *out) {
    if (!strcasecmp(s, "super") || !strcasecmp(s, "mod4") || !strcasecmp(s, "mod")) { *out = Mod4Mask; return 1; }
    if (!strcasecmp(s, "alt") || !strcasecmp(s, "mod1")) { *out = Mod1Mask; return 1; }
    if (!strcasecmp(s, "ctrl") || !strcasecmp(s, "control")) { *out = ControlMask; return 1; }
    return 0;
}
static void add_default_keys(void) {
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
static void config_defaults(void) {
    for (unsigned i = 0; i < nrules; i++) { free(rules[i].cls); free(rules[i].title); }
    nrules = 0;
    nkeys = 0;
    MOD = Mod4Mask; BORDER = 2; NWS = 5;
    BORDER_FOCUS = 0x7aa2f7; BORDER_NORMAL = 0x333333;
    BAR_BG = 0x1e1e2e; BAR_FG = 0xcdd6f4; BAR_ACC = 0x7aa2f7; BAR_DIM = 0x6c7086;
    C_WS_ACT = C_WS_ACT_TX = C_WS_OCC = C_WS_EMP = C_MODE = C_TITLE = C_SYS = 0;
    h_ws_act = h_ws_act_tx = h_ws_occ = h_ws_emp = h_mode = h_title = h_sys = 0;
    BAR_H = 24; WS_W = 40; ui_scale = 1.0f;
    free(font_name);
    font_name = xstrdup("monospace:size=10");
    bar_on = 1; gaps_on = 1; gap_outer = 10; gap_inner = 8;
    def_mfact = 0.55f; def_nmaster = 1;
    set_cmd(&termcmd, "xterm");
    set_cmd(&menucmd, "dmenu_run");
    set_cmd(&scratchcmd, "xterm -name scratchpad");
    push_rule(xstrdup("scratchpad"), NULL, 1, -1);
    push_rule(xstrdup("Gimp"), NULL, 1, -1);
    push_rule(xstrdup("mpv"), NULL, 1, -1);
    add_default_keys();
}
static const char *config_path(char *buf, size_t n) {
    const char *xdg = getenv("XDG_CONFIG_HOME");
    const char *home = getenv("HOME");
    if (xdg && *xdg) snprintf(buf, n, "%s/daniwm/config", xdg);
    else if (home && *home) snprintf(buf, n, "%s/.config/daniwm/config", home);
    else return NULL;
    return buf;
}
static void parse_scalar(char *key, char *val) {
    long v; float f; unsigned long h; int b; unsigned int m;
    if (!strcmp(key, "mod")) {
        if (mod_from_name(val, &m)) MOD = m;
        else fprintf(stderr, "daniwm: bad mod '%s'\n", val);
    } else if (!strcmp(key, "border")) {
        v = strtol(val, NULL, 10); if (v >= 0 && v <= 8) BORDER = (int)v;
    } else if (!strcmp(key, "border_focus")) {
        if (parse_hex(val, &h)) BORDER_FOCUS = h;
    } else if (!strcmp(key, "border_normal")) {
        if (parse_hex(val, &h)) BORDER_NORMAL = h;
    } else if (!strcmp(key, "bar_bg")) {
        if (parse_hex(val, &h)) BAR_BG = h;
    } else if (!strcmp(key, "bar_fg")) {
        if (parse_hex(val, &h)) BAR_FG = h;
    } else if (!strcmp(key, "bar_acc")) {
        if (parse_hex(val, &h)) BAR_ACC = h;
    } else if (!strcmp(key, "bar_dim")) {
        if (parse_hex(val, &h)) BAR_DIM = h;
    } else if (!strcmp(key, "bar_ws_active")) {
        if (parse_hex(val, &h)) { C_WS_ACT = h; h_ws_act = 1; }
    } else if (!strcmp(key, "bar_ws_active_text")) {
        if (parse_hex(val, &h)) { C_WS_ACT_TX = h; h_ws_act_tx = 1; }
    } else if (!strcmp(key, "bar_ws_occ")) {
        if (parse_hex(val, &h)) { C_WS_OCC = h; h_ws_occ = 1; }
    } else if (!strcmp(key, "bar_ws_empty")) {
        if (parse_hex(val, &h)) { C_WS_EMP = h; h_ws_emp = 1; }
    } else if (!strcmp(key, "bar_mode")) {
        if (parse_hex(val, &h)) { C_MODE = h; h_mode = 1; }
    } else if (!strcmp(key, "bar_title")) {
        if (parse_hex(val, &h)) { C_TITLE = h; h_title = 1; }
    } else if (!strcmp(key, "bar_sys")) {
        if (parse_hex(val, &h)) { C_SYS = h; h_sys = 1; }
    } else if (!strcmp(key, "bar_h")) {
        v = strtol(val, NULL, 10); if (v >= 8 && v <= 64) BAR_H = (int)v;
    } else if (!strcmp(key, "scale")) {
        f = strtof(val, NULL); if (f >= 0.5f && f <= 3.0f) ui_scale = f;
        else fprintf(stderr, "daniwm: bad scale '%s' (want 0.5..3.0)\n", val);
    } else if (!strcmp(key, "font")) {
        char *dup = xstrdup(val);
        if (dup) { free(font_name); font_name = dup; }
    } else if (!strcmp(key, "ws_w")) {
        v = strtol(val, NULL, 10); if (v >= 16 && v <= 128) WS_W = (int)v;
    } else if (!strcmp(key, "bar_on")) {
        if (parse_bool(val, &b)) bar_on = b;
    } else if (!strcmp(key, "gaps_on")) {
        if (parse_bool(val, &b)) gaps_on = b;
    } else if (!strcmp(key, "gap_outer")) {
        v = strtol(val, NULL, 10); if (v >= 0 && v <= 64) gap_outer = (int)v;
    } else if (!strcmp(key, "gap_inner")) {
        v = strtol(val, NULL, 10); if (v >= 0 && v <= 64) gap_inner = (int)v;
    } else if (!strcmp(key, "mfact")) {
        f = strtof(val, NULL); if (f >= 0.1f && f <= 0.9f) def_mfact = f;
    } else if (!strcmp(key, "nmaster")) {
        v = strtol(val, NULL, 10); if (v >= 1 && v <= 8) def_nmaster = (int)v;
    } else if (!strcmp(key, "workspaces")) {
        v = strtol(val, NULL, 10); if (v >= 1 && v <= MAXWS) NWS = (int)v;
    } else if (!strcmp(key, "term")) {
        set_cmd(&termcmd, val);
    } else if (!strcmp(key, "menu")) {
        set_cmd(&menucmd, val);
    } else if (!strcmp(key, "scratch")) {
        set_cmd(&scratchcmd, val);
    }
}
static void parse_rule(const char *val, int lineno, const char *path) {
    char *s = xstrdup(val);
    if (!s) return;
    char *p1 = strchr(s, ':');
    char *p2 = p1 ? strchr(p1 + 1, ':') : NULL;
    char *p3 = p2 ? strchr(p2 + 1, ':') : NULL;
    if (!p1 || !p2 || !p3) {
        fprintf(stderr, "daniwm: %s:%d: bad rule '%s' (want class:title:float:ws)\n", path, lineno, val);
        free(s); return;
    }
    *p1 = 0; *p2 = 0; *p3 = 0;
    char *cls = trim(s), *title = trim(p1 + 1);
    char *fs = trim(p2 + 1), *ws = trim(p3 + 1);
    int floating;
    if (!strcasecmp(fs, "float") || !strcmp(fs, "1")) floating = 1;
    else if (!strcasecmp(fs, "tile") || !strcasecmp(fs, "tiled") || !strcmp(fs, "0")) floating = 0;
    else {
        fprintf(stderr, "daniwm: %s:%d: bad rule float '%s'\n", path, lineno, fs);
        free(s); return;
    }
    int w = -1;
    if (strcmp(ws, "*")) {
        long n = strtol(ws, NULL, 10);
        if (n < 1 || n > NWS) {
            fprintf(stderr, "daniwm: %s:%d: bad rule ws '%s'\n", path, lineno, ws);
            free(s); return;
        }
        w = (int)n - 1;
    }
    char *c = (strcmp(cls, "*") && *cls) ? xstrdup(cls) : NULL;
    char *t = (strcmp(title, "*") && *title) ? xstrdup(title) : NULL;
    push_rule(c, t, floating, w);
    free(s);
}
static void parse_bind(const char *val, int lineno, const char *path) {
    const char *sep = strrchr(val, ':');
    if (!sep || !sep[1] || sep == val) {
        fprintf(stderr, "daniwm: %s:%d: bad bind '%s' (want mod+key:action)\n", path, lineno, val);
        return;
    }
    /* action name (after last ':') */
    char aname_buf[64];
    size_t alen = strlen(sep + 1);
    if (alen >= sizeof(aname_buf)) {
        fprintf(stderr, "daniwm: %s:%d: action too long in '%s'\n", path, lineno, val);
        return;
    }
    memcpy(aname_buf, sep + 1, alen + 1);
    char *aname = trim(aname_buf);
    void (*fn)(int) = NULL;
    int arg = 0;
    for (unsigned i = 0; i < sizeof(actions) / sizeof(actions[0]); i++)
        if (!strcmp(actions[i].name, aname)) { fn = actions[i].fn; break; }
    if (!fn && (!strncmp(aname, "ws", 2) || !strncmp(aname, "mv", 2))) {
        char *ep = NULL;
        long n = strtol(aname + 2, &ep, 10);
        if (ep && !*ep && n >= 1 && n <= NWS) { fn = aname[0] == 'w' ? view : send_to; arg = (int)n - 1; }
    }
    if (!fn) {
        fprintf(stderr, "daniwm: %s:%d: unknown action '%s'\n", path, lineno, aname);
        return;
    }
    /* combo (before last ':'): split on '+', collect tokens in one pass */
    size_t clen = (size_t)(sep - val);
    char combo[256];
    if (clen >= sizeof(combo)) {
        fprintf(stderr, "daniwm: %s:%d: bind combo too long\n", path, lineno);
        return;
    }
    memcpy(combo, val, clen);
    combo[clen] = 0;
    char *tokens[16];
    int ntokens = 0;
    for (char *p = combo; *p && ntokens < 16; ) {
        while (*p && isspace((unsigned char)*p)) p++;
        if (!*p) break;
        tokens[ntokens] = p;
        char *end = strchr(p, '+');
        if (end) { *end = 0; p = end + 1; }
        else p += strlen(p);
        /* trim trailing whitespace from token */
        char *e = tokens[ntokens] + strlen(tokens[ntokens]);
        while (e > tokens[ntokens] && isspace((unsigned char)e[-1])) *--e = 0;
        ntokens++;
    }
    if (ntokens < 1) {
        fprintf(stderr, "daniwm: %s:%d: empty bind combo\n", path, lineno);
        return;
    }
    /* last token = key, preceding tokens = modifiers */
    unsigned int mod = 0;
    for (int i = 0; i < ntokens - 1; i++) {
        char *t = tokens[i];
        if (!strcasecmp(t, "mod")) mod |= MOD;
        else if (!strcasecmp(t, "super") || !strcasecmp(t, "mod4")) mod |= Mod4Mask;
        else if (!strcasecmp(t, "alt") || !strcasecmp(t, "mod1")) mod |= Mod1Mask;
        else if (!strcasecmp(t, "ctrl") || !strcasecmp(t, "control")) mod |= ControlMask;
        else if (!strcasecmp(t, "shift")) mod |= ShiftMask;
        else fprintf(stderr, "daniwm: %s:%d: bad modifier '%s'\n", path, lineno, t);
    }
    char *keyname = tokens[ntokens - 1];
    if (!*keyname) {
        fprintf(stderr, "daniwm: %s:%d: empty key in bind '%s'\n", path, lineno, val);
        return;
    }
    KeySym ks = XStringToKeysym(keyname);
    if (ks == NoSymbol) {
        fprintf(stderr, "daniwm: %s:%d: bad key '%s'\n", path, lineno, keyname);
        return;
    }
    push_key_fn(ks, mod, fn, arg);
}
static void load_config(const char *path) {
    char buf[1024];
    char fallback[1024];
    if (!path) {
        path = config_path(buf, sizeof(buf));
        if (!path) { config_defaults(); finalize_nws(); return; }
    }
    FILE *f = fopen(path, "r");
    // fallback: old ~/.config/tilewm/config
    if (!f) {
        const char *home2 = getenv("HOME");
        if (home2) {
            snprintf(fallback, sizeof(fallback), "%s/.config/tilewm/config", home2);
            f = fopen(fallback, "r");
            if (f) path = fallback;
        }
    }
    config_defaults();
    if (!f) { finalize_nws(); return; }
    char *line = NULL;
    size_t cap = 0;
    char **lines = NULL;
    unsigned nlines = 0, linecap = 0;
    ssize_t len;
    while ((len = getline(&line, &cap, f)) >= 0) {
        if (nlines == linecap) {
            unsigned nc = linecap ? linecap * 2 : 64;
            char **nl = realloc(lines, nc * sizeof(*nl));
            if (!nl) break;
            lines = nl; linecap = nc;
        }
        lines[nlines] = xstrdup(line);
        if (!lines[nlines]) break; /* OOM: parse what we have, never store NULL (trim would crash) */
        nlines++;
    }
    free(line);
    fclose(f);
    /* pass 1: scalars (mod/term/... first so binds see final MOD).
     * Operates on duplicates: trim() strips in place and would otherwise
     * eat the space before '=' in the shared buffer, blinding pass 2. */
    for (unsigned i = 0; i < nlines; i++) {
        char *dup = xstrdup(lines[i]);
        if (!dup) continue;
        char *s = trim(dup);
        if (!*s || *s == '#' || *s == ';') { free(dup); continue; }
        char *eq = strchr(s, '=');
        if (!eq) { free(dup); continue; }
        *eq = 0;
        char *k = trim(s), *v = trim(eq + 1);
        for (char *p = k; *p; p++) *p = (char)tolower((unsigned char)*p);
        strip_comment(v);
        if (strcmp(k, "bind") && strcmp(k, "rule")) parse_scalar(k, v);
        free(dup);
    }
    /* pass 2: rules/binds; first occurrence replaces defaults */
    int saw_rule = 0, saw_bind = 0;
    for (unsigned i = 0; i < nlines; i++) {
        char *s = trim(lines[i]);
        if (!*s || *s == '#' || *s == ';') continue;
        char *eq = strchr(s, '=');
        if (!eq) {
            fprintf(stderr, "daniwm: %s:%u: ignoring '%s'\n", path, i + 1, s);
            continue;
        }
        *eq = 0;
        char *k = trim(s), *v = trim(eq + 1);
        for (char *p = k; *p; p++) *p = (char)tolower((unsigned char)*p);
        strip_comment(v);
        if (!strcmp(k, "rule")) {
            if (!saw_rule) {
                for (unsigned r = 0; r < nrules; r++) { free(rules[r].cls); free(rules[r].title); }
                nrules = 0; saw_rule = 1;
            }
            parse_rule(v, (int)(i + 1), path);
        } else if (!strcmp(k, "bind")) {
            if (!saw_bind) { nkeys = 0; saw_bind = 1; }
            parse_bind(v, (int)(i + 1), path);
        } else if (strcmp(k, "mod") && strcmp(k, "border") && strcmp(k, "border_focus") &&
            strcmp(k, "border_normal") && strcmp(k, "bar_bg") && strcmp(k, "bar_fg") &&
            strcmp(k, "bar_acc") && strcmp(k, "bar_dim") && strcmp(k, "bar_ws_active") &&
            strcmp(k, "bar_ws_active_text") && strcmp(k, "bar_ws_occ") && strcmp(k, "bar_ws_empty") &&
            strcmp(k, "bar_mode") && strcmp(k, "bar_title") && strcmp(k, "bar_sys") &&
            strcmp(k, "bar_h") && strcmp(k, "scale") && strcmp(k, "font") && strcmp(k, "ws_w") && strcmp(k, "bar_on") && strcmp(k, "gaps_on") &&
            strcmp(k, "gap_outer") && strcmp(k, "gap_inner") && strcmp(k, "mfact") &&
            strcmp(k, "nmaster") && strcmp(k, "workspaces") && strcmp(k, "term") &&
            strcmp(k, "menu") && strcmp(k, "scratch")) {
            fprintf(stderr, "daniwm: %s:%u: unknown key '%s'\n", path, i + 1, k);
        }
    }
    if (!saw_bind) { nkeys = 0; add_default_keys(); } /* rebuild with final MOD+NWS */
    finalize_nws();
    for (unsigned i = 0; i < nlines; i++) free(lines[i]);
    free(lines);
}
/* Sync per-workspace arrays to NWS. New slots get current defaults;
 * shrinking folds extra workspaces into the last one, and a parked
 * scratchpad follows the sentinel. OOM-safe: keeps the old set. */
static void finalize_nws(void) {
    Client **s;
    Layout *l;
    float *f;
    int *m;
    int keep, i;
    Client *c;
    if (NWS < 1) NWS = 1;
    if (NWS > MAXWS) NWS = MAXWS;
    if (NWS == nws_alloc) return;
    s = malloc((size_t)NWS * sizeof(*s));
    l = malloc((size_t)NWS * sizeof(*l));
    f = malloc((size_t)NWS * sizeof(*f));
    m = malloc((size_t)NWS * sizeof(*m));
    if (!s || !l || !f || !m) { free(s); free(l); free(f); free(m); NWS = nws_alloc; return; }
    keep = nws_alloc < NWS ? nws_alloc : NWS;
    for (i = 0; i < keep; i++) { s[i] = ws_sel[i]; l[i] = ws_layout[i]; f[i] = ws_mfact[i]; m[i] = ws_nmaster[i]; }
    for (i = keep; i < NWS; i++) { s[i] = NULL; l[i] = L_TILE; f[i] = def_mfact; m[i] = def_nmaster; }
    free(ws_sel); free(ws_layout); free(ws_mfact); free(ws_nmaster);
    ws_sel = s; ws_layout = l; ws_mfact = f; ws_nmaster = m;
    for (c = clients; c; c = c->next) {
        if (c->ws == nws_alloc) c->ws = NWS;
        else if (c->ws >= NWS) c->ws = NWS - 1;
    }
    nws_alloc = NWS;
    if (curws >= NWS) curws = 0;
    for (i = 0; i < NWS; i++)
        if (ws_sel[i] && ws_sel[i]->ws != i) ws_sel[i] = NULL;
    update_struts();
    ewmh_desktops();
}
static void k_reload(int) {
    load_config(NULL);
    for (int i = 0; i < NWS; i++) {
        if (ws_mfact[i] < 0.1f || ws_mfact[i] > 0.9f) ws_mfact[i] = def_mfact;
        if (ws_nmaster[i] < 1) ws_nmaster[i] = def_nmaster;
    }
    if (!sel || sel->ws != curws) sel = first_in_ws(curws);
    /* finalize may have moved curws or folded windows across it: re-sync
     * visibility like view() does (arrange maps tiled but never unmaps). */
    for (Client *c = clients; c; c = c->next) {
        if (c->ws == curws) XMapWindow(dpy, c->win);
        else XUnmapWindow(dpy, c->win);
    }
    if (bar) XMoveResizeWindow(dpy, bar, mons[0].x, mons[0].y, (unsigned)barw, (unsigned)S(BAR_H));
    if (!bar_on && bar) XUnmapWindow(dpy, bar);
    if (bar_on && bar) XMapWindow(dpy, bar);
    bar_style();
    grabkeys();
    for (Client *c = clients; c; c = c->next) grabbuttons(c);
    update_struts();
    arrange();
    if (sel) focus(sel);
}
static void grabkeys(void) {
    unsigned int masks[] = { 0, LockMask, Mod2Mask, LockMask|Mod2Mask };
    XUngrabKey(dpy, AnyKey, AnyModifier, root);
    for (unsigned i = 0; i < nkeys; i++) {
        KeyCode code = XKeysymToKeycode(dpy, keys[i].keysym);
        if (!code) continue;
        for (unsigned m = 0; m < sizeof(masks)/sizeof(masks[0]); m++)
            XGrabKey(dpy, code, keys[i].mod | masks[m], root, True, GrabModeAsync, GrabModeAsync);
    }
}

/* ---- main ---- */
/* scale a fontconfig pattern's size= by ui_scale so `scale=` also
 * enlarges bar text (fractional ok: size=10 @1.5 -> size=15).
 * No size= -> append one based on 10px. Output always NUL-terminated. */
static void scaled_font_pat(const char *pat, char *out, size_t n) {
    if (!pat || !*pat) pat = "monospace:size=10";
    const char *p = strstr(pat, "size=");
    if (!p) {
        snprintf(out, n, "%s:size=%.1f", pat, 10.0 * ui_scale);
        return;
    }
    double sz = strtod(p + 5, NULL);
    if (sz < 1.0 || sz > 128.0) sz = 10.0;
    size_t pre = (size_t)(p + 5 - pat);
    if (pre >= n) pre = n - 1;
    memcpy(out, pat, pre);
    out[pre] = 0;
    const char *q = p + 5;
    while (*q && (isdigit((unsigned char)*q) || *q == '.')) q++;
    snprintf(out + pre, n - pre, "%.1f%s", sz * ui_scale, q);
}
/* (re)apply bar font + colors: call at startup and on config reload.
 * Recreates GC, Xft font/draw/colors so `font =`/color changes apply live.
 * `font` is a fontconfig pattern (e.g. "monospace:size=11"); fallbacks
 * keep the bar readable when the pattern matches nothing. */
static void bar_style(void) {
    static const char *fallbacks[] = { NULL, "monospace:size=10", "monospace", "fixed", NULL };
    static const char *fb_cands[] = { "Noto Sans", "DejaVu Sans", "Sans", NULL };
    if (!bar) return;  /* nothing to (re)style without a bar window */
    Visual *vis = DefaultVisual(dpy, screen);
    Colormap cmap = DefaultColormap(dpy, screen);
    if (barxd) { XftDrawDestroy(barxd); barxd = NULL; }
    if (barpm) { XFreePixmap(dpy, barpm); barpm = None; }
    if (barfont) { XftFontClose(dpy, barfont); barfont = NULL; }
    for (int i = 0; i < bar_nfb; i++)
        if (barfont_fbs[i]) XftFontClose(dpy, barfont_fbs[i]);
    bar_nfb = 0;
    if (barcol_ok) {
        XftColorFree(dpy, vis, cmap, &barcol.bg);
        XftColorFree(dpy, vis, cmap, &barcol.ws_act);
        XftColorFree(dpy, vis, cmap, &barcol.ws_acttx);
        XftColorFree(dpy, vis, cmap, &barcol.ws_occ);
        XftColorFree(dpy, vis, cmap, &barcol.ws_emp);
        XftColorFree(dpy, vis, cmap, &barcol.mode);
        XftColorFree(dpy, vis, cmap, &barcol.title);
        XftColorFree(dpy, vis, cmap, &barcol.sys);
        barcol_ok = 0;
    }
    if (bargc) { XFreeGC(dpy, bargc); bargc = NULL; }
    XSetWindowBackground(dpy, bar, BAR_BG);
    bargc = XCreateGC(dpy, bar, 0, NULL);
    if (!bargc) return;
    XSetForeground(dpy, bargc, BAR_FG);
    XSetBackground(dpy, bargc, BAR_BG);
    fallbacks[0] = font_name ? font_name : "monospace:size=10";
    char fscaled[256];
    if (ui_scale != 1.0f) {
        scaled_font_pat(fallbacks[0], fscaled, sizeof(fscaled));
        fallbacks[0] = fscaled;
    }
    for (int i = 0; fallbacks[i] && !barfont; i++)
        barfont = XftFontOpenName(dpy, screen, fallbacks[i]);
    for (int i = 0; fb_cands[i] && bar_nfb < 4; i++) {
        XftFont *f = XftFontOpenName(dpy, screen, fb_cands[i]);
        if (f) barfont_fbs[bar_nfb++] = f;
    }
    xft_alloc(BAR_BG, &barcol.bg);
    xft_alloc(h_ws_act ? C_WS_ACT : BAR_ACC, &barcol.ws_act);
    xft_alloc(h_ws_act_tx ? C_WS_ACT_TX : BAR_BG, &barcol.ws_acttx);
    xft_alloc(h_ws_occ ? C_WS_OCC : BAR_FG, &barcol.ws_occ);
    xft_alloc(h_ws_emp ? C_WS_EMP : BAR_DIM, &barcol.ws_emp);
    xft_alloc(h_mode ? C_MODE : BAR_FG, &barcol.mode);
    xft_alloc(h_title ? C_TITLE : BAR_FG, &barcol.title);
    xft_alloc(h_sys ? C_SYS : BAR_DIM, &barcol.sys);
    barcol_ok = 1;
    drawbar();
}
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
