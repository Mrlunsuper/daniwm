/* run.c - dani-run: rofi replacement for daniwm (drun + fav + power).
 * Single file, Xlib + Xft, no extra deps beyond the current Makefile.
 *
 *   dani-run            default drun: vertical fuzzy list
 *   dani-run --fav      favorite apps: icon grid (filterable via input)
 *   dani-run --power    power menu: icon grid (reuses fav UI)
 *   dani-run --list     dump "Name<TAB>Exec" to stdout, no X (test/headless)
 *
 * Config: $XDG_CONFIG_HOME/daniwm/run.config, fallback ~/.config/daniwm/run.config
 * (symlink from the repo resolves itself). key = value, # comment. app=/power= can
 * repeat over lines; the first line clears the defaults (like daniwm bind/rule).
 *
 *   font = SpaceMono Nerd Font:size=11
 *   cols = 5
 *   lines = 2
 *   drun_lines = 12     (rows in the drun list)
 *   width = 800         (drun window width)
 *   cell_w = 136        (fav/power grid cell width)
 *   cell_h = 112        (fav/power grid cell height)
 *   input_h = 44        (input band height)
 *   padding = 0         (inner margin, all sides: top/right/bottom/left)
 *   padding_top/right/bottom/left = N   (per-side override of padding)
 *   drun_padding*  / grid_padding* = N  (per-mode override for drun | grid)
 *   row_gap = 6         (vertical gap between drun rows)
 *   cell_gap = 6        (gap between grid cells, both axes)
 *   cell_gap_x / cell_gap_y = N         (per-axis override of cell_gap)
 *   margin = 12         (left/right margin of the selection pill)
 *   drun_icons = 1      (0/1: real icon-theme PNG icon next to each .desktop app)
 *   icon_theme =        (optional: icon theme name; default = GTK theme)
 *   app = <icon>;<name>;<cmd>      (split only on the first 2 ';', cmd keeps spaces)
 *   power = <icon>;<name>;<cmd>
 *
 * Empty icon -> use the first letter of the name. cmd runs via wordexp like menucmd.
 * Theme (bg/fg/acc/dim + font fallback) is read from the daniwm config, run.config
 * `font` takes precedence when set.
 */
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>
#include <X11/Xft/Xft.h>
#include <X11/keysym.h>

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <locale.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <wordexp.h>

/* Rosé Pine Main (same as the daniwm defaults in config) */
#define DEF_BG  0x191724
#define DEF_FG  0xe0def4
#define DEF_ACC 0xc4a7e7
#define DEF_DIM 0x6e6a86

#define MAXQ 256
#define HIST_MAX 200

typedef enum { MODE_DRUN, MODE_FAV, MODE_POWER, MODE_CALC } Mode;

typedef struct {
    char *name;     /* display name */
    char *exec;     /* command to run (%f stripped, wrapped in term if needed) */
    char *icon;     /* Nerd glyph (may be "" = use first letter) */
    int terminal;   /* .desktop Terminal=true (wrapped at load time) */
    int hist;       /* usage count (history) */
    int score;      /* fuzzy score for the current query */
} Entry;

/* ---- config ---- */
static char font_pat[256] = "";
static int opt_cols = 5, opt_lines = 2;
#ifdef HAVE_CAIRO
static char icon_theme[64];
#endif

static Entry *entries = NULL;
static unsigned nentries = 0, capentries = 0;

static int saw_app = 0, saw_power = 0;
static Entry *fav = NULL, *power = NULL;
static unsigned nfav = 0, capfav = 0, npower = 0, cappower = 0;

/* theme read from the daniwm config (run.config `font` may override) */
static unsigned long T_BG = DEF_BG, T_FG = DEF_FG, T_ACC = DEF_ACC, T_DIM = DEF_DIM;
static char T_FONT[256] = "";

/* ---- X ---- */
static Display *dpy;
static int screen;
static Window win;
static GC gc;
static XftDraw *xd;
static XftFont *f_main, *f_big;
static XftFont *fbs[6];
static int nfb;
static int text_fb_done;
static XftColor c_bg, c_fg, c_acc, c_dim;
static XftColor c_band, c_div, c_sel, c_selout;
static XftColor c_love, c_gold, c_pine, c_foam;
static int opt_width = 560; /* drun window width */
static int opt_drun_icons = 1; /* Nerd glyph per .desktop app in the drun list */
static XIM xim;
static XIC xic;
static int WW, HH;
static int input_h = 44, row_h = 34, list_rows = 8; /* drun_lines */
static int cell_w = 136, cell_h = 112;
static int footer_h = 26;
/* spacing (all configurable):
 *   pad_t/pad_r/pad_b/pad_l = inner window margin per side
 *   row_gap   = vertical gap between drun rows
 *   cell_gap_x/cell_gap_y = horizontal/vertical gap between grid cells
 *   margin    = left/right margin of the selection pill (drun) */
static int opt_pad_t = 0, opt_pad_r = 0, opt_pad_b = 0, opt_pad_l = 0;
/* per-mode padding overrides (default -1 = fall back to opt_pad_*):
 * drun_padding* applies to MODE_DRUN, grid_padding* to MODE_FAV/MODE_POWER. */
static int drun_pad_t = -1, drun_pad_r = -1, drun_pad_b = -1, drun_pad_l = -1;
static int grid_pad_t = -1, grid_pad_r = -1, grid_pad_b = -1, grid_pad_l = -1;
static int opt_row_gap = 6, opt_cell_gap_x = 0, opt_cell_gap_y = 6, opt_margin = 12;

/* ---- state ---- */
static Mode mode = MODE_DRUN;
static char query[MAXQ];
static int qlen;
static int *filt;
static int nfilt, capfilt;
static int sel, scroll;

/* ============ helpers ============ */
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
/* strip a trailing "# comment" from a value; a leading '#' is hex color -> keep */
static void strip_comment(char *v) {
    char *p, *h;
    if (!v) return;
    p = (*v == '#') ? v + 1 : v;
    h = strchr(p, '#');
    if (h) { *h = 0; trim(v); }
}
static int parse_hex(const char *s, unsigned long *out) {
    char *e = NULL;
    unsigned long v;
    while (isspace((unsigned char)*s)) s++;
    if (*s == '#') s++;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s += 2;
    if (!*s) return 0;
    v = strtoul(s, &e, 16);
    if (!e || *e) return 0;
    *out = v & 0xffffff;
    return 1;
}
static void push_entry_vec(Entry **vec, unsigned *n, unsigned *cap,
        char *name, char *exec, char *icon) {
    Entry *nv;
    if (*n == *cap) {
        unsigned nc = *cap ? *cap * 2 : 16;
        nv = realloc(*vec, nc * sizeof(**vec));
        if (!nv) { free(name); free(exec); free(icon); return; }
        *vec = nv; *cap = nc;
    }
    (*vec)[*n].name = name;
    (*vec)[*n].exec = exec;
    (*vec)[*n].icon = icon ? icon : xstrdup("");
    (*vec)[*n].terminal = 0;
    (*vec)[*n].hist = 0;
    (*vec)[*n].score = 0;
    (*n)++;
}
/* triple "icon;name;cmd": split only on the first 2 ';'. 1 ';' -> empty icon. */
static int parse_triple(const char *val, char **icon, char **name, char **cmd) {
    const char *s1, *s2;
    char *a, *b, *c;
    *icon = *name = *cmd = NULL;
    s1 = strchr(val, ';');
    if (!s1) return 0;
    s2 = strchr(s1 + 1, ';');
    if (!s2) {
        /* "name;cmd", empty icon */
        a = xstrdup("");
        b = malloc((size_t)(s1 - val) + 1);
        if (!a || !b) { free(a); free(b); return 0; }
        memcpy(b, val, (size_t)(s1 - val));
        b[s1 - val] = 0;
        c = xstrdup(s1 + 1);
        if (!c) { free(a); free(b); return 0; }
    } else {
        a = malloc((size_t)(s1 - val) + 1);
        b = malloc((size_t)(s2 - s1));
        if (!a || !b) { free(a); free(b); return 0; }
        memcpy(a, val, (size_t)(s1 - val));
        a[s1 - val] = 0;
        memcpy(b, s1 + 1, (size_t)(s2 - s1 - 1));
        b[s2 - s1 - 1] = 0;
        c = xstrdup(s2 + 1);
        if (!c) { free(a); free(b); return 0; }
    }
    {
        char *ta = trim(a), *tb = trim(b), *tc = trim(c);
        if (!*tb || !*tc) { free(a); free(b); free(c); return 0; }
        *icon = xstrdup(ta);
        *name = xstrdup(tb);
        *cmd = xstrdup(tc);
        free(a); free(b); free(c);
        if (!*icon || !*name || !*cmd) {
            free(*icon); free(*name); free(*cmd);
            *icon = *name = *cmd = NULL;
            return 0;
        }
    }
    return 1;
}

/* ============ defaults (cleared when the config has its first app=/power= line) ============ */
/* after load_run_config(): merge per-mode padding overrides into opt_pad_*.
 * drun_padding* wins for MODE_DRUN, grid_padding* for fav/power; the plain
 * padding* keys act as the base for every mode. */
static void resolve_padding(void) {
    if (mode == MODE_DRUN) {
        if (drun_pad_t >= 0) opt_pad_t = drun_pad_t;
        if (drun_pad_r >= 0) opt_pad_r = drun_pad_r;
        if (drun_pad_b >= 0) opt_pad_b = drun_pad_b;
        if (drun_pad_l >= 0) opt_pad_l = drun_pad_l;
    } else if (mode == MODE_FAV || mode == MODE_POWER) {
        if (grid_pad_t >= 0) opt_pad_t = grid_pad_t;
        if (grid_pad_r >= 0) opt_pad_r = grid_pad_r;
        if (grid_pad_b >= 0) opt_pad_b = grid_pad_b;
        if (grid_pad_l >= 0) opt_pad_l = grid_pad_l;
    }
}
static void defaults(void) {
    char *a, *b, *c;
    a = xstrdup(""); b = xstrdup("Firefox"); c = xstrdup("firefox");
    if (a && b && c) push_entry_vec(&fav, &nfav, &capfav, b, c, a);
    else { free(a); free(b); free(c); }
    a = xstrdup(""); b = xstrdup("Terminal"); c = xstrdup("alacritty");
    if (a && b && c) push_entry_vec(&fav, &nfav, &capfav, b, c, a);
    else { free(a); free(b); free(c); }
    a = xstrdup(""); b = xstrdup("Files"); c = xstrdup("thunar");
    if (a && b && c) push_entry_vec(&fav, &nfav, &capfav, b, c, a);
    else { free(a); free(b); free(c); }
    a = xstrdup(""); b = xstrdup("Lock"); c = xstrdup("loginctl lock-session");
    if (a && b && c) push_entry_vec(&power, &npower, &cappower, b, c, a);
    else { free(a); free(b); free(c); }
    a = xstrdup(""); b = xstrdup("Logout"); c = xstrdup("pkill daniwm");
    if (a && b && c) push_entry_vec(&power, &npower, &cappower, b, c, a);
    else { free(a); free(b); free(c); }
    a = xstrdup(""); b = xstrdup("Reboot"); c = xstrdup("systemctl reboot");
    if (a && b && c) push_entry_vec(&power, &npower, &cappower, b, c, a);
    else { free(a); free(b); free(c); }
    a = xstrdup(""); b = xstrdup("Off"); c = xstrdup("systemctl poweroff");
    if (a && b && c) push_entry_vec(&power, &npower, &cappower, b, c, a);
    else { free(a); free(b); free(c); }
}

