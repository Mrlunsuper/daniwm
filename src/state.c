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
unsigned long BAR_BG  = 0x191724;
unsigned long BAR_FG  = 0xe0def4;
unsigned long BAR_ACC = 0xc4a7e7;
unsigned long BAR_DIM = 0x6e6a86;
unsigned long C_WS_ACT, C_WS_ACT_TX, C_WS_OCC, C_WS_EMP, C_MODE, C_TITLE, C_SYS, C_URGENT, C_SEP;
int h_ws_act, h_ws_act_tx, h_ws_occ, h_ws_emp, h_mode, h_title, h_sys, h_urgent, h_sep;
unsigned long C_TASK_ACT, C_TASK_ACT_TX, C_TASK_TEXT;
int h_task_act, h_task_act_tx, h_task_text;
int BAR_WS_STYLE = 2;
int BAR_TASK_STYLE = -1; /* follow BAR_WS_STYLE */
char *BAR_MODULES = NULL;
char *CLOCK_FMT = NULL;
char *BAR_SEP_STR = NULL;
int BAR_SHOW_TITLE = 1;
int BAR_SHOW_LAYOUT = 1;
int BAR_SHOW_TASKS = 1;
int BAR_TASK_W = 0;
int BAR_TASK_PAD = 6;
int task_nhit = 0;
int task_hit_x0[MAXTASKHIT];
int task_hit_x1[MAXTASKHIT];
Window task_hit_win[MAXTASKHIT];
int vol_hit_x0 = -1, vol_hit_x1 = -1;
int BAR_H = 24;
int BAR_GAP = 3;
int BAR_PAD_L = 10, BAR_PAD_R = 12;
char *ico_cpu = NULL, *ico_mem = NULL, *ico_bat = NULL, *ico_vol = NULL, *ico_mute = NULL, *ico_clk = NULL;
int WS_W = 46;
int BAR_WS_PAD = 12;
float ui_scale = 1.0f;
char *font_name = NULL;
int bar_on = 1;
int gaps_on = 1;
int gap_outer = 10;
int gap_inner = 8;
float def_mfact = 0.55f;
int def_nmaster = 1;
int NWS = 5;
char *ws_names[MAXWS] = { 0 };
char *RENAME_CMD = NULL;
char *ws_icons[MAXWS] = { 0 };
char progpath[1024] = "";

Display *dpy;
Window root, bar, checkwin;
Pixmap barpm = None;
GC bargc;
XftFont *barfont = NULL;
XftFont *barfont_fbs[6];
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
int prevws = 0;
Layout *ws_layout = NULL;
float *ws_mfact = NULL;
int *ws_nmaster = NULL;
int nws_alloc = 0;

struct MonitorGeom mons[MAXMONS];
int nmons = 1;
int rr_event_base = 0, rr_error_base = 0, rr_present = 0;

StrutMargin mon_struts[MAXMONS];

Dock *docks = NULL;
TrayIcon *trayicons = NULL;
Window traywin = None;
int tray_on = 1;
int tray_active = 0;
int TRAY_GAP = 6;
int barw = 0;

Atom A_TRAY_SEL = None, A_TRAY_OPCODE = None, A_XEMBED = None, A_XEMBED_INFO = None,
    A_TRAY_ORIENT = None, A_MANAGER = None;
Atom A_NET_SUPPORTED, A_NET_CLIENT_LIST, A_NET_ACTIVE_WINDOW,
    A_NET_WM_STATE, A_NET_WM_STATE_FS, A_NET_WM_STATE_HIDDEN,
    A_NET_WM_STATE_DA, A_NET_WM_STATE_MODAL, A_NET_WM_WINDOW_TYPE,
    A_NET_WM_WINDOW_TYPE_DIALOG, A_NET_WM_WINDOW_TYPE_DOCK,
    A_NET_WM_WINDOW_TYPE_TOOLBAR, A_NET_WM_WINDOW_TYPE_SPLASH,
    A_NET_WM_WINDOW_TYPE_UTILITY, A_NET_WM_WINDOW_TYPE_MENU,
    A_NET_WM_WINDOW_TYPE_DROPDOWN, A_NET_WM_WINDOW_TYPE_POPUP,
    A_NET_WM_WINDOW_TYPE_TOOLTIP, A_NET_WM_WINDOW_TYPE_NOTIF,
    A_NET_WM_WINDOW_TYPE_COMBO, A_NET_WM_WINDOW_TYPE_DND, A_NET_CLOSE_WINDOW,
    A_NET_SUPPORTING_WM_CHECK, A_NET_WM_NAME, A_NET_WM_PID,
    A_NET_WM_STRUT, A_NET_WM_STRUT_PARTIAL, A_NET_WORKAREA,
    A_NET_NUMBER_OF_DESKTOPS, A_NET_CURRENT_DESKTOP,
    A_NET_WM_DESKTOP, A_NET_DESKTOP_NAMES;

Cursor cur_move = None, cur_resize = None, cur_hsplit = None;
Drag drag = { 0 };
