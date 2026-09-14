/* comp.c - dani-comp: compositor đơn giản cho daniwm.
 *
 * Tính năng (cố tình minimal, ~600 dòng):
 *   - bóng đổ mềm 3 lớp (XRender, không blur convolution)
 *   - fade-in khi map (~160ms)
 *   - dim cửa sổ inactive (mặc định 0.92, tắt khi focus/fullscreen)
 *   - tôn trọng _NET_WM_WINDOW_OPACITY (transset, rules opacity...)
 *   - công bố _NET_WM_CM_Sn theo EWMH (nhường nếu đã có compositor khác)
 *
 * Cách chạy:
 *   dani-comp [-d display] [--no-shadow] [--no-fade] [--dim 0.5..1] [--fade-ms N]
 * Hoặc để daniwm tự spawn: `compositor = 1` trong ~/.config/daniwm/config.
 *
 * Giới hạn (ghi rõ để khỏi cãi):
 *   - vẽ trực tiếp lên root (XClearWindow + composite lại toàn bộ khi dirty):
 *     rẻ, đúng với ít cửa sổ, nhưng có thể nháy nhẹ khi resize liên tục.
 *   - không xử lý shaped window (bounding shape) — cửa sổ shape hiện thành
 *     hình chữ nhật. Đủ cho xterm/firefox/rofi/dmenu.
 *   - không vsync thật (chỉ gom Damage ~60Hz). Muốn mượt như picom thì
 *     dùng picom; dani-comp là để "có compositor của nhà trồng".
 *
 * Build: `make dani-comp` (xem Makefile).
 */
#define _POSIX_C_SOURCE 200809L
#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/extensions/Xcomposite.h>
#include <X11/extensions/Xdamage.h>
#include <X11/extensions/Xrender.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <sys/select.h>

static Display *dpy;
static Window root;
static int screen;
static int sw, sh;
static int xfd;

/* atoms */
static Atom A_CM;          /* _NET_WM_CM_S<screen> */
static Atom A_OPACITY;     /* _NET_WM_WINDOW_OPACITY */
static Atom A_ACTIVE;      /* _NET_ACTIVE_WINDOW */
static Atom A_WM_STATE;    /* _NET_WM_STATE */
static Atom A_WM_STATE_FS; /* _NET_WM_STATE_FULLSCREEN */
static Atom A_WM_WINTYPE;  /* _NET_WM_WINDOW_TYPE */
static Atom A_WM_WINTYPE_DOCK;

/* options */
static int opt_shadow = 1;
static int opt_fade = 1;
static double opt_dim = 0.92; /* 1.0 = tắt dim */
static int opt_fade_ms = 160;
static int opt_verbose = 0;

/* damage ext */
static int dmg_event = 0, dmg_error = 0;

/* render */
static Picture root_pict = None;
static XRenderPictFormat *argb_fmt; /* PictStandardARGB32 */
static int root_pw = 0, root_ph = 0;

/* selection window (giữ _NET_WM_CM_Sn) */
static Window cm_win = None;

/* ---- tracked windows ---- */
typedef struct Win Win;
struct Win {
    Window id;
    Damage damage;
    Pixmap pixmap;
    Picture pict;
    int w, h;            /* kích thước pixmap đang cache */
    int x, y;            /* vị trí root-relative lúc repaint gần nhất */
    int bw;
    int mapped;          /* đang viewable */
    int override;        /* override_redirect (bar/tray/menu...) */
    int fullscreen;
    int dock;
    unsigned opacity;    /* _NET_WM_WINDOW_OPACITY, mặc định ~0U */
    long long born;      /* ms, để fade-in */
    Win *next;
};
static Win *wins;
static Window active_win = None;
static int dirty = 1;      /* cần repaint */
static int need_clear = 1; /* cần xóa nền (chỉ khi đổi geometry/visibility) */
static int opt_debug = 0;  /* DANI_COMP_DEBUG=1: log event/paint ra stderr */
static long n_damage = 0, n_paint = 0;
static volatile sig_atomic_t quit_req = 0; /* SIGTERM/SIGINT -> dọn rồi thoát */
static void on_quit(int sig) { (void)sig; quit_req = 1; }
static int fading = 0; /* đang có anim -> giữ nhịp 60Hz */
static long long last_paint;