/* ============ config paths ============ */
static void run_config_path(char *out, size_t n) {
    const char *xdg = getenv("XDG_CONFIG_HOME"), *home = getenv("HOME");
    if (xdg && *xdg) snprintf(out, n, "%s/daniwm/run.config", xdg);
    else if (home && *home) snprintf(out, n, "%s/.config/daniwm/run.config", home);
    else snprintf(out, n, "%s", "");
}
static void wm_config_path(char *out, size_t n) {
    const char *xdg = getenv("XDG_CONFIG_HOME"), *home = getenv("HOME");
    if (xdg && *xdg) snprintf(out, n, "%s/daniwm/config", xdg);
    else if (home && *home) snprintf(out, n, "%s/.config/daniwm/config", home);
    else snprintf(out, n, "%s", "");
}
/* read 5 theme keys from the daniwm config: bar_bg/fg/acc/dim + font */
static void load_wm_theme(void) {
    char path[1024], line[1024];
    FILE *f;
    wm_config_path(path, sizeof(path));
    if (!path[0]) return;
    f = fopen(path, "r");
    if (!f) return;
    while (fgets(line, sizeof(line), f)) {
        char *s = trim(line), *eq, *k, *v;
        unsigned long h;
        if (!*s || *s == '#' || *s == ';') continue;
        eq = strchr(s, '=');
        if (!eq) continue;
        *eq = 0;
        k = trim(s); v = trim(eq + 1);
        for (char *p = k; *p; p++) *p = (char)tolower((unsigned char)*p);
        strip_comment(v);
        if (!strcmp(k, "bar_bg") && parse_hex(v, &h)) T_BG = h;
        else if (!strcmp(k, "bar_fg") && parse_hex(v, &h)) T_FG = h;
        else if (!strcmp(k, "bar_acc") && parse_hex(v, &h)) T_ACC = h;
        else if (!strcmp(k, "bar_dim") && parse_hex(v, &h)) T_DIM = h;
        else if (!strcmp(k, "font") && *v) snprintf(T_FONT, sizeof(T_FONT), "%.255s", v);
    }
    fclose(f);
}
static void load_run_config(void) {
    char path[1024], line[1024];
    FILE *f;
    long v;
    run_config_path(path, sizeof(path));
    if (!path[0]) return;
    f = fopen(path, "r");
    if (!f) return;
    while (fgets(line, sizeof(line), f)) {
        char *s = trim(line), *eq, *k, *vv;
        if (!*s || *s == '#' || *s == ';') continue;
        eq = strchr(s, '=');
        if (!eq) continue;
        *eq = 0;
        k = trim(s); vv = trim(eq + 1);
        for (char *p = k; *p; p++) *p = (char)tolower((unsigned char)*p);
        strip_comment(vv);
        if (!strcmp(k, "font")) {
            if (*vv) snprintf(font_pat, sizeof(font_pat), "%.255s", vv);
        } else if (!strcmp(k, "cols")) {
            v = strtol(vv, NULL, 10);
            if (v >= 1 && v <= 9) opt_cols = (int)v;
        } else if (!strcmp(k, "lines")) {
            v = strtol(vv, NULL, 10);
            if (v >= 1 && v <= 4) opt_lines = (int)v;
        } else if (!strcmp(k, "drun_lines")) {
            v = strtol(vv, NULL, 10);
            if (v >= 3 && v <= 16) list_rows = (int)v;
        } else if (!strcmp(k, "width")) {
            v = strtol(vv, NULL, 10);
            if (v >= 320 && v <= 1200) opt_width = (int)v;
        } else if (!strcmp(k, "cell_w")) {
            v = strtol(vv, NULL, 10);
            if (v >= 80 && v <= 240) cell_w = (int)v;
        } else if (!strcmp(k, "cell_h")) {
            v = strtol(vv, NULL, 10);
            if (v >= 64 && v <= 200) cell_h = (int)v;
        } else if (!strcmp(k, "drun_icons")) {
            long iv = strtol(vv, NULL, 10);
            opt_drun_icons = iv != 0;
        } else if (!strcmp(k, "padding")) {
            v = strtol(vv, NULL, 10);
            if (v >= 0 && v <= 64)
                opt_pad_t = opt_pad_r = opt_pad_b = opt_pad_l = (int)v;
        } else if (!strcmp(k, "padding_top")) {
            v = strtol(vv, NULL, 10);
            if (v >= 0 && v <= 64) opt_pad_t = (int)v;
        } else if (!strcmp(k, "padding_right")) {
            v = strtol(vv, NULL, 10);
            if (v >= 0 && v <= 64) opt_pad_r = (int)v;
        } else if (!strcmp(k, "padding_bottom")) {
            v = strtol(vv, NULL, 10);
            if (v >= 0 && v <= 64) opt_pad_b = (int)v;
        } else if (!strcmp(k, "padding_left")) {
            v = strtol(vv, NULL, 10);
            if (v >= 0 && v <= 64) opt_pad_l = (int)v;
        } else if (!strcmp(k, "drun_padding")) {
            v = strtol(vv, NULL, 10);
            if (v >= 0 && v <= 64)
                drun_pad_t = drun_pad_r = drun_pad_b = drun_pad_l = (int)v;
        } else if (!strcmp(k, "drun_padding_top")) {
            v = strtol(vv, NULL, 10);
            if (v >= 0 && v <= 64) drun_pad_t = (int)v;
        } else if (!strcmp(k, "drun_padding_right")) {
            v = strtol(vv, NULL, 10);
            if (v >= 0 && v <= 64) drun_pad_r = (int)v;
        } else if (!strcmp(k, "drun_padding_bottom")) {
            v = strtol(vv, NULL, 10);
            if (v >= 0 && v <= 64) drun_pad_b = (int)v;
        } else if (!strcmp(k, "drun_padding_left")) {
            v = strtol(vv, NULL, 10);
            if (v >= 0 && v <= 64) drun_pad_l = (int)v;
        } else if (!strcmp(k, "grid_padding")) {
            v = strtol(vv, NULL, 10);
            if (v >= 0 && v <= 64)
                grid_pad_t = grid_pad_r = grid_pad_b = grid_pad_l = (int)v;
        } else if (!strcmp(k, "grid_padding_top")) {
            v = strtol(vv, NULL, 10);
            if (v >= 0 && v <= 64) grid_pad_t = (int)v;
        } else if (!strcmp(k, "grid_padding_right")) {
            v = strtol(vv, NULL, 10);
            if (v >= 0 && v <= 64) grid_pad_r = (int)v;
        } else if (!strcmp(k, "grid_padding_bottom")) {
            v = strtol(vv, NULL, 10);
            if (v >= 0 && v <= 64) grid_pad_b = (int)v;
        } else if (!strcmp(k, "grid_padding_left")) {
            v = strtol(vv, NULL, 10);
            if (v >= 0 && v <= 64) grid_pad_l = (int)v;
        } else if (!strcmp(k, "row_gap")) {
            v = strtol(vv, NULL, 10);
            if (v >= 0 && v <= 32) opt_row_gap = (int)v;
        } else if (!strcmp(k, "cell_gap")) {
            v = strtol(vv, NULL, 10);
            if (v >= 0 && v <= 64) opt_cell_gap_x = opt_cell_gap_y = (int)v;
        } else if (!strcmp(k, "cell_gap_x")) {
            v = strtol(vv, NULL, 10);
            if (v >= 0 && v <= 64) opt_cell_gap_x = (int)v;
        } else if (!strcmp(k, "cell_gap_y")) {
            v = strtol(vv, NULL, 10);
            if (v >= 0 && v <= 64) opt_cell_gap_y = (int)v;
        } else if (!strcmp(k, "margin")) {
            v = strtol(vv, NULL, 10);
            if (v >= 0 && v <= 64) opt_margin = (int)v;
        } else if (!strcmp(k, "input_h")) {
            v = strtol(vv, NULL, 10);
            if (v >= 24 && v <= 96) input_h = (int)v;
#ifdef HAVE_CAIRO
        } else if (!strcmp(k, "icon_theme")) {
            if (*vv) snprintf(icon_theme, sizeof(icon_theme), "%.63s", vv);
#endif
        } else if (!strcmp(k, "app")) {
            char *ic, *nm, *cm;
            if (!parse_triple(vv, &ic, &nm, &cm)) {
                fprintf(stderr, "dani-run: bad app '%s' (want icon;name;cmd)\n", vv);
                continue;
            }
            if (!saw_app) {
                for (unsigned i = 0; i < nfav; i++) {
                    free(fav[i].name); free(fav[i].exec); free(fav[i].icon);
                }
                nfav = 0; saw_app = 1;
            }
            push_entry_vec(&fav, &nfav, &capfav, nm, cm, ic);
        } else if (!strcmp(k, "power")) {
            char *ic, *nm, *cm;
            if (!parse_triple(vv, &ic, &nm, &cm)) {
                fprintf(stderr, "dani-run: bad power '%s' (want icon;name;cmd)\n", vv);
                continue;
            }
            if (!saw_power) {
                for (unsigned i = 0; i < npower; i++) {
                    free(power[i].name); free(power[i].exec); free(power[i].icon);
                }
                npower = 0; saw_power = 1;
            }
            push_entry_vec(&power, &npower, &cappower, nm, cm, ic);
        }
    }
    fclose(f);
}

/* ============ .desktop scan (drun) ============ */
static char *clean_exec(const char *src) {
    /* drop field codes %f/%u/... (%% -> %), trim extra whitespace */
    size_t n = strlen(src);
    char *out = malloc(n + 1), *d;
    size_t i = 0;
    if (!out) return NULL;
    d = out;
    while (src[i]) {
        if (src[i] == '%' && src[i + 1]) {
            if (src[i + 1] == '%') { *d++ = '%'; i += 2; }
            else i += 2;
        } else *d++ = src[i++];
    }
    *d = 0;
    return out;
}
static void scan_dir(const char *dir) {
    DIR *dp = opendir(dir);
    struct dirent *de;
    if (!dp) return;
    while ((de = readdir(dp))) {
        size_t L = strlen(de->d_name);
        char path[2048], line[1024];
        FILE *f;
        char *name_plain = NULL, *name_any = NULL, *exec = NULL, *icon_name = NULL;
        int terminal = 0, hidden = 0;
        if (L < 9 || strcmp(de->d_name + L - 8, ".desktop")) continue;
        snprintf(path, sizeof(path), "%s/%s", dir, de->d_name);
        f = fopen(path, "r");
        if (!f) continue;
        while (fgets(line, sizeof(line), f)) {
            char *s = trim(line);
            if (!*s || *s == '#' || *s == '[') continue;
            if (!strncmp(s, "Name=", 5) && !name_plain) name_plain = xstrdup(trim(s + 5));
            else if (!strncmp(s, "Name[", 5) && !name_any) {
                char *rb = strchr(s, '=');
                if (rb && *(trim(rb + 1))) name_any = xstrdup(trim(rb + 1));
            } else if (!strncmp(s, "Exec=", 5) && !exec) exec = xstrdup(trim(s + 5));
            else if (!strncmp(s, "Icon=", 5) && !icon_name) icon_name = xstrdup(trim(s + 5));
            else if (!strncmp(s, "Terminal=", 9))
                terminal = !strcasecmp(trim(s + 9), "true");
            else if (!strncmp(s, "NoDisplay=", 10) || !strncmp(s, "Hidden=", 7))
                if (!strcasecmp(trim(strchr(s, '=') + 1), "true")) hidden = 1;
        }
        fclose(f);
        {
            char *nm = name_plain ? name_plain : name_any;
            name_plain = NULL; name_any = NULL;
            if (!nm || !*nm || !exec || !*exec || hidden) {
                free(nm); free(exec); free(icon_name);
                continue;
            }
            /* dedupe by name (user dir is scanned first, so it wins) */
            {
                int dup = 0;
                for (unsigned i = 0; i < nentries; i++)
                    if (!strcmp(entries[i].name, nm)) { dup = 1; break; }
                if (dup) { free(nm); free(exec); free(icon_name); continue; }
            }
            {
                char *ce = clean_exec(exec);
                free(exec);
                if (!ce || !*trim(ce)) { free(nm); free(ce); free(icon_name); continue; }
                {
                    char *t = trim(ce);
                    char *final_exec;
                    if (terminal) {
                        /* wrap in terminal: ${TERMINAL:-alacritty} -e <exec> */
                        const char *te = getenv("TERMINAL");
                        if (!te || !*te) te = "alacritty";
                        final_exec = malloc(strlen(te) + 4 + strlen(t) + 1);
                        if (final_exec) sprintf(final_exec, "%s -e %s", te, t);
                        free(ce);
                    } else {
                        final_exec = xstrdup(t);
                        free(ce);
                    }
                    if (!final_exec) { free(nm); free(icon_name); continue; }
                    push_entry_vec(&entries, &nentries, &capentries, nm, final_exec,
                        icon_name ? icon_name : xstrdup(""));
                    entries[nentries - 1].terminal = terminal;
                }
            }
        }
    }
    closedir(dp);
}
static void load_drun(void) {
    char user[1024];
    const char *home = getenv("HOME");
    if (home && *home) {
        snprintf(user, sizeof(user), "%s/.local/share/applications", home);
        scan_dir(user);
    }
    scan_dir("/usr/share/applications");
}

