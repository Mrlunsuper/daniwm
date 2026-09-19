#define _POSIX_C_SOURCE 200809L /* getline() under strict -std=c11 */
#include "config.h"

#include <X11/Xlib.h>
#include <X11/keysym.h>

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <wordexp.h>

#include "bar.h"
#include "client.h"
#include "ewmh.h"
#include "keys.h"
#include "layout.h"
#include "mouse.h"
#include "tray.h"

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
void ws_set_name(int idx, const char *val) {
    if (idx < 0 || idx >= MAXWS) return;
    free(ws_names[idx]);
    ws_names[idx] = NULL;
    if (!val) return;
    while (*val && isspace((unsigned char)*val)) val++;
    const char *e = val + strlen(val);
    while (e > val && isspace((unsigned char)e[-1])) e--;
    if (e <= val) return; /* empty = back to number */
    size_t n = (size_t)(e - val);
    if (n > 63) n = 63;
    char tmp[64];
    memcpy(tmp, val, n);
    tmp[n] = 0;
    /* never split a UTF-8 char at the cap: back off to a boundary */
    size_t m = n;
    while (m > 0 && (tmp[m - 1] & 0xC0) == 0x80) m--;
    if (m > 0 && (tmp[m - 1] & 0x80)) {
        unsigned char lead = (unsigned char)tmp[m - 1];
        size_t want = (lead >> 5) == 0x6 ? 2 : (lead >> 4) == 0xE ? 3 : 4;
        if (n - (m - 1) < want) m--;
    }
    tmp[m] = 0;
    if (!tmp[0]) return;
    ws_names[idx] = xstrdup(tmp);
}
/* `ws_names = ...`: wordexp split (quotes keep spaces: "my web").
 * Each token is either positional (fills next slot) or indexed `N:name`
 * (N = 1..10). Indexed + positional can mix. */
static void parse_ws_names(const char *val) {
    wordexp_t w;
    if (wordexp(val, &w, WRDE_NOCMD) != 0) return;
    int pos = 0;
    for (size_t i = 0; i < w.we_wordc; i++) {
        const char *tok = w.we_wordv[i];
        if (!tok || !*tok) continue;
        const char *colon = strchr(tok, ':');
        if (colon && colon != tok) {
            char num[8];
            size_t dl = (size_t)(colon - tok);
            if (dl < sizeof(num)) {
                memcpy(num, tok, dl);
                num[dl] = 0;
                char *ep = NULL;
                long n = strtol(num, &ep, 10);
                if (ep && !*ep && n >= 1 && n <= MAXWS) {
                    ws_set_name((int)n - 1, colon + 1);
                    continue;
                }
            }
        }
        while (pos < MAXWS && ws_names[pos]) pos++;
        if (pos < MAXWS) ws_set_name(pos++, tok);
    }
    wordfree(&w);
}
/* `ws_name_N = Foo` (N = 1..10). Accepts ws_name_1 / wsname1 / ws_1_name. */
static int parse_ws_name_key(const char *key, const char *val) {
    if (strncmp(key, "ws", 2) != 0 || !strstr(key, "name")) return 0;
    if (!strcmp(key, "ws_names") || !strcmp(key, "wsnames")) return 0;
    const char *p = key + 2;
    int idx = -1;
    while (*p) {
        if (isdigit((unsigned char)*p)) {
            char *ep = NULL;
            long n = strtol(p, &ep, 10);
            if (n >= 1 && n <= MAXWS) idx = (int)n - 1;
            break;
        }
        p++;
    }
    if (idx < 0) return 0;
    ws_set_name(idx, val);
    return 1;
}
/* `ws_icon_N = <glyph>` (N = 1..10). Accepts ws_icon_1 / wsicon1 / ws_1_icon.
 * Empty clears. Glyph cap ~15 bytes (a few codepoints max). */