#define OPAQUE (~0U)

static long long now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000LL + ts.tv_nsec / 1000000LL;
}


static Win *win_get(Window id) {
    for (Win *w = wins; w; w = w->next)
        if (w->id == id) return w;
    return NULL;
}

static void win_free_pix(Win *w) {
    if (w->pict) { XRenderFreePicture(dpy, w->pict); w->pict = None; }
    if (w->pixmap) { XFreePixmap(dpy, w->pixmap); w->pixmap = None; }
    w->w = w->h = 0;
}

static unsigned read_opacity(Window id) {
    Atom rt; int rf; unsigned long n = 0, extra = 0;
    unsigned char *data = NULL;
    unsigned op = OPAQUE;
    if (A_OPACITY == None) return OPAQUE;
    if (XGetWindowProperty(dpy, id, A_OPACITY, 0, 1, False, XA_CARDINAL,
            &rt, &rf, &n, &extra, &data) == Success && data) {
        if (rf == 32 && n == 1) op = *(unsigned long *)data;
        XFree(data);
    }
    return op;
}

static int has_atom(Window id, Atom prop, Atom val) {
    Atom rt; int rf; unsigned long n = 0, extra = 0;
    unsigned char *data = NULL;
    int found = 0;
    if (prop == None || val == None) return 0;
    if (XGetWindowProperty(dpy, id, prop, 0, 64, False, XA_ATOM,
            &rt, &rf, &n, &extra, &data) == Success && data) {
        if (rf == 32) {
            Atom *p = (Atom *)data;
            for (unsigned long i = 0; i < n; i++)
                if (p[i] == val) { found = 1; break; }
        }
        XFree(data);
    }
    return found;
}

static void win_refresh_flags(Win *w) {
    XWindowAttributes a;
    if (!XGetWindowAttributes(dpy, w->id, &a)) { w->mapped = 0; return; }
    w->mapped = (a.map_state == IsViewable);
    w->override = a.override_redirect ? 1 : 0;
    w->x = a.x; w->y = a.y; w->bw = a.border_width;
    w->opacity = read_opacity(w->id);
    w->fullscreen = has_atom(w->id, A_WM_STATE, A_WM_STATE_FS);
    w->dock = has_atom(w->id, A_WM_WINTYPE, A_WM_WINTYPE_DOCK);
}

static Win *win_add(Window id) {
    Win *w = win_get(id);
    if (w) { win_refresh_flags(w); return w; }
    w = calloc(1, sizeof(*w));
    if (!w) return NULL;
    w->id = id;
    w->opacity = OPAQUE;
    w->born = now_ms();
    w->next = wins;
    wins = w;
    /* theo dõi damage + prop của cửa sổ này */
    XSelectInput(dpy, id, PropertyChangeMask | StructureNotifyMask);
    w->damage = XDamageCreate(dpy, id, XDamageReportNonEmpty);
    win_refresh_flags(w);
    /* override_redirect (bar của daniwm, menu...) không fade: hiện ngay */
    if (w->override) w->born = 0;
    return w;
}

static void win_del(Window id) {
    Win **p = &wins;
    while (*p && (*p)->id != id) p = &(*p)->next;
    if (!*p) return;
    Win *w = *p;
    *p = w->next;
    if (w->damage) XDamageDestroy(dpy, w->damage);
    win_free_pix(w);
    free(w);
}