/* ============ history (~/.cache/dani-run/history) ============ */
static void hist_path(char *out, size_t n) {
    const char *home = getenv("HOME");
    const char *xdg = getenv("XDG_CACHE_HOME");
    if (xdg && *xdg) snprintf(out, n, "%s/dani-run/history", xdg);
    else if (home && *home) snprintf(out, n, "%s/.cache/dani-run/history", home);
    else snprintf(out, n, "%s", "");
}
static Entry *cur_vec(unsigned *n) {
    if (mode == MODE_FAV) { *n = nfav; return fav; }
    if (mode == MODE_POWER) { *n = npower; return power; }
    if (mode == MODE_CALC) { *n = 0; return NULL; }
    *n = nentries; return entries;
}
static void hist_load(void) {
    char path[1024], line[512];
    FILE *f;
    hist_path(path, sizeof(path));
    if (!path[0]) return;
    f = fopen(path, "r");
    if (!f) return;
    while (fgets(line, sizeof(line), f)) {
        char *tab, *nm;
        long c;
        unsigned n;
        Entry *vec = cur_vec(&n);
        tab = strchr(line, '\t');
        if (!tab) continue;
        *tab = 0;
        c = strtol(trim(line), NULL, 10);
        nm = trim(tab + 1);
        if (c <= 0 || !*nm) continue;
        for (unsigned i = 0; i < n; i++)
            if (!strcmp(vec[i].name, nm) && vec[i].hist == 0) { vec[i].hist = (int)c; break; }
    }
    fclose(f);
}
static void hist_save(const char *name) {
    char path[1024], line[512];
    FILE *f;
    /* read the old map */
    struct { char *k; int c; } map[HIST_MAX + 8];
    int nm = 0;
    hist_path(path, sizeof(path));
    if (!path[0] || !name) return;
    f = fopen(path, "r");
    if (f) {
        while (nm < HIST_MAX && fgets(line, sizeof(line), f)) {
            char *tab = strchr(line, '\t');
            long c;
            if (!tab) continue;
            *tab = 0;
            c = strtol(trim(line), NULL, 10);
            if (c > 0 && *trim(tab + 1)) {
                map[nm].k = xstrdup(trim(tab + 1));
                map[nm].c = (int)c;
                if (map[nm].k) nm++;
            }
        }
        fclose(f);
    }
    {
        int found = 0;
        for (int i = 0; i < nm; i++)
            if (!strcmp(map[i].k, name)) { map[i].c++; found = 1; break; }
        if (!found && nm < HIST_MAX) {
            map[nm].k = xstrdup(name);
            map[nm].c = 1;
            if (map[nm].k) nm++;
        }
        /* mkdir -p ~/.cache/dani-run */
        {
            char dir[1024];
            snprintf(dir, sizeof(dir), "%.1000s", path);
            {
                char *sl = strrchr(dir, '/');
                if (sl) {
                    *sl = 0;
                    mkdir(dir, 0755); /* parent ~/.cache thuong da co */
                }
            }
        }
        f = fopen(path, "w");
        if (f) {
            for (int i = 0; i < nm; i++) {
                fprintf(f, "%d\t%s\n", map[i].c, map[i].k);
                free(map[i].k);
            }
            fclose(f);
        } else {
            for (int i = 0; i < nm; i++) free(map[i].k);
        }
    }
}

/* ============ calc: recursive-descent (+ - * / % ^, (), funcs, pi/e) ============ */
typedef struct { const char *p; int ok; } PExpr;
static void pe_skip(PExpr *x) { while (isspace((unsigned char)*x->p)) x->p++; }
static double pe_expr(PExpr *x);
static double pe_unary(PExpr *x);
static double pe_func(PExpr *x, const char *name, double v) {
    if (!strcmp(name, "sqrt")) { if (v < 0) { x->ok = 0; return 0; } return sqrt(v); }
    if (!strcmp(name, "sin")) return sin(v);
    if (!strcmp(name, "cos")) return cos(v);
    if (!strcmp(name, "tan")) return tan(v);
    if (!strcmp(name, "asin")) { if (v < -1 || v > 1) { x->ok = 0; return 0; } return asin(v); }
    if (!strcmp(name, "acos")) { if (v < -1 || v > 1) { x->ok = 0; return 0; } return acos(v); }
    if (!strcmp(name, "atan")) return atan(v);
    if (!strcmp(name, "exp")) return exp(v);
    if (!strcmp(name, "abs")) return fabs(v);
    if (!strcmp(name, "ln")) { if (v <= 0) { x->ok = 0; return 0; } return log(v); }
    if (!strcmp(name, "log")) { if (v <= 0) { x->ok = 0; return 0; } return log10(v); }
    x->ok = 0; return 0;
}
static double pe_primary(PExpr *x) {
    double v;
    char *e;
    pe_skip(x);
    if (*x->p == '(') {
        x->p++;
        v = pe_expr(x);
        pe_skip(x);
        if (*x->p == ')') x->p++;
        else x->ok = 0;
        return v;
    }
    if (isalpha((unsigned char)*x->p)) {
        char name[16];
        int k = 0;
        while (isalpha((unsigned char)*x->p) && k < 15) name[k++] = (char)tolower((unsigned char)*x->p++);
        name[k] = 0;
        if (!strcmp(name, "pi")) return 3.141592653589793;
        if (!strcmp(name, "e")) return 2.718281828459045;
        pe_skip(x);
        if (*x->p != '(') { x->ok = 0; return 0; }
        x->p++;
        v = pe_expr(x);
        pe_skip(x);
        if (*x->p != ')') { x->ok = 0; return 0; }
        x->p++;
        return pe_func(x, name, v);
    }
    v = strtod(x->p, &e); /* LC_NUMERIC=C: decimal point is '.' (set in main) */
    if (e == x->p) { x->ok = 0; return 0; }
    x->p = e;
    return v;
}
static double pe_pow(PExpr *x) {
    double b = pe_unary(x);
    pe_skip(x);
    if (*x->p == '^') {
        double e;
        x->p++;
        e = pe_pow(x); /* right-assoc: 2^3^2 = 2^(3^2) */
        b = pow(b, e);
        if (!isfinite(b)) x->ok = 0;
    }
    return b;
}
static double pe_unary(PExpr *x) {
    pe_skip(x);
    if (*x->p == '-') { x->p++; return -pe_unary(x); }
    if (*x->p == '+') { x->p++; return pe_unary(x); }
    return pe_primary(x);
}
static double pe_term(PExpr *x) {
    double v = pe_pow(x);
    for (;;) {
        pe_skip(x);
        if (*x->p == '*') { x->p++; v *= pe_pow(x); }
        else if (*x->p == '/') {
            double d;
            x->p++; d = pe_pow(x);
            if (d == 0) { x->ok = 0; return 0; }
            v /= d;
        } else if (*x->p == '%') {
            double d;
            x->p++; d = pe_pow(x);
            if (d == 0) { x->ok = 0; return 0; }
            v = fmod(v, d);
        } else break;
        if (!isfinite(v)) { x->ok = 0; return 0; }
    }
    return v;
}
static double pe_expr(PExpr *x) {
    double v = pe_term(x);
    for (;;) {
        pe_skip(x);
        if (*x->p == '+') { x->p++; v += pe_term(x); }
        else if (*x->p == '-') { x->p++; v -= pe_term(x); }
        else break;
        if (!isfinite(v)) { x->ok = 0; return 0; }
    }
    return v;
}
/* 1 = valid, sets *out. Empty string / trailing chars / div-by-0 -> 0. */
static int calc_eval(const char *s, double *out) {
    PExpr x = { s, 1 };
    double v;
    if (!s || !*s) return 0;
    v = pe_expr(&x);
    pe_skip(&x);
    if (!x.ok || *x.p) return 0;
    if (v == 0) v = 0; /* -0 -> 0 */
    *out = v;
    return 1;
}
static void calc_str(double v, char *out, size_t n) {
    snprintf(out, n, "%.10g", v);
}
/* Copy the result to the clipboard (xclip, xsel fallback). Always returns (never blocks). */
static void to_clipboard(const char *s) {
    static const char *tools[][4] = {
        { "xclip", "-selection", "clipboard", NULL },
        { "xsel", "--clipboard", "--input", NULL },
    };
    for (unsigned t = 0; t < 2; t++) {
        int pfd[2];
        pid_t pid;
        if (pipe(pfd) != 0) return;
        pid = fork();
        if (pid == -1) { close(pfd[0]); close(pfd[1]); return; }
        if (pid == 0) {
            if (dpy) close(ConnectionNumber(dpy));
            setsid();
            dup2(pfd[0], 0);
            close(pfd[0]); close(pfd[1]);
            execvp(tools[t][0], (char *const *)tools[t]);
            _exit(127);
        }
        close(pfd[0]);
        {
            ssize_t w = write(pfd[1], s, strlen(s));
            (void)w; /* clipboard: best-effort, on error try the next tool/stdout */
        }
        close(pfd[1]);
        {
            int st;
            pid_t w = waitpid(pid, &st, 0);
            if (w == pid && WIFEXITED(st) && WEXITSTATUS(st) != 127) return; /* ok */
        }
        /* 127 = tool missing: try the next one */
    }
}