static int parse_ws_icon_key(const char *key, const char *val) {
    if (strncmp(key, "ws", 2) != 0 || !strstr(key, "icon")) return 0;
    const char *p = key + 2;
    int idx = -1;
    while (*p) {
        if (isdigit((unsigned char)*p)) {
            char *ep = NULL;
            long n = strtol(p, &ep, 10);
            if (n >= 1 && n <= MAXWS) idx = (int)n - 1;
            break;
        }
        p++;
    }
    if (idx < 0) return 0;
    free(ws_icons[idx]);
    ws_icons[idx] = NULL;
    if (!val) return 1;
    while (*val && isspace((unsigned char)*val)) val++;
    const char *e = val + strlen(val);
    while (e > val && isspace((unsigned char)e[-1])) e--;
    if (e <= val) return 1; /* empty = no icon */
    size_t n = (size_t)(e - val);
    if (n > 15) n = 15;
    char tmp[16];
    memcpy(tmp, val, n);
    tmp[n] = 0;
    ws_icons[idx] = xstrdup(tmp);
    return 1;
}
void config_defaults(void) {
    for (unsigned i = 0; i < nrules; i++) { free(rules[i].cls); free(rules[i].title); }
    nrules = 0;
    keys_reset();
    for (int i = 0; i < MAXWS; i++) { free(ws_names[i]); ws_names[i] = NULL; }
    for (int i = 0; i < MAXWS; i++) { free(ws_icons[i]); ws_icons[i] = NULL; }
    free(RENAME_CMD); RENAME_CMD = xstrdup("rofi -dmenu -i -p rename");
    MOD = Mod4Mask; BORDER = 2; NWS = 5;
    BORDER_FOCUS = 0x7aa2f7; BORDER_NORMAL = 0x333333;
    BAR_BG = 0x1e1e2e; BAR_FG = 0xcdd6f4; BAR_ACC = 0x7aa2f7; BAR_DIM = 0x6c7086;
    C_WS_ACT = C_WS_ACT_TX = C_WS_OCC = C_WS_EMP = C_MODE = C_TITLE = C_SYS = 0;
    h_ws_act = h_ws_act_tx = h_ws_occ = h_ws_emp = h_mode = h_title = h_sys = 0;
    C_TASK_ACT = C_TASK_ACT_TX = C_TASK_TEXT = 0;
    h_task_act = h_task_act_tx = h_task_text = 0;
    C_URGENT = 0xeb6f92; C_SEP = 0; h_urgent = h_sep = 0; BAR_WS_STYLE = 2;
    BAR_TASK_STYLE = -1;
    BAR_H = 24; WS_W = 46; BAR_WS_PAD = 12; ui_scale = 1.0f;
    BAR_GAP = 3;
    BAR_PAD_L = 10; BAR_PAD_R = 12;
    free(BAR_MODULES); BAR_MODULES = xstrdup("cpu mem bat vol clock");
    free(CLOCK_FMT); CLOCK_FMT = xstrdup("%d/%m %H:%M");
    free(BAR_SEP_STR); BAR_SEP_STR = xstrdup("\u00B7");
    BAR_SHOW_TITLE = 1; BAR_SHOW_LAYOUT = 1; BAR_SHOW_TASKS = 1; BAR_TASK_W = 0;
    BAR_TASK_PAD = 6;
    free(ico_cpu); ico_cpu = xstrdup("\U000F06E0");
    free(ico_mem); ico_mem = xstrdup("\U000F035B");
    free(ico_bat); ico_bat = xstrdup("\U000F0079");
    free(ico_vol); ico_vol = xstrdup("\U000F057E");
    free(ico_mute); ico_mute = xstrdup("\U000F0581");
    free(ico_clk); ico_clk = xstrdup("\U0000F017");
    tray_on = 1;
    TRAY_GAP = 6;
    free(font_name);
    font_name = xstrdup("JetBrainsMono Nerd Font Mono:size=10");
    bar_on = 1; gaps_on = 1; gap_outer = 10; gap_inner = 8;
    COMP_ON = 0; COMP_SHADOW = 1; COMP_FADE = 1; COMP_DIM = 0.92f;
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
    } else if (!strcmp(key, "bar_urgent")) {
        if (parse_hex(val, &h)) { C_URGENT = h; h_urgent = 1; }
    } else if (!strcmp(key, "bar_sep")) {
        if (parse_hex(val, &h)) { C_SEP = h; h_sep = 1; }
    } else if (!strcmp(key, "bar_ws_style")) {
        if (!strcasecmp(val, "underline")) BAR_WS_STYLE = 0;
        else if (!strcasecmp(val, "block")) BAR_WS_STYLE = 1;
        else if (!strcasecmp(val, "pill")) BAR_WS_STYLE = 2;
        else fprintf(stderr, "daniwm: bad bar_ws_style '%s' (want pill|underline|block)\n", val);
    } else if (!strcmp(key, "bar_task_style") || !strcmp(key, "task_style")) {
        if (!strcasecmp(val, "underline")) BAR_TASK_STYLE = 0;
        else if (!strcasecmp(val, "block")) BAR_TASK_STYLE = 1;
        else if (!strcasecmp(val, "pill")) BAR_TASK_STYLE = 2;
        else if (!strcasecmp(val, "none")) BAR_TASK_STYLE = 3;
        else if (!strcasecmp(val, "follow")) BAR_TASK_STYLE = -1;
        else fprintf(stderr, "daniwm: bad bar_task_style '%s' (want follow|pill|underline|block|none)\n", val);
    } else if (!strcmp(key, "bar_task_active") || !strcmp(key, "task_active")) {
        if (parse_hex(val, &h)) { C_TASK_ACT = h; h_task_act = 1; }
    } else if (!strcmp(key, "bar_task_active_text") || !strcmp(key, "task_active_text")) {
        if (parse_hex(val, &h)) { C_TASK_ACT_TX = h; h_task_act_tx = 1; }
    } else if (!strcmp(key, "bar_task_text") || !strcmp(key, "bar_task_normal") ||
               !strcmp(key, "task_text") || !strcmp(key, "task_normal")) {
        if (parse_hex(val, &h)) { C_TASK_TEXT = h; h_task_text = 1; }
    } else if (!strcmp(key, "bar_modules")) {
        char *dup = xstrdup(val);
        if (dup) { free(BAR_MODULES); BAR_MODULES = dup; }
    } else if (!strcmp(key, "clock_fmt")) {
        char *dup = xstrdup(val);
        if (dup) { free(CLOCK_FMT); CLOCK_FMT = dup; }
    } else if (!strcmp(key, "bar_sep_str")) {
        char *dup = xstrdup(val);
        if (dup) { free(BAR_SEP_STR); BAR_SEP_STR = dup; }
    } else if (!strcmp(key, "bar_show_title") || !strcmp(key, "bar_title_on") || !strcmp(key, "show_title")) {
        if (parse_bool(val, &b)) BAR_SHOW_TITLE = b;
    } else if (!strcmp(key, "bar_show_layout") || !strcmp(key, "bar_layout_on") || !strcmp(key, "show_layout")) {
        if (parse_bool(val, &b)) BAR_SHOW_LAYOUT = b;
    } else if (!strcmp(key, "bar_show_tasks") || !strcmp(key, "show_tasks")) {
        if (parse_bool(val, &b)) BAR_SHOW_TASKS = b;
    } else if (!strcmp(key, "bar_task_w") || !strcmp(key, "task_w")) {
        v = strtol(val, NULL, 10);
        if (v >= 0 && v <= 512) BAR_TASK_W = (int)v;
        else fprintf(stderr, "daniwm: bad bar_task_w '%s' (want 0..512, 0=auto)\n", val);
    } else if (!strcmp(key, "bar_task_pad") || !strcmp(key, "task_pad")) {
        v = strtol(val, NULL, 10);
        if (v >= 0 && v <= 32) BAR_TASK_PAD = (int)v;
        else fprintf(stderr, "daniwm: bad bar_task_pad '%s' (want 0..32)\n", val);
    } else if (!strcmp(key, "bar_gap")) {
        v = strtol(val, NULL, 10); if (v >= 0 && v <= 8) BAR_GAP = (int)v;
        else fprintf(stderr, "daniwm: bad bar_gap '%s' (want 0..8)\n", val);
    } else if (!strcmp(key, "ico_cpu")) {
        char *dup = xstrdup(val);
        if (dup) { free(ico_cpu); ico_cpu = dup; }
    } else if (!strcmp(key, "ico_mem")) {
        char *dup = xstrdup(val);
        if (dup) { free(ico_mem); ico_mem = dup; }
    } else if (!strcmp(key, "ico_bat")) {
        char *dup = xstrdup(val);
        if (dup) { free(ico_bat); ico_bat = dup; }
    } else if (!strcmp(key, "ico_vol")) {
        char *dup = xstrdup(val);
        if (dup) { free(ico_vol); ico_vol = dup; }
    } else if (!strcmp(key, "ico_mute")) {
        char *dup = xstrdup(val);
        if (dup) { free(ico_mute); ico_mute = dup; }
    } else if (!strcmp(key, "ico_clk")) {
        char *dup = xstrdup(val);
        if (dup) { free(ico_clk); ico_clk = dup; }
    } else if (!strcmp(key, "bar_pad_l")) {
        v = strtol(val, NULL, 10); if (v >= 0 && v <= 32) BAR_PAD_L = (int)v;
        else fprintf(stderr, "daniwm: bad bar_pad_l '%s' (want 0..32)\n", val);
    } else if (!strcmp(key, "bar_pad_r")) {
        v = strtol(val, NULL, 10); if (v >= 0 && v <= 32) BAR_PAD_R = (int)v;
        else fprintf(stderr, "daniwm: bad bar_pad_r '%s' (want 0..32)\n", val);
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
    } else if (!strcmp(key, "bar_ws_pad") || !strcmp(key, "ws_pad")) {
        v = strtol(val, NULL, 10);
        if (v >= 0 && v <= 32) BAR_WS_PAD = (int)v;
        else fprintf(stderr, "daniwm: bad bar_ws_pad '%s' (want 0..32)\n", val);
    } else if (!strcmp(key, "tray")) {
        if (parse_bool(val, &b)) tray_on = b;
    } else if (!strcmp(key, "tray_gap") || !strcmp(key, "tray_pad") ||
               !strcmp(key, "tray_spacing") || !strcmp(key, "tray_icons_gap")) {
        v = strtol(val, NULL, 10);
        if (v >= 0 && v <= 32) TRAY_GAP = (int)v;
        else fprintf(stderr, "daniwm: bad tray_gap '%s' (want 0..32)\n", val);
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
    } else if (!strcmp(key, "ws_names") || !strcmp(key, "wsnames") || !strcmp(key, "workspace_names")) {
        parse_ws_names(val);
    } else if (parse_ws_name_key(key, val)) {
        ; /* ws_name_N handled above */
    } else if (parse_ws_icon_key(key, val)) {
        ; /* ws_icon_N handled above */
    } else if (!strcmp(key, "rename_cmd") || !strcmp(key, "rename")) {
        char *dup = xstrdup(val);
        if (dup) { free(RENAME_CMD); RENAME_CMD = dup; }
    } else if (!strcmp(key, "compositor")) {
        if (parse_bool(val, &b)) COMP_ON = b;
    } else if (!strcmp(key, "shadow")) {
        if (parse_bool(val, &b)) COMP_SHADOW = b;
    } else if (!strcmp(key, "fade")) {
        if (parse_bool(val, &b)) COMP_FADE = b;
    } else if (!strcmp(key, "inactive_dim")) {
        f = strtof(val, NULL);
        if (f >= 0.5f && f <= 1.0f) COMP_DIM = f;
        else fprintf(stderr, "daniwm: bad inactive_dim '%s' (want 0.5..1.0)\n", val);
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
    /* action name (after last ':') — 512 để chứa `exec ...` dài */
    char aname_buf[512];
    size_t alen = strlen(sep + 1);
    if (alen >= sizeof(aname_buf)) {
        fprintf(stderr, "daniwm: %s:%d: action too long in '%s'\n", path, lineno, val);
        return;
    }
    memcpy(aname_buf, sep + 1, alen + 1);
    char *aname = trim(aname_buf);
    void (*fn)(int) = NULL;
    int arg = 0;
    /* generic command: bind = mod+Print:exec flameshot gui */
    if ((!strncasecmp(aname, "exec", 4) && (aname[4] == ' ' || aname[4] == ':' || aname[4] == '=')) ||
        (!strncasecmp(aname, "spawn", 5) && (aname[5] == ' ' || aname[5] == ':' || aname[5] == '='))) {
        char *cmd = trim(aname + (aname[0] == 'e' || aname[0] == 'E' ? 5 : 6));
        if (!*cmd) {
            fprintf(stderr, "daniwm: %s:%d: empty exec command\n", path, lineno);
            return;
        }
        char **argv = split_argv(cmd);
        if (!argv || !argv[0]) {
            fprintf(stderr, "daniwm: %s:%d: bad exec '%s'\n", path, lineno, cmd);
            free_argv(argv);
            return;
        }
        int idx = push_exec_cmd(argv);
        if (idx < 0) { free_argv(argv); return; }
        fn = k_exec; arg = idx;
        goto have_action;
    }
    for (unsigned i = 0; i < nactions; i++)
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
have_action:
    ; /* ISO C: label must precede a statement, not a declaration */
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
static int has_ws_action(void (*fn)(int), int arg) {
    for (unsigned i = 0; i < nkeys; i++)
        if (keys[i].fn == fn && keys[i].arg == arg) return 1;
    return 0;
}
static int combo_taken(KeySym ks, unsigned int mod) {
    for (unsigned i = 0; i < nkeys; i++)
        if (keys[i].keysym == ks && keys[i].mod == mod) return 1;
    return 0;
}
/* Auto-fill view/move binds for workspaces the user didn't bind explicitly
 * (e.g. `workspaces` raised 7 -> 9 with binds only up to 7). Uses the same
 * 1..9,0 convention as the defaults; never steals an occupied combo. */
static void fill_ws_binds(void) {
    for (int i = 0; i < NWS; i++) {
        KeySym ks = (i < 9) ? (KeySym)(XK_1 + i) : XK_0;
        if (!has_ws_action(view, i) && !combo_taken(ks, MOD))
            push_key_fn(ks, MOD, view, i);
        if (!has_ws_action(send_to, i) && !combo_taken(ks, MOD | ShiftMask))
            push_key_fn(ks, MOD | ShiftMask, send_to, i);
    }
}
void load_config(const char *path) {
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
        if (strcmp(k, "bind") && strcmp(k, "bind+") && strcmp(k, "rule")) parse_scalar(k, v);
        free(dup);
    }
    /* pass 2: rules/binds; first occurrence replaces defaults.
     * `bind+` appends to defaults (deferred past the rebuild below so a
     * changed `mod` applies to defaults first, then extras). */
    int saw_rule = 0, saw_bind = 0;
    char **plusbinds = NULL;
    int *pluslines = NULL;
    unsigned nplus = 0, capplus = 0;
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
            if (!saw_bind) { keys_reset(); saw_bind = 1; }
            parse_bind(v, (int)(i + 1), path);
        } else if (!strcmp(k, "bind+")) {
            if (nplus == capplus) {
                unsigned nc = capplus ? capplus * 2 : 8;
                char **nb = realloc(plusbinds, nc * sizeof(*nb));
                int *nl = realloc(pluslines, nc * sizeof(*nl));
                if (!nb || !nl) { free(nb); free(nl); continue; }
                plusbinds = nb; pluslines = nl; capplus = nc;
            }
            plusbinds[nplus] = xstrdup(v);
            if (!plusbinds[nplus]) continue;
            pluslines[nplus] = (int)(i + 1);
            nplus++;
        } else if (strcmp(k, "mod") && strcmp(k, "border") && strcmp(k, "border_focus") &&
            strcmp(k, "border_normal") && strcmp(k, "bar_bg") && strcmp(k, "bar_fg") &&
            strcmp(k, "bar_acc") && strcmp(k, "bar_dim") && strcmp(k, "bar_ws_active") &&
            strcmp(k, "bar_ws_active_text") && strcmp(k, "bar_ws_occ") && strcmp(k, "bar_ws_empty") &&
            strcmp(k, "bar_mode") && strcmp(k, "bar_title") && strcmp(k, "bar_sys") &&
            strcmp(k, "bar_urgent") && strcmp(k, "bar_sep") && strcmp(k, "bar_ws_style") &&
            strcmp(k, "bar_task_style") && strcmp(k, "task_style") &&
            strcmp(k, "bar_task_active") && strcmp(k, "task_active") &&
            strcmp(k, "bar_task_active_text") && strcmp(k, "task_active_text") &&
            strcmp(k, "bar_task_text") && strcmp(k, "bar_task_normal") &&
            strcmp(k, "task_text") && strcmp(k, "task_normal") &&
            strcmp(k, "bar_modules") && strcmp(k, "clock_fmt") && strcmp(k, "bar_sep_str") &&
            strcmp(k, "bar_show_title") && strcmp(k, "bar_title_on") && strcmp(k, "show_title") &&
            strcmp(k, "bar_show_layout") && strcmp(k, "bar_layout_on") && strcmp(k, "show_layout") &&
            strcmp(k, "bar_show_tasks") && strcmp(k, "show_tasks") &&
            strcmp(k, "bar_task_w") && strcmp(k, "task_w") &&
            strcmp(k, "bar_task_pad") && strcmp(k, "task_pad") &&
            strcmp(k, "bar_h") && strcmp(k, "bar_gap") && strcmp(k, "ico_cpu") &&
            strcmp(k, "ico_mem") && strcmp(k, "ico_bat") && strcmp(k, "ico_vol") &&
            strcmp(k, "ico_mute") && strcmp(k, "ico_clk") && strcmp(k, "bar_pad_l") &&
            strcmp(k, "bar_pad_r") && strcmp(k, "scale") && strcmp(k, "font") && strcmp(k, "ws_w") && strcmp(k, "bar_ws_pad") && strcmp(k, "ws_pad") && strcmp(k, "tray") && strcmp(k, "tray_gap") && strcmp(k, "tray_pad") &&
            strcmp(k, "tray_spacing") && strcmp(k, "tray_icons_gap") && strcmp(k, "bar_on") && strcmp(k, "gaps_on") &&
            strcmp(k, "gap_outer") && strcmp(k, "gap_inner") && strcmp(k, "compositor") &&
            strcmp(k, "shadow") && strcmp(k, "fade") && strcmp(k, "inactive_dim") && strcmp(k, "mfact") &&
            strcmp(k, "nmaster") && strcmp(k, "workspaces") && strcmp(k, "rename_cmd") &&
            strcmp(k, "rename") && strcmp(k, "ws_names") &&
            strcmp(k, "wsnames") && strcmp(k, "workspace_names") &&
            strncmp(k, "ws_name", 7) && strncmp(k, "wsname", 6) &&
            strncmp(k, "ws_icon", 7) && strncmp(k, "wsicon", 6) &&
            strcmp(k, "term") &&
            strcmp(k, "menu") && strcmp(k, "scratch")) {
            fprintf(stderr, "daniwm: %s:%u: unknown key '%s'\n", path, i + 1, k);
        }
    }
    if (!saw_bind) { keys_reset(); add_default_keys(); } /* rebuild with final MOD+NWS */
    for (unsigned i = 0; i < nplus; i++) {
        parse_bind(plusbinds[i], pluslines[i], path);
        free(plusbinds[i]);
    }
    free(plusbinds); free(pluslines);
    fill_ws_binds(); /* mod+8/9/0 etc. for ws beyond the user's explicit binds */
    finalize_nws();
    for (unsigned i = 0; i < nlines; i++) free(lines[i]);
    free(lines);
}
/* Sync per-workspace arrays to NWS. New slots get current defaults;
 * shrinking folds extra workspaces into the last one, and a parked
 * scratchpad follows the sentinel. OOM-safe: keeps the old set. */
