#pragma once
#include "types.h"
#include <time.h>

#define MAXWS 10
#define MAXMONS 16
#define FLOAT_STEP 20
#define RSZ_STEP 20

/* ---- display / root ---- */
extern Display *dpy;
extern Window root, bar, checkwin;
extern int screen, sw, sh;

/* ---- clients ---- */
extern Client *clients;
extern Client *sel;
extern Client **ws_sel;
extern int curws;
extern int prevws;
extern int nws_alloc;

/* ---- per-workspace layout arrays ---- */
extern Layout *ws_layout;
extern float *ws_mfact;
extern int *ws_nmaster;

/* ---- monitors ---- */
struct MonitorGeom { int x, y, w, h; };
extern struct MonitorGeom mons[MAXMONS];
extern int nmons;
extern StrutMargin mon_struts[MAXMONS];
extern int rr_event_base, rr_error_base, rr_present;

/* ---- docks / bar ---- */
extern Dock *docks;
extern TrayIcon *trayicons;
extern Window traywin;
extern int tray_on;
extern int tray_active;
extern int barw;
extern Pixmap barpm;
extern GC bargc;
extern XftFont *barfont;
extern XftFont *barfont_fbs[6];
extern int bar_nfb;
extern XftDraw *barxd;
extern BarColors barcol;
extern int barcol_ok;

/* ---- sysmon cache ---- */
extern unsigned long long cpu_prev_total, cpu_prev_idle;
extern char vol_cache[16];
extern time_t vol_ts;

/* ---- config ---- */
extern Rule *rules;
extern unsigned nrules, caprules;
extern char **termcmd;
extern char **menucmd;
extern char **scratchcmd;
extern unsigned int MOD;
extern int BORDER;
extern unsigned long BORDER_FOCUS;
extern unsigned long BORDER_NORMAL;
extern unsigned long BAR_BG;
extern unsigned long BAR_FG;
extern unsigned long BAR_ACC;
extern unsigned long BAR_DIM;
extern unsigned long C_WS_ACT, C_WS_ACT_TX, C_WS_OCC, C_WS_EMP, C_MODE, C_TITLE, C_SYS, C_URGENT, C_SEP;
extern int h_ws_act, h_ws_act_tx, h_ws_occ, h_ws_emp, h_mode, h_title, h_sys, h_urgent, h_sep;
extern int BAR_WS_STYLE; /* 0=underline 1=block 2=pill (default) */
extern char *BAR_MODULES;  /* order+visibility: e.g. "cpu mem bat vol clock" */
extern char *CLOCK_FMT;    /* strftime fmt, default "%d/%m %H:%M" */
extern char *BAR_SEP_STR;  /* separator between right modules, default "·" */
extern int BAR_SHOW_TITLE; /* 1/0 */
extern int BAR_SHOW_LAYOUT;/* 1/0 */
/* vol segment hitbox (bar-relative x), refreshed every drawbar();
 * -1 when volume is unavailable so clicks fall through to ws view */
extern int vol_hit_x0, vol_hit_x1;
extern int BAR_H;
extern int BAR_GAP;
extern int BAR_PAD_L, BAR_PAD_R;
extern char *ico_cpu, *ico_mem, *ico_bat, *ico_vol, *ico_mute, *ico_clk;
extern int WS_W;
extern float ui_scale;
extern char *font_name;
extern int bar_on;
extern int gaps_on;
extern int gap_outer;
extern int gap_inner;
extern float def_mfact;
extern int def_nmaster;
extern int NWS;

/* ---- EWMH atoms ---- */
extern Atom A_TRAY_SEL, A_TRAY_OPCODE, A_XEMBED, A_XEMBED_INFO,
    A_TRAY_ORIENT, A_MANAGER;
extern Atom A_NET_SUPPORTED, A_NET_CLIENT_LIST, A_NET_ACTIVE_WINDOW,
    A_NET_WM_STATE, A_NET_WM_STATE_FS, A_NET_WM_STATE_HIDDEN,
    A_NET_WM_STATE_DA, A_NET_WM_WINDOW_TYPE,
    A_NET_WM_WINDOW_TYPE_DIALOG, A_NET_WM_WINDOW_TYPE_DOCK,
    A_NET_WM_WINDOW_TYPE_TOOLBAR, A_NET_WM_WINDOW_TYPE_SPLASH,
    A_NET_WM_WINDOW_TYPE_UTILITY, A_NET_CLOSE_WINDOW,
    A_NET_SUPPORTING_WM_CHECK, A_NET_WM_NAME,
    A_NET_WM_STRUT, A_NET_WM_STRUT_PARTIAL, A_NET_WORKAREA,
    A_NET_NUMBER_OF_DESKTOPS, A_NET_CURRENT_DESKTOP,
    A_NET_WM_DESKTOP, A_NET_DESKTOP_NAMES;

/* ---- cursors ---- */
extern Cursor cur_move, cur_resize, cur_hsplit;

/* ---- drag ---- */
extern Drag drag;

/* per-workspace layout conveniences */
#define LAYOUT  (ws_layout[curws])
#define MFACT   (ws_mfact[curws])
#define NMASTER (ws_nmaster[curws])

/* scale raw UI px -> device px; v<=0 stays 0 so border 0 / gap 0 keep meaning */
static inline int S(int v) {
    if (v <= 0) return 0;
    return (int)((float)v * ui_scale + 0.5f);
}