/* ============ fuzzy: case-insensitive subsequence, word-start > mid-word ============ */
static int fuzzy_score(const char *name, const char *q) {
    size_t n, m, ni = 0;
    int score = 0, last = -2;
    if (!q[0]) return 0;
    n = strlen(name); m = strlen(q);
    for (size_t qi = 0; qi < m; qi++) {
        char qc = (char)tolower((unsigned char)q[qi]);
        int found = -1;
        for (size_t k = ni; k < n; k++) {
            if ((char)tolower((unsigned char)name[k]) == qc) { found = (int)k; break; }
        }
        if (found < 0) return -1;
        if (found == 0 || name[found - 1] == ' ' || name[found - 1] == '-' ||
            name[found - 1] == '_' || name[found - 1] == '/') score += 10;
        else if (found == last + 1) score += 6;
        else if (found == last + 2) score += 3;
        else score += 1;
        last = found; ni = (size_t)found + 1;
    }
    return score * 100 - (int)n;
}
static int cmp_filt(const void *a, const void *b) {
    unsigned n;
    Entry *vec = cur_vec(&n);
    int ia = *(const int *)a, ib = *(const int *)b;
    if (vec[ib].score != vec[ia].score) return vec[ib].score - vec[ia].score;
    if (vec[ib].hist != vec[ia].hist) return vec[ib].hist - vec[ia].hist;
    return strcasecmp(vec[ia].name, vec[ib].name);
}
static void refilter(void) {
    unsigned n;
    Entry *vec = cur_vec(&n);
    nfilt = 0;
    if ((int)n + 1 > capfilt) {
        int nc = (int)n + 1;
        int *nv = realloc(filt, (size_t)nc * sizeof(*nv));
        if (!nv) return;
        filt = nv; capfilt = nc;
    }
    for (unsigned i = 0; i < n; i++) {
        int s = fuzzy_score(vec[i].name, query);
        if (s < 0) continue;
        vec[i].score = s;
        filt[nfilt++] = (int)i;
    }
    qsort(filt, (size_t)nfilt, sizeof(*filt), cmp_filt);
    if (sel >= nfilt) sel = nfilt ? nfilt - 1 : 0;
    if (sel < 0) sel = 0;
    if (mode == MODE_DRUN) {
        scroll = (sel / list_rows) * list_rows;
    } else {
        int per = opt_cols * opt_lines;
        if (per < 1) per = 1;
        scroll = (sel / per) * per;
    }
}

/* ============ font / draw (reuses the bar.c pattern) ============ */
static FcChar32 u8dec(const unsigned char *s, size_t left, int *cl) {
    unsigned char b0 = s[0];
    FcChar32 u;
    if (b0 < 0x80) { *cl = 1; return b0; }
    if ((b0 >> 5) == 0x6 && left >= 2 && (s[1] & 0xC0) == 0x80) {
        *cl = 2; u = ((FcChar32)(b0 & 0x1f) << 6) | (s[1] & 0x3f); return u;
    }
    if ((b0 >> 4) == 0xE && left >= 3 && (s[1] & 0xC0) == 0x80 && (s[2] & 0xC0) == 0x80) {
        *cl = 3;
        u = ((FcChar32)(b0 & 0x0f) << 12) | ((FcChar32)(s[1] & 0x3f) << 6) | (s[2] & 0x3f);
        return u;
    }
    if ((b0 >> 3) == 0x1E && left >= 4 && (s[1] & 0xC0) == 0x80 &&
        (s[2] & 0xC0) == 0x80 && (s[3] & 0xC0) == 0x80) {
        *cl = 4;
        u = ((FcChar32)(b0 & 0x07) << 18) | ((FcChar32)(s[1] & 0x3f) << 12) |
            ((FcChar32)(s[2] & 0x3f) << 6) | (s[3] & 0x3f);
        return u;
    }
    *cl = 1; return 0xFFFD;
}
static void ensure_text_fbs(void) {
    static const char *cands[] = { "Noto Sans", "DejaVu Sans", "Sans", NULL };
    text_fb_done = 1;
    for (int i = 0; cands[i] && nfb < 6; i++) {
        XftFont *f = XftFontOpenName(dpy, screen, cands[i]);
        if (f) fbs[nfb++] = f;
    }
}
static XftFont *glyph_font(FcChar32 u) {
    int cl0;
    (void)cl0;
    if (u < 0x80 && f_main) return f_main;
    if (f_main && XftCharExists(dpy, f_main, u)) return f_main;
    for (int i = 0; i < nfb; i++)
        if (fbs[i] && XftCharExists(dpy, fbs[i], u)) return fbs[i];
    if (!text_fb_done) {
        int before = nfb;
        ensure_text_fbs();
        for (int i = before; i < nfb; i++)
            if (fbs[i] && XftCharExists(dpy, fbs[i], u)) return fbs[i];
    }
    return f_main;
}
/* icon or ASCII fallback: the first codepoint must exist, otherwise tofu */
static const char *pick_icon(const char *icon_utf8, const char *ascii) {
    FcChar32 u;
    int cl;
    if (!icon_utf8 || !*icon_utf8 || !f_main) return ascii;
    u = u8dec((const unsigned char *)icon_utf8, strlen(icon_utf8), &cl);
    if (XftCharExists(dpy, f_main, u)) return icon_utf8;
    for (int i = 0; i < nfb; i++)
        if (fbs[i] && XftCharExists(dpy, fbs[i], u)) return icon_utf8;
    return ascii;
}
static int runs_w(XftFont *alt, const char *s) {
    const unsigned char *p = (const unsigned char *)s;
    size_t left = strlen(s);
    int xoff = 0;
    if (!f_main || !*s) return 0;
    while (left > 0) {
        FcChar32 u;
        int cl;
        XftFont *f;
        const unsigned char *q;
        size_t qleft;
        XGlyphInfo e;
        u = u8dec(p, left, &cl);
        f = alt ? alt : glyph_font(u);
        if (alt && !XftCharExists(dpy, alt, u)) f = glyph_font(u);
        q = p + cl; qleft = left - (size_t)cl;
        while (qleft > 0) {
            FcChar32 u2;
            int cl2;
            XftFont *f2;
            u2 = u8dec(q, qleft, &cl2);
            f2 = alt ? alt : glyph_font(u2);
            if (alt && !XftCharExists(dpy, alt, u2)) f2 = glyph_font(u2);
            if (f2 != f) break;
            q += cl2; qleft -= (size_t)cl2;
        }
        XftTextExtentsUtf8(dpy, f, (const FcChar8 *)p, (int)(q - p), &e);
        xoff += e.xOff;
        left = qleft; p = q;
    }
    return xoff;
}
static void runs_draw(XftFont *alt, XftColor *c, int x, int y, const char *s) {
    const unsigned char *p = (const unsigned char *)s;
    size_t left = strlen(s);
    int xoff = 0;
    if (!f_main || !xd || !*s) return;
    while (left > 0) {
        FcChar32 u;
        int cl;
        XftFont *f;
        const unsigned char *q;
        size_t qleft;
        XGlyphInfo e;
        u = u8dec(p, left, &cl);
        f = alt ? alt : glyph_font(u);
        if (alt && !XftCharExists(dpy, alt, u)) f = glyph_font(u);
        q = p + cl; qleft = left - (size_t)cl;
        while (qleft > 0) {
            FcChar32 u2;
            int cl2;
            XftFont *f2;
            u2 = u8dec(q, qleft, &cl2);
            f2 = alt ? alt : glyph_font(u2);
            if (alt && !XftCharExists(dpy, alt, u2)) f2 = glyph_font(u2);
            if (f2 != f) break;
            q += cl2; qleft -= (size_t)cl2;
        }
        XftDrawStringUtf8(xd, c, f, x + xoff, y, (const FcChar8 *)p, (int)(q - p));
        XftTextExtentsUtf8(dpy, f, (const FcChar8 *)p, (int)(q - p), &e);
        xoff += e.xOff;
        left = qleft; p = q;
    }
}
static void utf8_pop(char *s) {
    size_t n = strlen(s);
    size_t j;
    if (!n) return;
    j = n - 1;
    while (j > 0 && (s[j] & 0xC0) == 0x80) j--;
    s[j] = 0;
}
/* truncate a label to cell_w: trailing "...", never split a UTF-8 char */
static void ellipsize(const char *src, int maxw, char *out, size_t n) {
    if (runs_w(NULL, src) <= maxw) { snprintf(out, n, "%s", src); return; }
    {
        char tmp[160];
        snprintf(tmp, sizeof(tmp), "%.150s", src);
        while (tmp[0] && runs_w(NULL, tmp) > maxw - runs_w(NULL, "...")) utf8_pop(tmp);
        snprintf(out, n, "%s...", tmp);
    }
}
static void xft_alloc(unsigned long hex, XftColor *c) {
    XRenderColor rc = {
        (unsigned short)(((hex >> 16) & 0xff) * 257),
        (unsigned short)(((hex >> 8) & 0xff) * 257),
        (unsigned short)((hex & 0xff) * 257),
        0xffff
    };
    if (!XftColorAllocValue(dpy, DefaultVisual(dpy, screen),
            DefaultColormap(dpy, screen), &rc, c)) {
        c->pixel = WhitePixel(dpy, screen);
        c->color = rc;
    }
}

/* first letter as the fallback icon ("Firefox" -> "F") */
static void first_letter(const char *name, char *out, size_t n) {
    while (*name && isspace((unsigned char)*name)) name++;
    if (!*name) { snprintf(out, n, "?"); return; }
    {
        int cl;
        FcChar32 u = u8dec((const unsigned char *)name, strlen(name), &cl);
        if (u >= 'a' && u <= 'z') u -= 32;
        if (cl >= (int)n) cl = (int)n - 1;
        memcpy(out, name, (size_t)cl);
        /* uppercase ASCII byte dau neu la chu thuong */
        if (cl == 1 && out[0] >= 'a' && out[0] <= 'z') out[0] -= 32;
        out[cl] = 0;
    }
}

/* ============ polish: soft palette + rounded shapes ============ */
/* mix a toward b by pct_b percent; both 0xRRGGBB */
static unsigned long mix_hex(unsigned long a, unsigned long b, int pct_b) {
    int pct_a = 100 - pct_b;
    return (((((a >> 16) & 0xff) * pct_a + ((b >> 16) & 0xff) * pct_b) / 100) << 16)
         | (((((a >> 8) & 0xff) * pct_a + ((b >> 8) & 0xff) * pct_b) / 100) << 8)
         | (((a & 0xff) * pct_a + (b & 0xff) * pct_b) / 100);
}
/* rounded rectangle fill on the window (4 arcs + 2 strips, no XRender needed) */
static void fill_rrect(unsigned long pixel, int x, int y, int w, int h, int r) {
    XSetForeground(dpy, gc, pixel);
    if (w < 3 || h < 3 || r < 1) {
        XFillRectangle(dpy, win, gc, (unsigned)x, (unsigned)y, (unsigned)w, (unsigned)h);
        return;
    }
    if (r > h / 2) r = h / 2;
    if (r > w / 2) r = w / 2;
    XFillRectangle(dpy, win, gc, (unsigned)(x + r), (unsigned)y, (unsigned)(w - 2 * r), (unsigned)h);
    XFillRectangle(dpy, win, gc, (unsigned)x, (unsigned)(y + r), (unsigned)w, (unsigned)(h - 2 * r));
    XFillArc(dpy, win, gc, x, y, 2 * r, 2 * r, 90 * 64, 90 * 64);
    XFillArc(dpy, win, gc, x + w - 2 * r, y, 2 * r, 2 * r, 0, 90 * 64);
    XFillArc(dpy, win, gc, x + w - 2 * r, y + h - 2 * r, 2 * r, 2 * r, 270 * 64, 90 * 64);
    XFillArc(dpy, win, gc, x, y + h - 2 * r, 2 * r, 2 * r, 180 * 64, 90 * 64);
}
/* selection backdrop: rounded when r > 0, plain 1px-outline rect when r == 0 */
static void pill(int x, int y, int w, int h, int r, unsigned long fill, unsigned long ring) {
    if (r >= 1 && w >= 4 && h >= 4) {
        fill_rrect(ring, x, y, w, h, r);
        fill_rrect(fill, x + 1, y + 1, w - 2, h - 2, r - 1);
        return;
    }
    if (w < 3 || h < 3) { fill_rrect(fill, x, y, w, h, 0); return; }
    XSetForeground(dpy, gc, ring);
    XFillRectangle(dpy, win, gc, (unsigned)x, (unsigned)y, (unsigned)w, 1);
    XFillRectangle(dpy, win, gc, (unsigned)x, (unsigned)(y + h - 1), (unsigned)w, 1);
    XFillRectangle(dpy, win, gc, (unsigned)x, (unsigned)(y + 1), 1, (unsigned)(h - 2));
    XFillRectangle(dpy, win, gc, (unsigned)(x + w - 1), (unsigned)(y + 1), 1, (unsigned)(h - 2));
    XSetForeground(dpy, gc, fill);
    XFillRectangle(dpy, win, gc, (unsigned)(x + 1), (unsigned)(y + 1), (unsigned)(w - 2), (unsigned)(h - 2));
}
/* .desktop Icon= value -> lowercase basename without extension ("firefox.png"
 * or "/usr/share/icons/x/firefox.png" -> "firefox"). Empty -> returns 0. */