void finalize_nws(void) {
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
    if (prevws >= NWS || prevws == curws) prevws = curws;
    for (i = 0; i < NWS; i++)
        if (ws_sel[i] && ws_sel[i]->ws != i) ws_sel[i] = NULL;
    update_struts();
    ewmh_desktops();
}
void k_reload(int unused) {
    (void)unused;
    load_config(NULL);
    for (int i = 0; i < NWS; i++) {
        if (ws_mfact[i] < 0.1f || ws_mfact[i] > 0.9f) ws_mfact[i] = def_mfact;
        if (ws_nmaster[i] < 1) ws_nmaster[i] = def_nmaster;
    }
    if (!sel || sel->ws != curws) sel = first_in_ws(curws);
    /* finalize may have moved curws or folded windows across it: re-sync
     * visibility like view() does (arrange maps tiled but never unmaps). */
    for (Client *c = clients; c; c = c->next) {
        if (c->ws == curws) {
            /* Skip monocle-hidden windows: arrange() below re-hides them
             * (or maps them if the layout is now tile). Mapping them here
             * first only causes a transient over-map flash. */
            if (!c->hidden) XMapWindow(dpy, c->win);
        } else XUnmapWindow(dpy, c->win);
    }
    if (bar) XMoveResizeWindow(dpy, bar, mons[0].x, mons[0].y, (unsigned)barw, (unsigned)S(BAR_H));
    if (!bar_on && bar) XUnmapWindow(dpy, bar);
    if (bar_on && bar) XMapWindow(dpy, bar);
    tray_enable(tray_on);
    ewmh_desktops(); /* names may have changed with same NWS (no finalize) */
    bar_style();
    grabkeys();
    for (Client *c = clients; c; c = c->next) grabbuttons(c);
    update_struts();
    arrange();
    if (sel) focus(sel);
}