/* active window hiện tại (để dim inactive) */
static void read_active(void) {
    Atom rt; int rf; unsigned long n = 0, extra = 0;
    unsigned char *data = NULL;
    active_win = None;
    if (A_ACTIVE == None) return;
    if (XGetWindowProperty(dpy, root, A_ACTIVE, 0, 1, False, XA_WINDOW,
            &rt, &rf, &n, &extra, &data) == Success && data) {
        if (rf == 32 && n == 1) active_win = *(Window *)data;
        XFree(data);
    }
}

/* alpha cuối cùng 0..1 của 1 cửa sổ */
static double win_alpha(Win *w, long long now) {
    double a = (double)w->opacity / (double)OPAQUE;
    if (opt_fade && !w->override && w->born > 0) {
        long long dt = now - w->born;
        if (dt < 0) dt = 0;
        if (dt < opt_fade_ms) {
            double f = (double)dt / (double)opt_fade_ms;
            if (f < 0.0) f = 0.0;
            if (f > 1.0) f = 1.0;
            a *= f;
            fading = 1;
        }
    }
    if (opt_dim < 1.0 && !w->override && !w->fullscreen && !w->dock) {
        if (w->id != active_win) a *= opt_dim;
    }
    if (a < 0.0) a = 0.0;
    if (a > 1.0) a = 1.0;
    return a;
}

static int should_shadow(const Win *w) {
    if (!opt_shadow) return 0;
    if (w->override) return 0;   /* bar/tray/menu: không bóng */
    if (w->dock) return 0;
    if (w->fullscreen) return 0; /* fullscreen phủ hết: bóng vô nghĩa */
    return 1;
}

/* đảm bảo root_pict còn đúng size */
static void ensure_root_pict(void) {
    XWindowAttributes a;
    if (!XGetWindowAttributes(dpy, root, &a)) return;
    if (root_pict != None && a.width == root_pw && a.height == root_ph) return;
    if (root_pict != None) { XRenderFreePicture(dpy, root_pict); root_pict = None; }
    XRenderPictFormat *fmt = XRenderFindVisualFormat(dpy, a.visual);
    if (!fmt) { if (opt_debug) fprintf(stderr, "dani-comp: no Render fmt for root visual\n"); return; }
    root_pict = XRenderCreatePicture(dpy, root, fmt, 0, NULL);
    if (opt_debug) fprintf(stderr, "dani-comp: root_pict=0x%lx %dx%d\n",
        (unsigned long)root_pict, a.width, a.height);
    root_pw = a.width; root_ph = a.height;
}

/* vẽ bóng: 3 lớp chữ nhật đen mờ lệch xuống dưới */
static void paint_shadow(int x, int y, int w, int h) {
    static const struct { int grow; int dy; unsigned short alpha; } layers[] = {
        { 9, 4, 0x0e00 }, /* ngoài cùng, nhạt nhất */
        { 5, 3, 0x1600 },
        { 2, 2, 0x2600 }, /* sát cửa sổ, đậm nhất */
    };
    for (unsigned i = 0; i < sizeof(layers) / sizeof(layers[0]); i++) {
        XRenderColor c = { .red = 0, .green = 0, .blue = 0, .alpha = layers[i].alpha };
        Picture p = XRenderCreateSolidFill(dpy, &c);
        if (!p) continue;
        XRenderComposite(dpy, PictOpOver, p, None, root_pict,
            0, 0, 0, 0,
            x - layers[i].grow, y - layers[i].grow + layers[i].dy,
            (unsigned)(w + layers[i].grow * 2), (unsigned)(h + layers[i].grow * 2));
        XRenderFreePicture(dpy, p);
    }
}