static int icon_key(const char *icon, char *out, size_t n) {
    const char *p = icon;
    size_t i = 0;
    if (!p || !*p) { out[0] = 0; return 0; }
    {
        const char *sl = strrchr(p, '/');
        if (sl) p = sl + 1;
    }
    while (p[i] && p[i] != '.' && i < n - 1) {
        out[i] = (char)tolower((unsigned char)p[i]);
        i++;
    }
    out[i] = 0;
    return i > 0;
}
/* Icon name -> Nerd glyph; exact match, or substring match for keys of 4+
 * chars. NULL when unmatched (caller then draws the first letter). All glyph
 * codepoints verified present in SpaceMono/JetBrainsMono Nerd Fonts. */
static const char *icon_glyph(const char *icon) {
    static const struct { const char *k; const char *g; } m[] = {
        /* browsers */
        { "firefox",  "\xef\x89\xa9" }, { "chrome", "\xef\x89\xa8" },
        { "brave",    "\xef\x89\xa8" }, { "edge",   "\xef\x89\xa8" },
        { "opera",    "\xef\x89\xa8" }, { "vivaldi","\xef\x89\xa8" },
        { "browser",  "\xef\x82\xac" }, { "web",    "\xef\x82\xac" },
        /* terminals */
        { "alacritty","\xef\x84\xa0" }, { "terminal","\xef\x84\xa0" },
        { "konsole",  "\xef\x84\xa0" }, { "kitty",  "\xef\x84\xa0" },
        { "wezterm",  "\xef\x84\xa0" }, { "console","\xef\x84\xa0" },
        { "xterm",    "\xef\x84\xa0" },
        /* dev */
        { "code",     "\xef\x84\xa1" }, { "visual-studio","\xef\x84\xa1" },
        { "codium",   "\xef\x84\xa1" }, { "idea",   "\xef\x84\xa1" },
        { "pycharm",  "\xef\x84\xa1" }, { "sublime","\xef\x84\xa1" },
        { "git",      "\xef\x82\x9b" }, { "github", "\xef\x82\x9b" },
        { "postgres", "\xef\x86\xb9" }, { "mysql",  "\xef\x86\xb9" },
        { "sqlite",   "\xef\x86\xb9" }, { "database","\xef\x86\xb9" },
        /* chat */
        { "slack",    "\xef\x86\x98" }, { "discord","\xef\x8a\x92" },
        { "irc",      "\xef\x8a\x92" }, { "telegram","\xef\x8b\x86" },
        { "whatsapp", "\xef\x88\xb2" }, { "chat",   "\xef\x81\xb5" },
        { "message",  "\xef\x81\xb5" }, { "comments","\xef\x8b\x9b" },
        /* mail / clock */
        { "thunderbird","\xef\x83\xa0" }, { "mail",  "\xef\x83\xa0" },
        { "evolution","\xef\x83\xa0" }, { "envelope","\xef\x83\xa0" },
        { "calendar","\xef\x84\xb3" }, { "clock",  "\xef\x80\x97" },
        /* files / docs */
        { "files",    "\xef\x81\xbb" }, { "nautilus","\xef\x81\xbb" },
        { "thunar",   "\xef\x81\xbb" }, { "nemo",   "\xef\x81\xbb" },
        { "folder",   "\xef\x81\xbb" }, { "directory","\xef\x81\xbb" },
        { "file",     "\xef\x85\x9b" }, { "document","\xef\x85\x9b" },
        { "text",     "\xef\x85\x9b" }, { "office", "\xef\x85\x9c" },
        { "libreoffice","\xef\x85\x9c" }, { "writer","\xef\x85\x9c" },
        { "pdf",      "\xef\x87\x81" }, { "reader", "\xef\x87\x81" },
        { "evince",   "\xef\x87\x81" },
        /* media */
        { "vlc",      "\xef\x80\xbd" }, { "mpv",    "\xef\x80\xbd" },
        { "video",    "\xef\x80\xbd" }, { "obs",    "\xef\x80\xbd" },
        { "handbrake","\xef\x80\xbd" }, { "music",  "\xef\x80\x81" },
        { "rhythmbox","\xef\x80\x81" }, { "audio",  "\xef\x80\x81" },
        { "spotify",  "\xef\x86\xbc" }, { "steam",  "\xef\x86\xb6" },
        { "game",     "\xef\x84\x9b" },
        /* graphics */
        { "gimp",     "\xef\x80\xbe" }, { "image",  "\xef\x80\xbe" },
        { "photo",    "\xef\x80\xbe" }, { "shotwell","\xef\x80\xbe" },
        { "krita",    "\xef\x80\xbe" }, { "inkscape","\xef\x80\xbe" },
        { "blender",  "\xef\x80\xbe" }, { "darktable","\xef\x80\xbe" },
        /* system */
        { "settings", "\xef\x80\x93" }, { "preferences","\xef\x80\x93" },
        { "control",  "\xef\x80\x93" }, { "monitor", "\xef\x82\x80" },
        { "htop",     "\xef\x82\x80" }, { "system", "\xef\x82\x80" },
        { "calculator","\xef\x87\xac" }, { "camera", "\xef\x80\xb0" },
        { "screenshot","\xef\x80\xb0" }, { "android","\xef\x85\xbb" },
        { "map",      "\xef\x94\x9b" }, { "coffee", "\xef\x83\xb4" },
        { "battery",  "\xef\x86\x86" }, { "bolt",   "\xef\x83\xa7" },
        { "ping",     "\xef\x8b\x8d" }, { "network","\xef\x8b\x8d" },
    };
    char key[64];
    if (!icon_key(icon, key, sizeof(key))) return NULL;
    for (unsigned i = 0; i < sizeof(m) / sizeof(m[0]); i++) {
        size_t kl = strlen(m[i].k);
        if (!strcmp(m[i].k, key) || (kl >= 4 && strstr(key, m[i].k)))
            return m[i].g;
    }
    return NULL;
}

#ifdef HAVE_CAIRO
/* ---- real .desktop app icons (PNG from the FDO icon theme), rofi-style.
 * Cairo decodes + scales + keeps alpha; the icon is alpha-blended onto the
 * row background, so selected pills don't show a square box. Missing files
 * fall back to the Nerd glyph table / first letter above. ---- */
#include <cairo/cairo.h>
#define ICONPIX 26
#define ICONPATH 300   /* max icon file path (ipath buffer size) */
#define ICONCACHE 48
typedef struct { char key[ICONPATH]; cairo_surface_t *surf; } IconEnt;
static IconEnt icache[ICONCACHE];
static int icache_n;
/* path cache: Icon= name -> resolved file ("" = negative). find_icon_file()
 * probes dozens of dirs/sizes per row, so it must not run every frame. */
#define PATHCACHE 128
typedef struct { char key[128]; char path[ICONPATH]; } PathEnt;
static PathEnt pcache[PATHCACHE];
static int pcache_n;
static int ico_rshift, ico_gshift, ico_bshift;
static int ico_rbits, ico_gbits, ico_bbits;

static void load_gtk_icon_theme(void) {
    char path[1024], line[512];
    const char *xdg = getenv("XDG_CONFIG_HOME"), *home = getenv("HOME");
    FILE *f;
    if (icon_theme[0]) return;   /* run.config won */
    if (xdg && *xdg) snprintf(path, sizeof(path), "%s/gtk-3.0/settings.ini", xdg);
    else if (home && *home) snprintf(path, sizeof(path), "%s/.config/gtk-3.0/settings.ini", home);
    else return;
    f = fopen(path, "r");
    if (!f) return;
    while (fgets(line, sizeof(line), f)) {
        char *s = trim(line), *eq;
        if (!*s || *s == '#') continue;
        eq = strchr(s, '=');
        if (!eq) continue;
        *eq = 0;
        if (!strcmp(trim(s), "gtk-icon-theme-name")) {
            snprintf(icon_theme, sizeof(icon_theme), "%.63s", trim(eq + 1));
            break;
        }
    }
    fclose(f);
}
static int file_is_png(const char *path) {
    unsigned char sig[4];
    FILE *f = fopen(path, "rb");
    size_t r;
    if (!f) return 0;
    r = fread(sig, 1, 4, f);
    fclose(f);
    return r == 4 && sig[0] == 0x89 && sig[1] == 'P' && sig[2] == 'N' && sig[3] == 'G';
}
/* FDO-lite: theme dirs (user first) x [icon_theme, hicolor] x sizes, then
 * any category under 48x48, then flat /usr/share/pixmaps. */
