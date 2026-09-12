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