/* gắn (lại) pixmap khi chưa có hoặc đã resize */
static int ensure_win_pict(Win *w) {
    XWindowAttributes a;
    if (!XGetWindowAttributes(dpy, w->id, &a)) return 0;
    w->x = a.x; w->y = a.y; w->bw = a.border_width;
    if (w->pict != None && a.width == w->w && a.height == w->h) return 1;
    win_free_pix(w);
    /* cửa sổ vừa unmap/map lại: pixmap cũ vô nghĩa */
    Pixmap pm = XCompositeNameWindowPixmap(dpy, w->id);
    if (!pm) return 0;
    XRenderPictFormat *fmt = XRenderFindVisualFormat(dpy, a.visual);
    if (!fmt) { XFreePixmap(dpy, pm); return 0; }
    Picture pict = XRenderCreatePicture(dpy, pm, fmt, 0, NULL);
    if (!pict) { XFreePixmap(dpy, pm); return 0; }
    w->pixmap = pm; w->pict = pict;
    w->w = a.width; w->h = a.height;
    return 1;
}

static void repaint(void) {
    Window r, p, *kids = NULL;
    unsigned nk = 0;
    long long now = now_ms();
    fading = 0;
    ensure_root_pict();
    if (root_pict == None) return;

    /* nền: trả root về background (wallpaper feh đặt ở root bg).
     * Chỉ khi đổi geometry/visibility (di chuyển/unmap/resize) mới cần —
     * gõ chữ (pure content damage) vẽ đè cùng vị trí là đúng, khỏi xóa
     * vừa nhanh vừa hết nháy. Dùng exposures=False để khỏi tự sinh
     * Expose -> vòng lặp repaint vô hạn (bug lag 100% CPU). */
    if (!XQueryTree(dpy, root, &r, &p, &kids, &nk)) return;
    /* Pre-pass: đối chiếu state với server trước khi clear/vẽ (xem chú thích
     * ở vòng vẽ). Flip ở đây thì clear ngay trong paint này, không trễ. */
    for (unsigned i = 0; i < nk; i++) {
        if (kids[i] == cm_win) continue;
        Win *w = win_get(kids[i]);
        if (!w) continue; /* cửa sổ lạ: vòng vẽ lo (win_add) */
        XWindowAttributes ra;
        int vs = 0;
        if (XGetWindowAttributes(dpy, w->id, &ra))
            vs = (ra.map_state == IsViewable ||
                  (ra.override_redirect && ra.map_state != IsUnmapped));
        if (vs != w->mapped) {
            win_free_pix(w);
            need_clear = 1;
            if (vs) w->born = now_ms(); /* remap: fade lại từ đầu */
            w->mapped = vs;
            w->opacity = read_opacity(w->id);
            w->fullscreen = has_atom(w->id, A_WM_STATE, A_WM_STATE_FS);
            w->dock = has_atom(w->id, A_WM_WINTYPE, A_WM_WINTYPE_DOCK);
        }
    }

    if (need_clear) {
        fprintf(stderr, "COMP: XClearArea running (need_clear=1)\n");
        XClearArea(dpy, root, 0, 0, 0, 0, False);
        need_clear = 0;
        fprintf(stderr, "COMP: XClearArea done\n");
    } else {
        fprintf(stderr, "COMP: NO CLEAR (need_clear=0)\n");
    }

    if (opt_debug >= 3 && !getenv("DANI_COMP_BARONLY")) {
        /* chẩn đoán: chỉ clear, không vẽ gì. Ghost mất -> composite loop
         * vẽ bậy; ghost còn -> server tự hiển thị (vấn đề tầng X). */
        if (kids) XFree(kids);
        XFlush(dpy);
        last_paint = now;
        dirty = 0;
        return;
    }
    /* kids[0] = dưới cùng -> vẽ từ dưới lên trên.
     * mapped đã tươi sau pre-pass: tin luôn, khỏi hỏi server lần nữa. */
    for (unsigned i = 0; i < nk; i++) {
        Window id = kids[i];
        if (id == cm_win) continue;
        Win *w = win_get(id);
        if (!w) {
            /* cửa sổ mới xuất hiện giữa 2 lần query (chưa qua MapNotify) */
            XWindowAttributes a;
            if (!XGetWindowAttributes(dpy, id, &a)) continue;
            if (a.override_redirect == False && a.map_state != IsViewable) continue;
            /* chỉ composite top-level viewable hoặc override (bar/menu) */
            if (a.map_state != IsViewable && !a.override_redirect) continue;
            w = win_add(id);
            if (!w) continue;
        }
        if (!w->mapped) continue; /* đã tươi sau pre-pass */
        if (!ensure_win_pict(w)) continue;
        int dx = w->x, dy = w->y;
        int dw = w->w, dh = w->h;
        if (dw <= 0 || dh <= 0) continue;

        double a = win_alpha(w, now);
        if (a <= 0.0) continue;
        if (opt_debug >= 2)
            fprintf(stderr, "dani-comp: draw 0x%lx at %d,%d %dx%d a=%.2f clear=%d\n",
                (unsigned long)w->id, dx, dy, dw, dh, a, need_clear);
        if (should_shadow(w) && a > 0.05) paint_shadow(dx, dy, dw, dh);
        if (a >= 0.999) {
            XRenderComposite(dpy, PictOpOver, w->pict, None, root_pict,
                0, 0, 0, 0, dx, dy, (unsigned)dw, (unsigned)dh);
        } else {
            XRenderColor mc = { .red = 0, .green = 0, .blue = 0,
                .alpha = (unsigned short)(a * 65535.0) };
            Picture mask = XRenderCreateSolidFill(dpy, &mc);
            if (!mask) continue;
            XRenderComposite(dpy, PictOpOver, w->pict, mask, root_pict,
                0, 0, 0, 0, dx, dy, (unsigned)dw, (unsigned)dh);
            XRenderFreePicture(dpy, mask);
        }
    }
    if (kids) XFree(kids);
    if (opt_debug) {
        /* XSync ép server vẽ xong mới về: đo latency thật (XFlush chỉ xếp hàng) */
        long long t0 = now_ms();
        XSync(dpy, False);
        long long dt = now_ms() - t0;
        if (dt > 25 || (n_paint % 10) == 0)
            fprintf(stderr, "dani-comp: paint#%ld took %lldms\n", n_paint + 1, dt);
    } else {
        XFlush(dpy);
    }
    last_paint = now;
    dirty = 0;
    n_paint++;
    if (opt_debug >= 2) {
        /* liệt kê ai bị vẽ: bắt quả tang ghost */
        Window r2, p2, *k2 = NULL;
        unsigned n2 = 0;
        if (XQueryTree(dpy, root, &r2, &p2, &k2, &n2)) {
            for (unsigned i = 0; i < n2; i++) {
                Win *w = win_get(k2[i]);
                XWindowAttributes a;
                int vs = (XGetWindowAttributes(dpy, k2[i], &a) && a.map_state == IsViewable);
                fprintf(stderr, "dani-comp:   kid 0x%lx tracked=%d cmapped=%d server_vs=%d\n",
                    (unsigned long)k2[i], w != NULL, w ? w->mapped : -1, vs);
            }
            if (k2) XFree(k2);
        }
    }
}