static const char *find_icon_file(const char *icon, char *out, size_t n) {
    char key[64], udir[1024];
    const char *dirs[2];
    const char *themes[8];
    static const int sizes[] = { 48, 32, 64, 24, 22, 16, 128, 256, 96 };
    const char *xdg = getenv("XDG_DATA_HOME"), *home = getenv("HOME");
    int nd = 0, nt = 0;
    out[0] = 0;
    if (!icon || !*icon) return NULL;
    if (icon[0] == '/') { snprintf(out, n, "%.200s", icon); return out; }
    if (!icon_key(icon, key, sizeof(key))) return NULL;
    if (xdg && *xdg) snprintf(udir, sizeof(udir), "%s/icons", xdg);
    else if (home && *home) snprintf(udir, sizeof(udir), "%s/.local/share/icons", home);
    else udir[0] = 0;
    if (udir[0]) dirs[nd++] = udir;
    dirs[nd++] = "/usr/share/icons";
    if (icon_theme[0] && strcmp(icon_theme, "hicolor")) themes[nt++] = icon_theme;
    themes[nt++] = "hicolor";
    for (int d = 0; d < nd; d++)
        for (int t = 0; t < nt; t++) {
            for (unsigned s = 0; s < sizeof(sizes) / sizeof(sizes[0]); s++) {
                snprintf(out, n, "%s/%s/%dx%d/apps/%s.png",
                         dirs[d], themes[t], sizes[s], sizes[s], key);
                if (file_is_png(out)) return out;
            }
            /* any category under 48x48 (misc/utility/... themes) */
            snprintf(out, n, "%s/%s/48x48", dirs[d], themes[t]);
            {
                DIR *dp = opendir(out);
                if (dp) {
                    struct dirent *de;
                    while ((de = readdir(dp))) {
                        if (de->d_name[0] == '.' || strlen(de->d_name) > 120) continue;
                        {
                            /* build in a scratch sized for the worst case, then
                             * copy bounded (gcc can't bound %s args here) */
                            char tmp[2048];
                            int tn = snprintf(tmp, sizeof(tmp), "%s/%s/48x48/%s/%s.png",
                                              dirs[d], themes[t], de->d_name, key);
                            if (tn <= 0) continue;
                            if (tn >= (int)n) tn = (int)n - 1;
                            memcpy(out, tmp, (size_t)tn);
                            out[tn] = 0;
                        }
                        if (file_is_png(out)) { closedir(dp); return out; }
                    }
                    closedir(dp);
                }
            }
        }
    snprintf(out, n, "/usr/share/pixmaps/%s.png", key);
    if (file_is_png(out)) return out;
    return NULL;
}
/* cached find_icon_file(): keyed by the Icon= value; "" means negative hit. */
static const char *icon_path_cached(const char *icon, char *out, size_t n) {
    char key[128];
    int i;
    if (!icon || !*icon) return NULL;
    snprintf(key, sizeof(key), "%.127s", icon);
    for (i = 0; i < pcache_n; i++)
        if (!strcmp(pcache[i].key, key)) break;
    if (i == pcache_n) {
        PathEnt e;
        char tmp[256];
        snprintf(e.key, sizeof(e.key), "%s", key);
        if (find_icon_file(icon, tmp, sizeof(tmp)))
            snprintf(e.path, sizeof(e.path), "%.255s", tmp);
        else
            e.path[0] = 0;
        if ((size_t)pcache_n >= PATHCACHE) {
            memmove(&pcache[1], &pcache[0], (PATHCACHE - 1) * sizeof(PathEnt));
            pcache[0] = e;
            i = 0;
        } else {
            pcache[pcache_n++] = e;
            i = pcache_n - 1;
        }
    }
    if (!pcache[i].path[0]) return NULL;
    snprintf(out, n, "%.299s", pcache[i].path);
    return out;
}
/* LRU cache of scaled ARGB32 surfaces (NULL entries = negative cache) */
static cairo_surface_t *icon_surface(const char *path) {
    for (int i = 0; i < icache_n; i++) {
        if (!strcmp(icache[i].key, path)) {
            IconEnt t = icache[i];
            memmove(&icache[1], &icache[0], (size_t)i * sizeof(t));
            icache[0] = t;
            return icache[0].surf;
        }
    }
    {
        cairo_surface_t *im = cairo_image_surface_create_from_png(path);
        cairo_surface_t *cs = NULL;
        if (im && cairo_surface_status(im) == CAIRO_STATUS_SUCCESS) {
            int w = cairo_image_surface_get_width(im);
            int h = cairo_image_surface_get_height(im);
            if (w > 0 && h > 0) {
                cs = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, ICONPIX, ICONPIX);
                if (cs && cairo_surface_status(cs) == CAIRO_STATUS_SUCCESS) {
                    cairo_t *cr = cairo_create(cs);
                    cairo_scale(cr, (double)ICONPIX / w, (double)ICONPIX / h);
                    cairo_set_source_surface(cr, im, 0, 0);
                    cairo_paint(cr);
                    cairo_destroy(cr);
                } else {
                    if (cs) cairo_surface_destroy(cs);
                    cs = NULL;
                }
            }
        }
        if (im) cairo_surface_destroy(im);
        if (icache_n < ICONCACHE) {
            snprintf(icache[icache_n].key, sizeof(icache[icache_n].key), "%.299s", path);
            icache[icache_n].surf = cs;
            icache_n++;
        } else {
            if (icache[ICONCACHE - 1].surf)
                cairo_surface_destroy(icache[ICONCACHE - 1].surf);
            memmove(&icache[1], &icache[0], (ICONCACHE - 1) * sizeof(IconEnt));
            snprintf(icache[0].key, sizeof(icache[0].key), "%.299s", path);
            icache[0].surf = cs;
        }
        return cs;
    }
}
static void mask_bits(unsigned long m, int *shift, int *bits) {
    int s = 0, b = 0;
    if (!m) { *shift = 0; *bits = 0; return; }
    while (!(m & 1)) { m >>= 1; s++; }
    while (m & 1) { m >>= 1; b++; }
    *shift = s; *bits = b;
}
/* premultiplied cairo ARGB32 (bytes B,G,R,A) blended onto row bg -> pixel */
static unsigned long blend_pixel(const unsigned char *s, unsigned long bg) {
    int a = s[3];
    int r = s[2] + ((int)((bg >> 16) & 0xff) * (255 - a) + 127) / 255;
    int g = s[1] + ((int)((bg >> 8) & 0xff) * (255 - a) + 127) / 255;
    int b = s[0] + ((int)(bg & 0xff) * (255 - a) + 127) / 255;
    unsigned long p = 0;
    if (r > 255) r = 255;
    if (g > 255) g = 255;
    if (b > 255) b = 255;
    if (ico_rbits) {
        unsigned long rv = ico_rbits >= 8 ? (unsigned)(r << (ico_rbits - 8))
                                          : (unsigned)(r >> (8 - ico_rbits));
        p |= (rv & ((1UL << ico_rbits) - 1)) << ico_rshift;
    }
    if (ico_gbits) {
        unsigned long gv = ico_gbits >= 8 ? (unsigned)(g << (ico_gbits - 8))
                                          : (unsigned)(g >> (8 - ico_gbits));
        p |= (gv & ((1UL << ico_gbits) - 1)) << ico_gshift;
    }
    if (ico_bbits) {
        unsigned long bv = ico_bbits >= 8 ? (unsigned)(b << (ico_bbits - 8))
                                          : (unsigned)(b >> (8 - ico_bbits));
        p |= (bv & ((1UL << ico_bbits) - 1)) << ico_bshift;
    }
    return p;
}
static void draw_icon_img(cairo_surface_t *sf, int x, int y, unsigned long bg) {
    unsigned char *d = cairo_image_surface_get_data(sf);
    int stride = cairo_image_surface_get_stride(sf);
    XImage *img = XCreateImage(dpy, DefaultVisual(dpy, screen), DefaultDepth(dpy, screen),
                               ZPixmap, 0, NULL, ICONPIX, ICONPIX, 32, 0);
    if (!img) return;
    img->byte_order = ImageByteOrder(dpy);
    img->bitmap_bit_order = ImageByteOrder(dpy);
    XInitImage(img);
    img->data = malloc((size_t)img->bytes_per_line * (unsigned)img->height);
    if (!img->data) { XDestroyImage(img); return; }
    {
        unsigned int *pp = (unsigned int *)img->data;
        for (int yy = 0; yy < ICONPIX; yy++) {
            const unsigned char *row = d + (size_t)yy * (size_t)stride;
            for (int xx = 0; xx < ICONPIX; xx++)
                pp[(size_t)yy * ICONPIX + (size_t)xx] = (unsigned int)blend_pixel(row + xx * 4, bg);
        }
    }
    XPutImage(dpy, win, gc, img, 0, 0, x, y, ICONPIX, ICONPIX);
    XDestroyImage(img);
}
#endif /* HAVE_CAIRO */
/* byte offsets of the query subsequence inside name (UTF-8 start bytes only,
 * so matchers never split a multibyte char while drawing) */
static void match_pos(const char *name, const char *q, int *pos, int npos) {
    size_t ni = 0;
    for (int qi = 0; qi < npos; qi++) {
        char qc = (char)tolower((unsigned char)q[qi]);
        while (name[ni] && (char)tolower((unsigned char)name[ni]) != qc) ni++;
        if (!name[ni] || ((unsigned char)name[ni] & 0xC0) == 0x80) {
            for (int j = qi; j < npos; j++) pos[j] = -1;
            return;
        }
        pos[qi] = (int)ni++;
    }
}
/* draw a name with the fuzzy-matched bytes in hl (base otherwise) */
static void draw_match_text(XftColor *base, XftColor *hl, int x, int y,
                            const char *name, const int *pos, int npos) {
    char tmp[240];
    size_t len = strlen(name);
    int s = 0, pi = 0;
    if (!name[0]) return;
    if (len >= sizeof(tmp)) { runs_draw(NULL, base, x, y, name); return; }
    while (s < (int)len) {
        int hit = (pi < npos && pos[pi] == s);
        int e;
        if (hit) pi++;
        e = s + 1;
        while (e < (int)len && !(pi < npos && pos[pi] == e)) e++;
        memcpy(tmp, name + s, (size_t)(e - s));
        tmp[e - s] = 0;
        runs_draw(NULL, hit ? hl : base, x, y, tmp);
        x += runs_w(NULL, tmp);
        s = e;
    }
}
/* case-insensitive substring (strcasestr is GNU-only; avoid the dep) */
static int has_ci(const char *hay, const char *needle) {
    size_t n = strlen(needle);
    if (!n) return 0;
    for (const char *p = hay; *p; p++)
        if (!strncasecmp(p, needle, n)) return 1;
    return 0;
}
/* power grid: give each action its own Rosé Pine tint */
static XftColor *power_color(const char *name) {
    if (has_ci(name, "suspend") || has_ci(name, "lock")) return &c_foam;
    if (has_ci(name, "logout")) return &c_gold;
    if (has_ci(name, "reboot") || has_ci(name, "restart")) return &c_pine;
    if (has_ci(name, "off") || has_ci(name, "shutdown")) return &c_love;
    return &c_acc;
}
/* Nerd glyph prompt per mode, ASCII fallback when the font lacks the glyph */
static const char *mode_glyph(void) {
    static const char *g[4] = {
        "\xef\x80\x82",           /* U+F002 search  */
        "\xef\x80\x85",           /* U+F005 star    */
        "\xef\x80\x91",           /* U+F011 power   */
        "\xef\x87\xac",           /* U+F1EC calc    */
    };
    static const char *a[4] = { ">", "*", "\xc2\xbb", "\xe2\x88\x91" };
    return pick_icon(g[mode], a[mode]);
}
static const char *placeholder(void) {
    switch (mode) {
    case MODE_DRUN: return "type to filter apps\xe2\x80\xa6";
    case MODE_FAV:  return "type to filter favorites\xe2\x80\xa6";
    case MODE_POWER: return "what do you want to do\xe2\x80\xa6";
    case MODE_CALC: return "e.g. 2*(3+4)^2 + sqrt(16)";
    }
    return "";
}
static const char *help_hint(void) {
    if (mode == MODE_CALC) return "\xe2\x86\xb5 copy \xc2\xb7 esc quit";
    return "\xe2\x86\x91\xe2\x86\x93 move \xc2\xb7 \xe2\x86\xb5 run \xc2\xb7 esc quit";
}

