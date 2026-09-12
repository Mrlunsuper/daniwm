#include "state.h"

Rule *rules = NULL;
unsigned nrules = 0, caprules = 0;
char **termcmd = NULL;
char **menucmd = NULL;
char **scratchcmd = NULL;
unsigned int MOD = Mod4Mask;
int BORDER = 2;
unsigned long BORDER_FOCUS  = 0x7aa2f7;
unsigned long BORDER_NORMAL = 0x333333;
unsigned long BAR_BG  = 0x1e1e2e;
unsigned long BAR_FG  = 0xcdd6f4;
unsigned long BAR_ACC = 0x7aa2f7;
unsigned long BAR_DIM = 0x6c7086;
unsigned long C_WS_ACT, C_WS_ACT_TX, C_WS_OCC, C_WS_EMP, C_MODE, C_TITLE, C_SYS;
int h_ws_act, h_ws_act_tx, h_ws_occ, h_ws_emp, h_mode, h_title, h_sys;
int BAR_H = 24;
int WS_W = 40;
float ui_scale = 1.0f;
char *font_name = NULL;
int bar_on = 1;
int gaps_on = 1;
int gap_outer = 10;
int gap_inner = 8;
float def_mfact = 0.55f;
int def_nmaster = 1;
int NWS = 5;

Display *dpy;
Window root, bar, checkwin;
Pixmap barpm = None;
GC bargc;
XftFont *barfont = NULL;
XftFont *barfont_fbs[4];
int bar_nfb = 0;
XftDraw *barxd = NULL;
BarColors barcol;
int barcol_ok = 0;

unsigned long long cpu_prev_total = 0, cpu_prev_idle = 0;
char vol_cache[16] = "";
time_t vol_ts = 0;

int screen, sw, sh;
Client *clients = NULL;
Client *sel = NULL;
Client **ws_sel = NULL;
int curws = 0;
Layout *ws_layout = NULL;
float *ws_mfact = NULL;
int *ws_nmaster = NULL;
int nws_alloc = 0;

struct MonitorGeom mons[MAXMONS];
int nmons = 1;
int rr_event_base = 0, rr_error_base = 0, rr_present = 0;

StrutMargin mon_struts[MAXMONS];

Dock *docks = NULL;
int barw = 0;

Atom A_NET_SUPPORTED, A_NET_CLIENT_LIST, A_NET_ACTIVE_WINDOW,
    A_NET_WM_STATE, A_NET_WM_STATE_FS, A_NET_WM_STATE_HIDDEN,
    A_NET_WM_STATE_DA, A_NET_WM_WINDOW_TYPE,
    A_NET_WM_WINDOW_TYPE_DIALOG, A_NET_WM_WINDOW_TYPE_DOCK,
    A_NET_WM_WINDOW_TYPE_TOOLBAR, A_NET_WM_WINDOW_TYPE_SPLASH,
    A_NET_WM_WINDOW_TYPE_UTILITY, A_NET_CLOSE_WINDOW,
    A_NET_SUPPORTING_WM_CHECK, A_NET_WM_NAME,
    A_NET_WM_STRUT, A_NET_WM_STRUT_PARTIAL, A_NET_WORKAREA,
    A_NET_NUMBER_OF_DESKTOPS, A_NET_CURRENT_DESKTOP,
    A_NET_WM_DESKTOP, A_NET_DESKTOP_NAMES;

Cursor cur_move = None, cur_resize = None, cur_hsplit = None;
Drag drag = { 0 };