static int xerror_ignore(Display *d, XErrorEvent *e) {
    (void)d;
    if (opt_debug) {
        char msg[128] = "";
        XGetErrorText(dpy, e->error_code, msg, sizeof(msg));
        fprintf(stderr, "dani-comp: Xerror %s req=%d/%d res=0x%lx\n",
            msg, (int)e->request_code, (int)e->minor_code, (unsigned long)e->resourceid);
    }
    return 0;
}

static void usage(const char *prog) {
    fprintf(stderr,
        "dani-comp — compositor đơn giản cho daniwm\n"
        "usage: %s [-d display] [--shadow/--no-shadow] [--fade/--no-fade]\n"
        "         [--dim 0.5..1] [--fade-ms N] [-v] [-h]\n"
        "  --dim 0.92  độ sáng cửa sổ inactive (1 = tắt dim)\n", prog);
}

int main(int argc, char **argv) {
    const char *dpyname = NULL;
    if (getenv("DANI_COMP_DEBUG")) {
        opt_debug = atoi(getenv("DANI_COMP_DEBUG"));
        if (opt_debug < 1) opt_debug = 1;
        setvbuf(stderr, NULL, _IONBF, 0); /* kill -9 cũng còn log */
    }
    for (int i = 1; i < argc; i++) {
        if ((!strcmp(argv[i], "-d") || !strcmp(argv[i], "-display")) && i + 1 < argc) {
            dpyname = argv[++i];
        } else if (!strcmp(argv[i], "--no-shadow") || !strcmp(argv[i], "-s-")) {
            opt_shadow = 0;
        } else if (!strcmp(argv[i], "--shadow") || !strcmp(argv[i], "-s")) {
            opt_shadow = 1;
        } else if (!strcmp(argv[i], "--no-fade") || !strcmp(argv[i], "-f-")) {
            opt_fade = 0;
        } else if (!strcmp(argv[i], "--fade") || !strcmp(argv[i], "-f")) {
            opt_fade = 1;
        } else if (!strcmp(argv[i], "--dim") && i + 1 < argc) {
            opt_dim = strtod(argv[++i], NULL);
            if (opt_dim < 0.5) opt_dim = 0.5;
            if (opt_dim > 1.0) opt_dim = 1.0;
        } else if ((!strcmp(argv[i], "--fade-ms") || !strcmp(argv[i], "--fade-time")) && i + 1 < argc) {
            opt_fade_ms = atoi(argv[++i]);
            if (opt_fade_ms < 0) opt_fade_ms = 0;
            if (opt_fade_ms > 1000) opt_fade_ms = 1000;
        } else if (!strcmp(argv[i], "-v") || !strcmp(argv[i], "--verbose")) {
            opt_verbose = 1;
        } else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
            usage(argv[0]);
            return 0;
        } else {
            fprintf(stderr, "dani-comp: unknown arg '%s'\n", argv[i]);
            usage(argv[0]);
            return 1;
        }
    }

    dpy = XOpenDisplay(dpyname);
    if (!dpy) { fprintf(stderr, "dani-comp: cannot open display\n"); return 1; }
    screen = DefaultScreen(dpy);
    root = RootWindow(dpy, screen);
    sw = DisplayWidth(dpy, screen);
    sh = DisplayHeight(dpy, screen);
    (void)sw; (void)sh;
    xfd = ConnectionNumber(dpy);

    /* extension bắt buộc */
    {
        int ev, err;
        if (!XCompositeQueryExtension(dpy, &ev, &err)) {
            fprintf(stderr, "dani-comp: Composite extension missing\n");
            return 1;
        }
        if (!XDamageQueryExtension(dpy, &dmg_event, &dmg_error)) {
            fprintf(stderr, "dani-comp: Damage extension missing\n");
            return 1;
        }
        {
            int ev2, err2;
            if (!XRenderQueryExtension(dpy, &ev2, &err2)) {
                fprintf(stderr, "dani-comp: Render extension missing\n");
                return 1;
            }
        }
    }
    {
        int major = 0, minor = 0;
        /* cần Manual redirect (Composite >= 0.3) */
        if (!XCompositeQueryVersion(dpy, &major, &minor)) {
            fprintf(stderr, "dani-comp: Composite version query failed\n");
            return 1;
        }
        if (major == 0 && minor < 3) {
            fprintf(stderr, "dani-comp: Composite %d.%d too old (need >= 0.3)\n", major, minor);
            return 1;
        }
    }

    A_OPACITY = XInternAtom(dpy, "_NET_WM_WINDOW_OPACITY", False);
    A_ACTIVE = XInternAtom(dpy, "_NET_ACTIVE_WINDOW", False);
    A_WM_STATE = XInternAtom(dpy, "_NET_WM_STATE", False);
    A_WM_STATE_FS = XInternAtom(dpy, "_NET_WM_STATE_FULLSCREEN", False);
    A_WM_WINTYPE = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE", False);
    A_WM_WINTYPE_DOCK = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE_DOCK", False);
    {
        char cm[32];
        snprintf(cm, sizeof(cm), "_NET_WM_CM_S%d", screen);
        A_CM = XInternAtom(dpy, cm, False);
    }
    argb_fmt = XRenderFindStandardFormat(dpy, PictStandardARGB32);

    /* đã có compositor khác? nhường, không giẫm chân */
    if (A_CM != None && XGetSelectionOwner(dpy, A_CM) != None) {
        fprintf(stderr, "dani-comp: another compositor is already running\n");
        return 1;
    }

    XSetErrorHandler(xerror_ignore);

    /* redirect toàn bộ: cửa sổ render offscreen, mình composite tay */
    XCompositeRedirectSubwindows(dpy, root, CompositeRedirectManual);

    /* giữ selection _NET_WM_CM_Sn (EWMH compositor announcement) */
    {
        XSetWindowAttributes wa;
        memset(&wa, 0, sizeof(wa));
        wa.override_redirect = True;
        cm_win = XCreateWindow(dpy, root, -10, -10, 1, 1, 0, 0,
            InputOnly, CopyFromParent, CWOverrideRedirect, &wa);
        XSetSelectionOwner(dpy, A_CM, cm_win, CurrentTime);
        if (XGetSelectionOwner(dpy, A_CM) != cm_win) {
            fprintf(stderr, "dani-comp: cannot own %s\n", XGetAtomName(dpy, A_CM));
            return 1;
        }
    }

    XSelectInput(dpy, root,
        SubstructureNotifyMask | PropertyChangeMask | StructureNotifyMask | ExposureMask);

    /* quản lý cửa sổ đang có */
    {
        Window r, p, *kids = NULL;
        unsigned nk = 0;
        if (XQueryTree(dpy, root, &r, &p, &kids, &nk)) {
            for (unsigned i = 0; i < nk; i++) {
                if (kids[i] == cm_win) continue;
                XWindowAttributes a;
                if (!XGetWindowAttributes(dpy, kids[i], &a)) continue;
                if (a.override_redirect) { win_add(kids[i]); continue; }
                if (a.map_state == IsViewable) win_add(kids[i]);
            }
            if (kids) XFree(kids);
        }
    }
    read_active();
    if (opt_verbose)
        fprintf(stderr, "dani-comp: shadow=%d fade=%d dim=%.2f fade_ms=%d\n",
            opt_shadow, opt_fade, opt_dim, opt_fade_ms);

    signal(SIGTERM, on_quit);
    signal(SIGINT, on_quit);
    repaint();

    for (;;) {
        if (quit_req) {
            /* trả redirect về cho server: không thì cửa sổ đông cứng
             * offscreen sau khi compositor chết. */
            XCompositeUnredirectSubwindows(dpy, root, CompositeRedirectManual);
            XFlush(dpy);
            return 0;
        }
        /* nhịp anim: đang fade thì tick 16ms, không thì block tới event */
        struct timeval tv, *tvp = NULL;
        struct timeval tick = { .tv_sec = 0, .tv_usec = 16000 };
        if (fading || dirty) {
            long long now = now_ms();
            long long elapsed = now - last_paint;
            if (elapsed < 16 && !dirty) {
                tick.tv_usec = (long)(16000 - elapsed * 1000);
                tv = tick; tvp = &tv;
            } else {
                repaint();
                continue;
            }
        }
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(xfd, &rfds);
        int ret = select(xfd + 1, &rfds, NULL, NULL, tvp);
        if (ret < 0) {
            if (errno == EINTR) continue;
            fprintf(stderr, "dani-comp: select: %s\n", strerror(errno));
            return 1;
        }
        if (ret == 0) { repaint(); continue; } /* tick anim */
        while (XPending(dpy)) {
            XEvent ev;
            XNextEvent(dpy, &ev);
            if (ev.type == dmg_event + XDamageNotify) {
                XDamageNotifyEvent *e = (XDamageNotifyEvent *)&ev;
                /* ReportNonEmpty: phải subtract để server báo tiếp lần sau.
                 * Thiếu dòng này là bug "gõ chữ không thấy chữ" — damage
                 * kẹt non-empty, server im luôn sau event đầu tiên. */
                Win *dw = NULL;
                for (Win *t = wins; t; t = t->next)
                    if (t->damage == e->damage) { dw = t; break; }
                if (dw)
                    XDamageSubtract(dpy, dw->damage, None, None);
                dirty = 1; /* content đổi, geometry giữ -> khỏi clear nền */
                n_damage++;
                if (opt_debug && (n_damage % 10) == 1)
                    fprintf(stderr, "dani-comp: damage#%ld drawable=0x%lx\n",
                        n_damage, (unsigned long)e->drawable);
            } else switch (ev.type) {
            case MapNotify: {
                XMapEvent *e = &ev.xmap;
                if (e->window == cm_win) break;
                if (opt_debug) fprintf(stderr, "dani-comp: Map 0x%lx\n", (unsigned long)e->window);
                Win *w = win_add(e->window);
                if (w) { w->born = now_ms(); win_free_pix(w); }
                dirty = 1; need_clear = 1;
                break;
            }
            case UnmapNotify: {
                XUnmapEvent *e = &ev.xunmap;
                if (opt_debug) fprintf(stderr, "dani-comp: Unmap 0x%lx tracked=%d\n",
                    (unsigned long)e->window, win_get(e->window) != NULL);
                Win *w = win_get(e->window);
                if (w) { w->mapped = 0; win_free_pix(w); }
                dirty = 1; need_clear = 1;
                break;
            }
            case DestroyNotify: {
                win_del(ev.xdestroywindow.window);
                dirty = 1; need_clear = 1;
                break;
            }
            case ReparentNotify: {
                XReparentEvent *e = &ev.xreparent;
                /* daniwm không reparent, nhưng transients có thể */
                Win *w = win_get(e->window);
                if (e->parent == root) {
                    if (!w) win_add(e->window);
                    else win_refresh_flags(w);
                } else if (w) {
                    win_del(e->window);
                }
                dirty = 1; need_clear = 1;
                break;
            }
            case ConfigureNotify: {
                XConfigureEvent *e = &ev.xconfigure;
                if (e->window == root) {
                    if (root_pict != None) { XRenderFreePicture(dpy, root_pict); root_pict = None; }
                } else {
                    Win *w = win_get(e->window);
                    if (w) { win_refresh_flags(w); win_free_pix(w); }
                }
                dirty = 1; need_clear = 1;
                break;
            }
            case CirculateNotify:
                dirty = 1; need_clear = 1; /* đổi stacking -> vẽ lại theo order mới */
                break;
            case PropertyNotify: {
                XPropertyEvent *e = &ev.xproperty;
                if (e->window == root && e->atom == A_ACTIVE) {
                    read_active();
                    dirty = 1;
                } else if (e->atom == A_OPACITY || e->atom == A_WM_STATE || e->atom == A_WM_WINTYPE) {
                    Win *w = win_get(e->window);
                    if (w) win_refresh_flags(w);
                    dirty = 1;
                }
                break;
            }
            case Expose:
                if (ev.xexpose.window == root) dirty = 1;
                break;
            case SelectionClear:
                if (ev.xselectionclear.selection == A_CM) {
                    fprintf(stderr, "dani-comp: replaced, exiting\n");
                    XCompositeUnredirectSubwindows(dpy, root, CompositeRedirectManual);
                    XFlush(dpy);
                    return 0;
                }
                break;
            default:
                break;
            }
        }
        /* vòng drain ở trên đã ăn hết event đang queued (gom burst damage
         * tự nhiên) nên vẽ luôn 1 lần — khỏi delay 8ms gây trễ phím. */
        if (dirty) repaint();
    }
    return 0;
}
