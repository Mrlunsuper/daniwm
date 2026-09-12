#include "bar.h"

#include <X11/Xft/Xft.h>

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "client.h"
#include "ewmh.h"
#include "state.h"
#include "sysmon.h"
#include "tray.h"

/* Nerd Font icons (UTF-8 via \U escapes so the source stays ASCII).
 * Verified against JetBrainsMono Nerd Font with fontTools (all present).
 * Missing font on the target machine -> pick_icon() falls back to ASCII. */
#define ICO_CPU  "\U000F06E0"
#define ICO_MEM  "\U000F035B"
#define ICO_BAT  "\U000F0079"
#define ICO_VOL  "\U000F057E"
#define ICO_MUTE "\U000F0581"
#define ICO_CLK  "\U0000F017"
#define SEP_DOT  "\u00B7"
#define ELLIPSIS "\u2026"

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
/* Text fallbacks (Noto/DejaVu/Sans) open lazily on first missing glyph so
 * startup only pays for the main font + 2 Nerd icon fonts. One-shot per
 * bar_style(): after the attempt, misses just use barfont (tofu) instead
 * of re-probing fontconfig on every draw. */
static int text_fb_done = 0;
static void ensure_text_fbs(void) {
    static const char *text_cands[] = { "Noto Sans", "DejaVu Sans", "Sans", NULL };
    text_fb_done = 1;
    for (int i = 0; text_cands[i] && bar_nfb < 6; i++) {
        XftFont *f = XftFontOpenName(dpy, screen, text_cands[i]);
        if (f) barfont_fbs[bar_nfb++] = f;
    }
}
/* font holding ucs4 u: main first, then first fallback that has it */
static XftFont *bar_glyph_font(FcChar32 u) {
    /* fast path: ASCII almost always in the primary font */
    if (u < 0x80 && barfont) return barfont;
    if (barfont && XftCharExists(dpy, barfont, u)) return barfont;
    for (int i = 0; i < bar_nfb; i++)
        if (barfont_fbs[i] && XftCharExists(dpy, barfont_fbs[i], u)) return barfont_fbs[i];
    if (!text_fb_done) {
        int before = bar_nfb;
        ensure_text_fbs();
        for (int i = before; i < bar_nfb; i++)
            if (barfont_fbs[i] && XftCharExists(dpy, barfont_fbs[i], u)) return barfont_fbs[i];
    }
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
/* drop the last UTF-8 char from a NUL-terminated string */
static void utf8_pop(char *s) {
    size_t n = strlen(s);
    if (!n) return;
    size_t j = n - 1;
    while (j > 0 && (s[j] & 0xC0) == 0x80) j--;
    s[j] = 0;
}
/* module separator: BAR_GAP spaces around U+00B7, built fresh per draw so
 * measure and draw always use the identical string (alignment stays exact).
 * BAR_GAP = 2 reproduces the old hardcoded "  ·  ". */
static void build_msep(char *out, size_t n) {
    int g = BAR_GAP;
    size_t k = 0;
    if (g < 0) g = 0;
    if (g > 8) g = 8;
    for (int i = 0; i < g && k + 1 < n; i++) out[k++] = ' ';
    if (k + 2 < n) { memcpy(out + k, SEP_DOT, 2); k += 2; }
    for (int i = 0; i < g && k + 1 < n; i++) out[k++] = ' ';
    out[k < n ? k : n - 1] = 0;
}
/* icon or ASCII fallback: first codepoint must exist in barfont/fallbacks,
 * else return ascii so machines without a Nerd Font never show tofu. */
static const char *pick_icon(const char *icon_utf8, const char *ascii) {
    FcChar32 u;
    if (!icon_utf8 || !*icon_utf8 || !barfont) return ascii;
    utf8_decode((const unsigned char *)icon_utf8, strlen(icon_utf8), &u);
    if (XftCharExists(dpy, barfont, u)) return icon_utf8;
    for (int i = 0; i < bar_nfb; i++)
        if (barfont_fbs[i] && XftCharExists(dpy, barfont_fbs[i], u)) return icon_utf8;
    return ascii;
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
void drawbar(void) {
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
    unsigned long c_sep      = h_sep      ? C_SEP      : BAR_DIM;
    (void)c_sys; (void)c_title; (void)c_mode;

    /* text baseline from font metrics so bar_h != 24 stays vertically centered */
    int baseline = 16;
    if (barfont) baseline = (bar_h + barfont->ascent - barfont->descent) / 2;
    if (baseline < 4) baseline = 4;
    if (baseline > bar_h - 2) baseline = bar_h - 2;

    /* vol hitbox is only valid for the frame being drawn: drop it first so
     * a vanished backend can't leave a stale mute zone behind */
    vol_hit_x0 = vol_hit_x1 = -1;

    /* clear background of pixmap */
    XSetForeground(dpy, bargc, BAR_BG);
    XFillRectangle(dpy, barpm, bargc, 0, 0, (unsigned)barw, (unsigned)bar_h);

    int n = count_tiled();
    /* edge padding: left inset before the ws block, right margin after
     * text/tray (tray.c uses the same pad_r so icons line up with text) */
    int pad_l = S(BAR_PAD_L), pad_r = S(BAR_PAD_R);
    /* ---- workspaces: underline (default) or block (rollback) ---- */
    if (BAR_WS_STYLE == 1) {
        for (int i = 0; i < NWS; i++) {
            int x = pad_l + i * wsw;
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
                    bar_text(&barcol.urgent, x + S(12), baseline, label);
                } else {
                    XSetForeground(dpy, bargc, ws_occupied(i) ? c_ws_occ : c_ws_emp);
                    bar_text(ws_occupied(i) ? &barcol.ws_occ : &barcol.ws_emp, x + S(12), baseline, label);
                }
            }
        }
    } else {
        int uh = S(2);
        if (uh < 2) uh = 2;
        int wpad = S(6);
        for (int i = 0; i < NWS; i++) {
            int x = pad_l + i * wsw;
            char label[16];
            snprintf(label, sizeof(label), "%d", i + 1);
            int tw = bar_textw(label);
            int tx = x + (wsw - tw) / 2;
            if (tx < x) tx = x;
            if (i == curws) {
                /* active wins over urgent: urgent is usually cleared by focus.
                 * Underline mode draws on BAR_BG, so an un-overridden
                 * active text (default BAR_BG, meant for block fill) would be
                 * invisible -> fall back to the occupied (fg) color. */
                XftColor *actx = h_ws_act_tx ? &barcol.ws_acttx : &barcol.ws_occ;
                XSetForeground(dpy, bargc, c_ws_act);
                bar_text(actx, tx, baseline, label);
                if (wsw > wpad * 2)
                    XFillRectangle(dpy, barpm, bargc, x + wpad, bar_h - uh - 1,
                        (unsigned)(wsw - wpad * 2), (unsigned)uh);
            } else if (ws_has_urgent(i)) {
                XSetForeground(dpy, bargc, h_urgent ? C_URGENT : 0xe64553);
                bar_text(&barcol.urgent, tx, baseline, label);
            } else {
                XSetForeground(dpy, bargc, ws_occupied(i) ? c_ws_occ : c_ws_emp);
                bar_text(ws_occupied(i) ? &barcol.ws_occ : &barcol.ws_emp, tx, baseline, label);
            }
        }
    }
    /* short vertical separator after the ws block */
    {
        int sx = pad_l + NWS * wsw + S(4);
        int y1 = 6, y2 = bar_h - 7;
        if (y2 > y1 && sx < barw) {
            XSetForeground(dpy, bargc, c_sep);
            XDrawLine(dpy, barpm, bargc, sx, y1, sx, y2);
        }
    }

    /* ---- layout + counts + gaps ---- */
    char mode[64];
    snprintf(mode, sizeof(mode), "[%c] %dn%s", LAYOUT == L_TILE ? 'T' : 'M', n, gaps_on ? "" : " G-");
    int mx = pad_l + NWS * wsw + S(10);
    XSetForeground(dpy, bargc, c_mode);
    bar_text(&barcol.mode, mx, baseline, mode);
    int mode_end = mx + bar_textw(mode);

    /* ---- right segments: measure first (for title clipping), draw later ---- */
    struct RSeg { const char *icon; char val[24]; int alert; int draw; int isvol; };
    struct RSeg segs[4];
    int nsegs = 0;
    char clk[32] = "";
    {
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
        if (cached_cpu >= 0 && nsegs < 4) {
            segs[nsegs].icon = pick_icon(ico_cpu ? ico_cpu : ICO_CPU, "C");
            snprintf(segs[nsegs].val, sizeof(segs[nsegs].val), "%d%%", cached_cpu);
            segs[nsegs].alert = 0; segs[nsegs].draw = 1; segs[nsegs].isvol = 0; nsegs++;
        }
        if (cached_mem >= 0 && nsegs < 4) {
            segs[nsegs].icon = pick_icon(ico_mem ? ico_mem : ICO_MEM, "M");
            snprintf(segs[nsegs].val, sizeof(segs[nsegs].val), "%d%%", cached_mem);
            segs[nsegs].alert = 0; segs[nsegs].draw = 1; segs[nsegs].isvol = 0; nsegs++;
        }
        if (cached_bat >= 0 && nsegs < 4) {
            int low = cached_bat < 20 && cached_bat_chg[0] != '+';
            segs[nsegs].icon = pick_icon(ico_bat ? ico_bat : ICO_BAT, "B");
            snprintf(segs[nsegs].val, sizeof(segs[nsegs].val), "%d%s",
                cached_bat, cached_bat_chg);
            segs[nsegs].alert = low; segs[nsegs].draw = 1; segs[nsegs].isvol = 0; nsegs++;
        }
        if (vol && *vol) {
            int mute = !strcmp(vol, "MUTE");
            if (nsegs < 4) {
                segs[nsegs].icon = mute ? pick_icon(ico_mute ? ico_mute : ICO_MUTE, "V") : pick_icon(ico_vol ? ico_vol : ICO_VOL, "V");
                snprintf(segs[nsegs].val, sizeof(segs[nsegs].val), "%.15s",
                    mute ? "MUTE" : vol);
                segs[nsegs].alert = mute; segs[nsegs].draw = 1; segs[nsegs].isvol = 1; nsegs++;
            }
        } else if (nsegs == 4) {
            /* slots full and no room for vol: vol dropped, like before when
             * the single-line buffer filled up; nothing to do */
        }
        {
            struct tm *tm = localtime(&now);
            if (tm) strftime(clk, sizeof(clk), "%H:%M", tm);
            else snprintf(clk, sizeof(clk), "--:--");
        }
    }
    int trayw = tray_width_px();
    char msep[24];
    build_msep(msep, sizeof(msep));
    int space_w = bar_textw(" ");
    int mid_w = bar_textw(msep);
    /* clock icon: default Nerd clock (verified in JetBrainsMono Nerd Font);
     * empty (no Nerd Font, or `ico_clk =`) -> no icon, exactly the old look */
    const char *clk_icon = pick_icon(ico_clk ? ico_clk : ICO_CLK, "");
    int clk_icon_w = bar_textw(clk_icon);
    int right_w = bar_textw(clk);
    if (clk_icon_w > 0) right_w += clk_icon_w + space_w; /* icon + gap */
    for (int i = 0; i < nsegs; i++)
        right_w += bar_textw(segs[i].icon) + space_w + bar_textw(segs[i].val);
    if (nsegs > 0) right_w += mid_w; /* gap before clock */
    if (nsegs > 1) right_w += (nsegs - 1) * mid_w;
    if (trayw > 0) right_w += mid_w; /* separator between clock and tray */

    /* ---- focused title (UTF-8), clipped to the free space ---- */
    if (sel) {
        char t[128];
        if (get_title(sel->win, t, sizeof(t) - 16) > 0) {
            int tx = mode_end + S(12);
            int avail = (barw - right_w - trayw - pad_r) - tx - S(8);
            if (avail > 0) {
                XSetForeground(dpy, bargc, c_title);
                if (bar_textw(t) <= avail) {
                    bar_text(&barcol.title, tx, baseline, t);
                } else {
                    int ew = bar_textw(ELLIPSIS);
                    char tmp[160];
                    snprintf(tmp, sizeof(tmp), "%.150s", t);
                    while (tmp[0] && bar_textw(tmp) > avail - ew) utf8_pop(tmp);
                    size_t tn = strlen(tmp);
                    if (tn + 3 < sizeof(tmp) - 1) {
                        memcpy(tmp + tn, ELLIPSIS, 3);
                        tmp[tn + 3] = 0;
                    }
                    bar_text(&barcol.title, tx, baseline, tmp);
                }
            }
        }
    }

    /* ---- draw right segments left-to-right ---- */
    {
        int x = barw - right_w - trayw - pad_r;
        for (int i = 0; i < nsegs; i++) {
            XftColor *ic = segs[i].alert ? &barcol.urgent : &barcol.ws_act;
            XftColor *vc = segs[i].alert ? &barcol.urgent : &barcol.sys;
            int seg_x0 = x;
            bar_text(ic, x, baseline, segs[i].icon);
            x += bar_textw(segs[i].icon) + space_w;
            bar_text(vc, x, baseline, segs[i].val);
            x += bar_textw(segs[i].val);
            /* mute zone: icon + value only, not the trailing separator */
            if (segs[i].isvol) { vol_hit_x0 = seg_x0; vol_hit_x1 = x; }
            bar_text(&barcol.sep, x, baseline, msep);
            x += mid_w;
        }
        if (clk_icon_w > 0) {
            bar_text(&barcol.ws_act, x, baseline, clk_icon);
            x += clk_icon_w + space_w;
        }
        bar_text(&barcol.sys, x, baseline, clk);
        if (trayw > 0) {
            x += bar_textw(clk);
            bar_text(&barcol.sep, x, baseline, msep);
        }
    }

    tray_layout_icons();

    /* bottom border: 1px, sits right below the active underline */
    XSetForeground(dpy, bargc, c_sep);
    XFillRectangle(dpy, barpm, bargc, 0, (unsigned)(bar_h - 1), (unsigned)barw, 1);

    /* copy double-buffer to bar */
    XCopyArea(dpy, barpm, bar, bargc, 0, 0, (unsigned)barw, (unsigned)bar_h, 0, 0);
    XFlush(dpy);
}

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
void bar_style(void) {
    static const char *fallbacks[] = { NULL, "monospace:size=10", "monospace", "fixed", NULL };
    if (!bar) return;  /* nothing to (re)style without a bar window */
    Visual *vis = DefaultVisual(dpy, screen);
    Colormap cmap = DefaultColormap(dpy, screen);
    if (barxd) { XftDrawDestroy(barxd); barxd = NULL; }
    if (barpm) { XFreePixmap(dpy, barpm); barpm = None; }
    if (barfont) { XftFontClose(dpy, barfont); barfont = NULL; }
    for (int i = 0; i < bar_nfb; i++)
        if (barfont_fbs[i]) XftFontClose(dpy, barfont_fbs[i]);
    bar_nfb = 0;
    text_fb_done = 0; /* lazy text fallbacks reopen on next glyph miss */
    if (barcol_ok) {
        XftColorFree(dpy, vis, cmap, &barcol.bg);
        XftColorFree(dpy, vis, cmap, &barcol.ws_act);
        XftColorFree(dpy, vis, cmap, &barcol.ws_acttx);
        XftColorFree(dpy, vis, cmap, &barcol.ws_occ);
        XftColorFree(dpy, vis, cmap, &barcol.ws_emp);
        XftColorFree(dpy, vis, cmap, &barcol.mode);
        XftColorFree(dpy, vis, cmap, &barcol.title);
        XftColorFree(dpy, vis, cmap, &barcol.sys);
        XftColorFree(dpy, vis, cmap, &barcol.urgent);
        XftColorFree(dpy, vis, cmap, &barcol.sep);
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
    /* Eager fallback chain: Nerd Fonts only (same point size as the main
     * font, scaled) so icons resolve on the very first draw. Text fallbacks
     * (Noto/DejaVu/Sans for VN/symbols) open lazily via ensure_text_fbs()
     * on the first glyph miss — see bar_glyph_font(). NOTE: a custom
     * ico_* glyph living only in a text font falls back to ASCII until the
     * lazy load triggers; the 1s tick redraw then picks the real icon. */
    {
        double base_sz = 10.0;
        if (font_name) {
            const char *pp = strstr(font_name, "size=");
            if (pp) {
                double v = strtod(pp + 5, NULL);
                if (v >= 1.0 && v <= 128.0) base_sz = v;
            }
        }
        char nerd1[96], nerd2[96];
        snprintf(nerd1, sizeof(nerd1), "JetBrainsMono Nerd Font Mono:size=%.1f",
            base_sz * (double)ui_scale);
        snprintf(nerd2, sizeof(nerd2), "Symbols Nerd Font Mono:size=%.1f",
            base_sz * (double)ui_scale);
        const char *fb_cands[] = { nerd1, nerd2, NULL };
        for (int i = 0; fb_cands[i] && bar_nfb < 6; i++) {
            XftFont *f = XftFontOpenName(dpy, screen, fb_cands[i]);
            if (f) barfont_fbs[bar_nfb++] = f;
        }
    }
    xft_alloc(BAR_BG, &barcol.bg);
    xft_alloc(h_ws_act ? C_WS_ACT : BAR_ACC, &barcol.ws_act);
    xft_alloc(h_ws_act_tx ? C_WS_ACT_TX : BAR_BG, &barcol.ws_acttx);
    xft_alloc(h_ws_occ ? C_WS_OCC : BAR_FG, &barcol.ws_occ);
    xft_alloc(h_ws_emp ? C_WS_EMP : BAR_DIM, &barcol.ws_emp);
    xft_alloc(h_mode ? C_MODE : BAR_FG, &barcol.mode);
    xft_alloc(h_title ? C_TITLE : BAR_FG, &barcol.title);
    xft_alloc(h_sys ? C_SYS : BAR_DIM, &barcol.sys);
    xft_alloc(h_urgent ? C_URGENT : 0xe64553, &barcol.urgent);
    xft_alloc(h_sep ? C_SEP : BAR_DIM, &barcol.sep);
    barcol_ok = 1;
    drawbar();
}