/* ============ draw ============ */
static void draw(void) {
    unsigned n;
    Entry *vec = cur_vec(&n);
    int base;
    char cnt[32];
    (void)n;
    if (!xd) return;
    XSetForeground(dpy, gc, c_bg.pixel);
    XFillRectangle(dpy, win, gc, 0, 0, (unsigned)WW, (unsigned)HH);

    /* ---- input band ---- */
    XSetForeground(dpy, gc, c_band.pixel);
    XFillRectangle(dpy, win, gc, 0, 0, (unsigned)WW, (unsigned)input_h);
    base = input_h / 2 + (f_main ? (f_main->ascent - f_main->descent) / 2 : 5);
    {
        const char *gly = mode_glyph();
        int tx = 16;
        runs_draw(NULL, &c_acc, tx, base, gly);
        tx += runs_w(NULL, gly) + 12;
        runs_draw(NULL, &c_fg, tx, base, query);
        if (qlen == 0) runs_draw(NULL, &c_dim, tx + runs_w(NULL, query) + 10, base, placeholder());
        fill_rrect(c_acc.pixel, tx + runs_w(NULL, query) + 6, input_h / 2 - 10, 2, 20, 0);
    }
    {
        static const char *badge[4] = { "drun", "fav", "power", "calc" };
        const char *b = badge[mode];
        runs_draw(NULL, &c_acc, WW - 16 - runs_w(NULL, b), base, b);
    }
    XSetForeground(dpy, gc, c_div.pixel);
    XFillRectangle(dpy, win, gc, 0, input_h - 1, (unsigned)WW, 1);

    if (mode == MODE_DRUN) {
        int icol = opt_drun_icons ? 40 : 0;   /* icon column width */
        for (int r = 0; r < list_rows; r++) {
            int idx = scroll + r;
            int y = input_h + opt_pad_t + r * row_h;
            int ty = y + row_h / 2 + (f_main ? (f_main->ascent - f_main->descent) / 2 : 5);
            char label[180];
            int pos[MAXQ];
            int npos = 0;
            if (idx >= nfilt) break;
            {
                char fl[16];
                const char *ic;
                int iw;
#ifdef HAVE_CAIRO
                char ipath[300];
                cairo_surface_t *isf = NULL;
                if (icol > 0 && icon_path_cached(vec[filt[idx]].icon, ipath, sizeof(ipath)))
                    isf = icon_surface(ipath);
#endif
                if (runs_w(NULL, vec[filt[idx]].name) > WW - 48 - icol) {
                    ellipsize(vec[filt[idx]].name, WW - 48 - icol, label, sizeof(label));
                } else {
                    snprintf(label, sizeof(label), "%s", vec[filt[idx]].name);
                }
                if (qlen > 0) { match_pos(label, query, pos, qlen); npos = qlen; }
                /* pill first: it is opaque and would erase anything under it */
                if (idx == sel)
                    pill(opt_margin + opt_pad_l, y + opt_row_gap / 2,
                         WW - 2 * opt_margin - opt_pad_l - opt_pad_r,
                         row_h - opt_row_gap, 0, c_sel.pixel, c_selout.pixel);
                if (icol > 0) {
#ifdef HAVE_CAIRO
                    if (isf) {
                        unsigned long bg = (idx == sel) ? c_sel.pixel : T_BG;
                        draw_icon_img(isf, 30 + (icol - ICONPIX) / 2, y + (row_h - ICONPIX) / 2, bg);
                    } else
#endif
                    {
                        first_letter(vec[filt[idx]].name, fl, sizeof(fl));
                        ic = pick_icon(icon_glyph(vec[filt[idx]].icon), fl);
                        iw = runs_w(f_main, ic);
                        runs_draw(f_main, idx == sel ? &c_fg : &c_acc, 30 + (icol - iw) / 2, ty, ic);
                    }
                }
                draw_match_text(&c_fg, &c_acc, 26 + icol, ty, label, npos ? pos : NULL, npos);
            }
        }
        if (!nfilt) {
            const char *msg = qlen ? "\xe2\x86\xb5 run as command" : "no apps found";
            runs_draw(NULL, &c_dim, 26,
                input_h + opt_pad_t + row_h / 2 + (f_main ? (f_main->ascent - f_main->descent) / 2 : 5), msg);
        }
    } else if (mode == MODE_CALC) {
        double v;
        char res[64];
        int cy = input_h + opt_pad_t + ((HH - input_h - footer_h - opt_pad_t - opt_pad_b) / 2);
        int ty = cy + (f_big ? (f_big->ascent - f_big->descent) / 2 : 5);
        if (qlen) runs_draw(NULL, &c_dim, 20, input_h + 22, query);
        if (calc_eval(query, &v)) {
            calc_str(v, res, sizeof(res));
            runs_draw(f_big, &c_acc, 20, ty, res);
        } else if (qlen) {
            runs_draw(NULL, &c_dim, 20, ty, "nope - check the expression");
        }
    } else {
        int per = opt_cols * opt_lines;
        int gw = opt_cols * cell_w + opt_cell_gap_x * (opt_cols - 1);
        int gx0 = opt_pad_l + (WW - opt_pad_l - opt_pad_r - gw) / 2;
        int gy0 = input_h + opt_pad_t;
        if (per < 1) per = 1;
        for (int k = 0; k < per; k++) {
            int idx = scroll + k;
            int cx = gx0 + (k % opt_cols) * (cell_w + opt_cell_gap_x);
            int cy = gy0 + (k / opt_cols) * (cell_h + opt_cell_gap_y);
            char fl[16], label[96];
            const char *ic;
            int iw, ty;
            if (idx >= nfilt) break;
            if (idx == sel) {
                pill(cx + 6, cy + 2, cell_w - 12, cell_h - 4, 0,
                    c_sel.pixel, c_selout.pixel);
            }
            first_letter(vec[filt[idx]].name, fl, sizeof(fl));
            ic = pick_icon(vec[filt[idx]].icon, fl);
            iw = runs_w(f_big, ic);
            if (idx == sel) {
                runs_draw(f_big, &c_fg, cx + (cell_w - iw) / 2, cy + 48, ic);
            } else {
                XftColor *cc = (mode == MODE_POWER) ? power_color(vec[filt[idx]].name) : &c_acc;
                runs_draw(f_big, cc, cx + (cell_w - iw) / 2, cy + 48, ic);
            }
            ellipsize(vec[filt[idx]].name, cell_w - 24, label, sizeof(label));
            iw = runs_w(NULL, label);
            ty = cy + cell_h - 28;
            runs_draw(NULL, idx == sel ? &c_fg : &c_dim, cx + (cell_w - iw) / 2, ty, label);
            (void)fl;
        }
        if (!nfilt) {
            runs_draw(NULL, &c_dim, 26,
                input_h + ((HH - input_h - footer_h) / 2) +
                (f_main ? (f_main->ascent - f_main->descent) / 2 : 5),
                "no match - clear the filter to see all");
        }
    }

    /* footer band */
    {
        int fy = HH - footer_h / 2 + (f_main ? (f_main->ascent - f_main->descent) / 2 : 5);
        int vis = (mode == MODE_DRUN) ? list_rows : opt_cols * opt_lines;
        XSetForeground(dpy, gc, c_div.pixel);
        XFillRectangle(dpy, win, gc, 0, HH - footer_h, (unsigned)WW, 1);
        runs_draw(NULL, &c_dim, 16, fy, help_hint());
        if (mode == MODE_CALC) {
            snprintf(cnt, sizeof(cnt), "%s", "math");
        } else {
            snprintf(cnt, sizeof(cnt), "%d/%d", nfilt ? sel + 1 : 0, nfilt);
            if (nfilt > 0 && (scroll + vis < nfilt || scroll > 0)) {
                const char *ch = scroll + vis < nfilt ? "\xe2\x96\xbe" : "\xe2\x96\xb4";
                runs_draw(NULL, &c_dim, WW - 16 - runs_w(NULL, cnt) - 20, fy, ch);
            }
        }
        runs_draw(NULL, &c_dim, WW - 16 - runs_w(NULL, cnt), fy, cnt);
    }
    XFlush(dpy);
}

/* ============ launch: wordexp + fork+exec, like menucmd ============ */
static void launch(const char *cmd, const char *name) {
    wordexp_t w;
    if (!cmd || !*cmd) return;
    if (wordexp(cmd, &w, WRDE_NOCMD) != 0 || w.we_wordc == 0) {
        wordfree(&w);
        return;
    }
    hist_save(name);
    {
        pid_t pid = fork();
        if (pid == -1) { wordfree(&w); return; }
        if (pid == 0) {
            if (dpy) close(ConnectionNumber(dpy));
            setsid();
            execvp(w.we_wordv[0], (char *const *)w.we_wordv);
            fprintf(stderr, "dani-run: exec %s failed: %s\n", w.we_wordv[0], strerror(errno));
            _exit(1);
        }
    }
    wordfree(&w);
}

/* ============ moves ============ */
static void move_sel(int d) {
    if (!nfilt) return;
    sel += d;
    if (sel < 0) sel = 0;
    if (sel >= nfilt) sel = nfilt - 1;
    if (mode == MODE_DRUN) {
        if (sel < scroll) scroll = sel;
        if (sel >= scroll + list_rows) scroll = sel - list_rows + 1;
    } else {
        int per = opt_cols * opt_lines;
        if (per < 1) per = 1;
        if (sel < scroll) scroll = (sel / per) * per;
        if (sel >= scroll + per) scroll = (sel / per) * per;
    }
}
static void activate(void) {
    unsigned n;
    Entry *vec = cur_vec(&n);
    if (mode == MODE_CALC) {
        double v;
        char res[64];
        if (!calc_eval(query, &v)) { XCloseDisplay(dpy); exit(1); }
        calc_str(v, res, sizeof(res));
        to_clipboard(res);
        printf("%s\n", res);
        fflush(stdout);
        XCloseDisplay(dpy);
        exit(0);
    }
    if (nfilt > 0) {
        launch(vec[filt[sel]].exec, vec[filt[sel]].name);
    } else if (mode == MODE_DRUN && qlen > 0) {
        /* drun with no match: run the query as a direct command (dmenu_run style) */
        launch(query, query);
    } else return;
    XCloseDisplay(dpy);
    exit(0);
}

/* ============ main ============ */
static void usage(const char *a0) {
    printf("Usage: %s [--fav|--power|--calc|--list|--help]\n"
           "  (no flag)     drun: vertical fuzzy list from .desktop\n"
           "  --fav         grid of favorite apps (filterable)\n"
           "  --power       grid power menu\n"
           "  --calc        calculator: Enter = copy result + print to stdout\n"
           "  --list        dump Name<TAB>Exec, no X\n", a0);
}

int main(int argc, char **argv) {
    int do_list = 0;
    setlocale(LC_ALL, "");
    setlocale(LC_NUMERIC, "C"); /* calc: strtod/printf always use '.' */
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--fav")) mode = MODE_FAV;
        else if (!strcmp(argv[i], "--power")) mode = MODE_POWER;
        else if (!strcmp(argv[i], "--calc")) mode = MODE_CALC;
        else if (!strcmp(argv[i], "--list")) do_list = 1;
        else if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) {
            usage(argv[0]);
            return 0;
        } else {
            fprintf(stderr, "dani-run: unknown arg '%s'\n", argv[i]);
            usage(argv[0]);
            return 1;
        }
    }
    load_wm_theme();
    defaults();
    load_run_config();
#ifdef HAVE_CAIRO
    load_gtk_icon_theme();
#endif
    resolve_padding();
    if (!font_pat[0]) {
        if (T_FONT[0]) snprintf(font_pat, sizeof(font_pat), "%.255s", T_FONT);
        else snprintf(font_pat, sizeof(font_pat), "SpaceMono Nerd Font:size=11");
    }
    if (mode == MODE_DRUN) load_drun();
    hist_load();
    refilter();

    if (do_list) {
        unsigned n;
        Entry *vec = cur_vec(&n);
        for (int i = 0; i < nfilt; i++)
            printf("%s\t%s\n", vec[filt[i]].name, vec[filt[i]].exec);
        return 0;
    }

    dpy = XOpenDisplay(NULL);
    if (!dpy) { fprintf(stderr, "dani-run: cannot open display\n"); return 1; }
    screen = DefaultScreen(dpy);
    {
        int sw = DisplayWidth(dpy, screen), sh = DisplayHeight(dpy, screen);
        if (mode == MODE_DRUN || mode == MODE_CALC) {
            /* fit short screens: shrink the visible rows, minimum 3 */
            int maxrows = (sh - 80 - input_h - footer_h - opt_pad_t - opt_pad_b) / row_h;
            if (maxrows < 3) maxrows = 3;
            if (list_rows > maxrows) { list_rows = maxrows; refilter(); }
            WW = opt_width; HH = input_h + row_h * list_rows + footer_h + opt_pad_t + opt_pad_b;
        }
        else {
            WW = opt_cols * cell_w + opt_cell_gap_x * (opt_cols - 1) + opt_pad_l + opt_pad_r + 32;
            HH = input_h + opt_pad_t + opt_lines * (cell_h + opt_cell_gap_y) + opt_pad_b + footer_h;
        }
        if (WW > sw - 40) WW = sw - 40;
        if (HH > sh - 80) HH = sh - 80;
        {
            XSetWindowAttributes wa = { .override_redirect = True, .background_pixel = T_BG };
            win = XCreateWindow(dpy, RootWindow(dpy, screen),
                (sw - WW) / 2, (sh - HH) / 3, (unsigned)WW, (unsigned)HH, 0,
                CopyFromParent, InputOutput, CopyFromParent,
                CWOverrideRedirect | CWBackPixel, &wa);
        }
        {
            Atom ty = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE", False);
            Atom dg = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE_DIALOG", False);
            if (ty != None && dg != None) XChangeProperty(dpy, win, ty, XA_ATOM, 32, PropModeReplace, (unsigned char *)&dg, 1);
            /* Mark as ABOVE so compositor and WM know this stays on top */
            Atom state = XInternAtom(dpy, "_NET_WM_STATE", False);
            Atom above = XInternAtom(dpy, "_NET_WM_STATE_ABOVE", False);
            if (state != None && above != None)
                XChangeProperty(dpy, win, state, XA_ATOM, 32, PropModeReplace, (unsigned char *)&above, 1);
            XStoreName(dpy, win, mode == MODE_FAV ? "dani-run fav" : mode == MODE_POWER ? "dani-run power" : mode == MODE_CALC ? "dani-run calc" : "dani-run");
        }
        XSelectInput(dpy, win, ExposureMask | KeyPressMask | ButtonPressMask | PointerMotionMask);
        XMapWindow(dpy, win);
        xim = XOpenIM(dpy, NULL, NULL, NULL);
        if (xim) {
            xic = XCreateIC(xim, XNInputStyle,
                XIMPreeditNothing | XIMStatusNothing, XNClientWindow, win, NULL);
        }
        /* grab keyboard: let our WM pass (daniwm never grabs override windows) */
        for (int i = 0; i < 100; i++) {
            if (XGrabKeyboard(dpy, win, True, GrabModeAsync, GrabModeAsync, CurrentTime) == GrabSuccess) break;
            usleep(10000);
        }
    }
    /* fonts: main + Nerd fallbacks (like bar.c), text fallbacks are lazy */
    f_main = XftFontOpenName(dpy, screen, font_pat);
    if (!f_main) f_main = XftFontOpenName(dpy, screen, "monospace:size=11");
    if (!f_main) f_main = XftFontOpenName(dpy, screen, "fixed");
    if (!f_main) { fprintf(stderr, "dani-run: no font\n"); return 1; }
    {
        /* big icon = Nerd at ~26pt, derive the size from the main font when parseable */
        double sz = 11.0;
        const char *pp = strstr(font_pat, "size=");
        if (pp) {
            double v = strtod(pp + 5, NULL);
            if (v >= 6.0 && v <= 64.0) sz = v;
        }
        {
            char b1[96], b2[96];
            snprintf(b1, sizeof(b1), "JetBrainsMono Nerd Font Mono:size=%.1f", sz * 2.2);
            snprintf(b2, sizeof(b2), "Symbols Nerd Font Mono:size=%.1f", sz * 2.2);
            f_big = XftFontOpenName(dpy, screen, b1);
            if (!f_big) f_big = XftFontOpenName(dpy, screen, b2);
            if (!f_big) f_big = f_main;
        }
        {
            char n1[96], n2[96];
            snprintf(n1, sizeof(n1), "JetBrainsMono Nerd Font Mono:size=%.1f", sz);
            snprintf(n2, sizeof(n2), "Symbols Nerd Font Mono:size=%.1f", sz);
            {
                const char *cands[] = { n1, n2, NULL };
                for (int i = 0; cands[i] && nfb < 6; i++) {
                    XftFont *f = XftFontOpenName(dpy, screen, cands[i]);
                    if (f) fbs[nfb++] = f;
                }
            }
        }
    }
    gc = XCreateGC(dpy, win, 0, NULL);
    xd = XftDrawCreate(dpy, win, DefaultVisual(dpy, screen), DefaultColormap(dpy, screen));
#ifdef HAVE_CAIRO
    {
        Visual *v = DefaultVisual(dpy, screen);
        mask_bits(v->red_mask, &ico_rshift, &ico_rbits);
        mask_bits(v->green_mask, &ico_gshift, &ico_gbits);
        mask_bits(v->blue_mask, &ico_bshift, &ico_bbits);
    }
#endif
    xft_alloc(T_BG, &c_bg);
    xft_alloc(T_FG, &c_fg);
    xft_alloc(T_ACC, &c_acc);
    xft_alloc(T_DIM, &c_dim);
    xft_alloc(mix_hex(T_BG, T_FG, 5), &c_band);
    xft_alloc(mix_hex(T_BG, T_FG, 8), &c_div);
    xft_alloc(mix_hex(T_BG, T_ACC, 22), &c_sel);   /* soft iris tint (mostly bg) */
    xft_alloc(mix_hex(T_ACC, T_BG, 55), &c_selout); /* 1px accent ring */
    xft_alloc(0xeb6f92, &c_love); /* Rosé Pine love  */
    xft_alloc(0xf6c177, &c_gold); /* Rosé Pine gold  */
    xft_alloc(0x31748f, &c_pine); /* Rosé Pine pine  */
    xft_alloc(0x9ccfd8, &c_foam); /* Rosé Pine foam  */
    XSetWindowBackground(dpy, win, c_bg.pixel);
    XClearWindow(dpy, win);

    for (;;) {
        XEvent ev;
        XNextEvent(dpy, &ev);
        /* Stay on top: daniwm may raise docks/floating/fullscreen via
         * XRaiseWindow, pushing our override_redirect window down.
         * Re-raise ourselves after every event to counter this. */
        XRaiseWindow(dpy, win);
        if (ev.type == Expose) {
            if (ev.xexpose.count == 0) draw();
        } else if (ev.type == KeyPress) {
            char buf[64];
            KeySym ks;
            Status st;
            int n = xic
                ? Xutf8LookupString(xic, &ev.xkey, buf, sizeof(buf) - 1, &ks, &st)
                : XLookupString(&ev.xkey, buf, sizeof(buf) - 1, &ks, NULL);
            int ctrl = (ev.xkey.state & ControlMask) != 0;
            if (n < 0) n = 0;
            buf[n] = 0;
            if (ks == XK_Escape) return 0;
            else if (ks == XK_Return || ks == XK_KP_Enter) activate();
            else if (ks == XK_BackSpace) {
                if (ctrl) { query[0] = 0; qlen = 0; }
                else utf8_pop(query);
                qlen = (int)strlen(query);
                sel = 0; refilter(); draw();
            } else if (ks == XK_Delete) {
                query[0] = 0; qlen = 0; sel = 0; refilter(); draw();
            } else if (ks == XK_Up || (ctrl && (ks == XK_k || ks == XK_K))) {
                if (mode == MODE_DRUN) move_sel(-1);
                else move_sel(-opt_cols);
                draw();
            } else if (ks == XK_Down || (ctrl && (ks == XK_j || ks == XK_N))) {
                if (mode == MODE_DRUN) move_sel(1);
                else move_sel(opt_cols);
                draw();
            } else if (ks == XK_Left || (ctrl && (ks == XK_h || ks == XK_H))) {
                move_sel(-1); draw();
            } else if (ks == XK_Right || (ctrl && (ks == XK_l || ks == XK_L))) {
                move_sel(1); draw();
            } else if (ks == XK_Tab) {
                move_sel(ev.xkey.state & ShiftMask ? -1 : 1); draw();
            } else if (ks == XK_Page_Up || ks == XK_Home) {
                if (mode == MODE_DRUN) move_sel(-list_rows);
                else move_sel(-opt_cols * opt_lines);
                draw();
            } else if (ks == XK_Page_Down || ks == XK_End) {
                if (mode == MODE_DRUN) move_sel(list_rows);
                else move_sel(opt_cols * opt_lines);
                draw();
            } else if (ctrl && (ks == XK_u || ks == XK_U)) {
                query[0] = 0; qlen = 0; sel = 0; refilter(); draw();
            } else if (ctrl && (ks == XK_w || ks == XK_W)) {
                /* delete one word: drop trailing spaces, then cut to the space */
                while (qlen > 0 && isspace((unsigned char)query[qlen - 1])) query[--qlen] = 0;
                while (qlen > 0 && !isspace((unsigned char)query[qlen - 1])) {
                    utf8_pop(query);
                    qlen = (int)strlen(query);
                }
                sel = 0; refilter(); draw();
            } else if (ctrl && (ks == XK_a || ks == XK_A)) {
                /* home: no mid-text cursor -> no-op, keep selection at top */
                sel = 0;
                if (mode == MODE_DRUN) scroll = 0;
                else scroll = 0;
                draw();
            } else if (n > 0) {
                /* hjkl navigates while the query is empty (vim-style), no Ctrl;
                 * calc: always type (function names like sqrt need letters) */
                if (mode != MODE_CALC && qlen == 0 && !ctrl && n == 1 &&
                    (buf[0] == 'j' || buf[0] == 'k' || buf[0] == 'h' || buf[0] == 'l')) {
                    if (mode == MODE_DRUN) {
                        if (buf[0] == 'j') move_sel(1);
                        else if (buf[0] == 'k') move_sel(-1);
                        else if (buf[0] == 'h') move_sel(-list_rows);
                        else move_sel(list_rows);
                    } else {
                        if (buf[0] == 'j') move_sel(opt_cols);
                        else if (buf[0] == 'k') move_sel(-opt_cols);
                        else if (buf[0] == 'h') move_sel(-1);
                        else move_sel(1);
                    }
                    draw();
                } else if ((unsigned char)buf[0] >= 0x20 || (unsigned char)buf[0] >= 0x80) {
                    if (qlen + n < MAXQ - 1) {
                        memcpy(query + qlen, buf, (size_t)n);
                        qlen += n;
                        query[qlen] = 0;
                        sel = 0; refilter(); draw();
                    }
                }
            }
        } else if (ev.type == MotionNotify) {
            int mx = ev.xmotion.x, my = ev.xmotion.y, idx = -1;
            if (mode == MODE_DRUN) {
                int r = (my - input_h - opt_pad_t) / row_h;
                if (r >= 0 && r < list_rows && mx > 8 && mx < WW - 8) idx = scroll + r;
            } else {
                int per = opt_cols * opt_lines;
                int gw = opt_cols * cell_w + opt_cell_gap_x * (opt_cols - 1);
                int gx0 = opt_pad_l + (WW - opt_pad_l - opt_pad_r - gw) / 2;
                int gy0 = input_h + opt_pad_t;
                int cx = (mx - gx0) / (cell_w + opt_cell_gap_x);
                int cy = (my - gy0) / (cell_h + opt_cell_gap_y);
                if (cx >= 0 && cx < opt_cols && cy >= 0 && cy < opt_lines) {
                    int k = cy * opt_cols + cx;
                    if (k < per) idx = scroll + k;
                }
            }
            if (idx >= 0 && idx < nfilt && idx != sel) { sel = idx; draw(); }
        } else if (ev.type == ButtonPress) {
            int mx = (int)ev.xbutton.x, my = (int)ev.xbutton.y, idx = -1;
            if (ev.xbutton.button == Button4) { /* scroll up */
                if (mode == MODE_DRUN) move_sel(-1); else move_sel(-opt_cols);
                draw(); continue;
            }
            if (ev.xbutton.button == Button5) {
                if (mode == MODE_DRUN) move_sel(1); else move_sel(opt_cols);
                draw(); continue;
            }
            if (ev.xbutton.button != Button1) continue;
            if (mode == MODE_DRUN) {
                int r = (my - input_h - opt_pad_t) / row_h;
                if (r >= 0 && r < list_rows) idx = scroll + r;
            } else {
                int gw = opt_cols * cell_w + opt_cell_gap_x * (opt_cols - 1);
                int gx0 = opt_pad_l + (WW - opt_pad_l - opt_pad_r - gw) / 2;
                int gy0 = input_h + opt_pad_t;
                int cx = (mx - gx0) / (cell_w + opt_cell_gap_x);
                int cy = (my - gy0) / (cell_h + opt_cell_gap_y);
                if (cx >= 0 && cx < opt_cols && cy >= 0 && cy < opt_lines)
                    idx = scroll + cy * opt_cols + cx;
            }
            if (idx >= 0 && idx < nfilt) { sel = idx; activate(); }
        }
    }
    return 0;
}
