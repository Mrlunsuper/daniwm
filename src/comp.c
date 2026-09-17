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
 *   - double-buffered: dựng frame trên pixmap offscreen rồi present bằng
 *     1 lần XCopyArea (không clear trắng root giữa frame -> hết chớp khi
 *     menu chuột phải map/unmap). Nền (wallpaper) snapshot từ _XROOTPMAP_ID
 *     1 lần rồi reuse; chụp lại khi resize root / wallpaper đổi / Expose
 *     ngoài. Không có _XROOTPMAP_ID (xsetroot màu trơn) thì fallback clear
 *     1 frame lúc khởi động/đổi nền — không ảnh hưởng menu.
 *   - shaped window (bounding shape): hỗ trợ qua XShape — clip phía đích
 *     (XRenderSetPictureClipRectangles trên back buffer; XFixes clip bị
 *     Xvfb bỏ qua với pixmap redirect). Bóng cửa sổ shaped bị tắt.
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
#include <X11/extensions/Xfixes.h>
#include <X11/extensions/shape.h>
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
static Atom A_XROOTPMAP;  /* _XROOTPMAP_ID (feh đổi wallpaper) */
static Atom A_XSETROOT;   /* _XSETROOT_ID */

/* options */
static int opt_shadow = 1;
static int opt_fade = 1;
static double opt_dim = 0.92; /* 1.0 = tắt dim */
static int opt_fade_ms = 160;
static int opt_verbose = 0;

/* damage ext */
static int dmg_event = 0, dmg_error = 0;

/* shape ext (bounding shape cho shaped window) */
static int shape_ok = 0, shape_event = 0;

/* render: double-buffer — dựng frame trên back_buf rồi present 1 blit.
 * Visible root không bao giờ bị clear giữa frame nên hết chớp. */
static Picture back_pict = None;
static Pixmap back_buf = None;
static Pixmap bg_pix = None; /* snapshot nền root (wallpaper), chụp lại khi cần */
static int back_w = 0, back_h = 0;
static GC present_gc = None;
static int bg_dirty = 1; /* cần chụp lại nền */

/* P1: clip đích hiện tại của frame = hộp bao damage (toạ độ root). Khi
 * cur_clip_on, mọi thao tác vẽ giới hạn trong hộp này; các clip tạm (bóng
 * P5, shaped) giao với hộp rồi trả về hộp, không trả về None. */
static XRectangle cur_clip;      /* hộp bao damage của frame content-only */
static int cur_clip_on = 0;      /* 1 = đang clip theo hộp damage */

/* P1: gom hộp bao damage (toạ độ root) giữa các frame. Frame content-only
 * chỉ vẽ lại hộp này thay vì cả màn hình -> hết scale theo diện tích màn. */
static int dmg_x1, dmg_y1, dmg_x2, dmg_y2;
static int dmg_have = 0;
static int full_dirty = 1;       /* frame tới phải vẽ toàn màn (không dùng hộp) */
static XserverRegion dmg_scratch = None; /* vùng tạm đọc rect damage */

static void dmg_add(int x1, int y1, int x2, int y2) {
    if (!dmg_have) {
        dmg_x1 = x1; dmg_y1 = y1; dmg_x2 = x2; dmg_y2 = y2;
        dmg_have = 1;
    } else {
        if (x1 < dmg_x1) dmg_x1 = x1;
        if (y1 < dmg_y1) dmg_y1 = y1;
        if (x2 > dmg_x2) dmg_x2 = x2;
        if (y2 > dmg_y2) dmg_y2 = y2;
    }
}

/* buffer clip dùng lại giữa các frame (khỏi malloc/free mỗi cửa sổ shaped) */
static XRectangle *clipbuf = NULL;
static int clipbuf_cap = 0;
static XRectangle *clipbuf_ensure(int n) {
    if (n <= clipbuf_cap) return clipbuf;
    XRectangle *nb = realloc(clipbuf, (size_t)n * sizeof(XRectangle));
    if (!nb) return NULL;
    clipbuf = nb; clipbuf_cap = n;
    return clipbuf;
}

/* đặt clip đích về hộp damage (hoặc bỏ clip nếu full-frame).
 * Lưu ý: SetPictureClipRectangles với n=0 = clip RỖNG (không vẽ gì), nên bỏ
 * clip phải dùng ChangePicture clip_mask=None, không phải mảng rỗng. */
static void back_clip_reset(void) {
    if (cur_clip_on) {
        XRenderSetPictureClipRectangles(dpy, back_pict, 0, 0, &cur_clip, 1);
    } else {
        XRenderPictureAttributes pa;
        pa.clip_mask = None;
        XRenderChangePicture(dpy, back_pict, CPClipMask, &pa);
    }
}

/* giao hộp a với hộp b -> out. Trả 0 nếu rỗng. */
static int rect_intersect(const XRectangle *a, const XRectangle *b, XRectangle *out) {
    int ax1 = a->x, ay1 = a->y, ax2 = a->x + a->width, ay2 = a->y + a->height;
    int bx1 = b->x, by1 = b->y, bx2 = b->x + b->width, by2 = b->y + b->height;
    int x1 = ax1 > bx1 ? ax1 : bx1, y1 = ay1 > by1 ? ay1 : by1;
    int x2 = ax2 < bx2 ? ax2 : bx2, y2 = ay2 < by2 ? ay2 : by2;
    if (x2 <= x1 || y2 <= y1) return 0;
    out->x = (short)x1; out->y = (short)y1;
    out->width = (unsigned short)(x2 - x1); out->height = (unsigned short)(y2 - y1);
    return 1;
}

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
    int bw, bh;          /* border width/height (pixmap gồm cả border) */
    int depth;           /* depth X (32 = ARGB, có alpha từng pixel) */
    int mapped;          /* đang viewable */
    int override;        /* override_redirect (bar/tray/menu...) */
    int fullscreen;
    int dock;
    unsigned opacity;    /* _NET_WM_WINDOW_OPACITY, mặc định ~0U */
    long long born;      /* ms, để fade-in */
    XRectangle *clip_rects; /* bounding shape (window coords), NULL nếu không shaped */
    int n_clip;
    int shaped;
    int shape_checked;   /* P6: số lần đã recheck shape sau map (0,1,2 = xong) */
    Win *next;
};
static Win *wins;
static Window active_win = None;
static int dirty = 1;       /* cần repaint */
static int stack_dirty = 1; /* geometry/stacking đổi: cần pre-pass + re-query root */
/* cache để khỏi round-trip/alloc mỗi frame */
static Picture shadow_fill[3] = { None, None, None }; /* solid fill 3 lớp bóng */
/* mask alpha: các cửa sổ khác nhau có alpha khác nhau (active/inactive/
 * opacity riêng) nên cache 4 giá trị gần nhất thay vì 1. */
#define ALPHA_SLOTS 4
static Picture alpha_slot[ALPHA_SLOTS] = { None, None, None, None };
static double alpha_val[ALPHA_SLOTS] = { -1.0, -1.0, -1.0, -1.0 };
static unsigned alpha_next = 0;
/* cache stacking order: frame content-only reuse, khỏi QueryTree mỗi frame */
static Window *stack_cache = NULL;
static unsigned stack_nk = 0;
static XWindowAttributes root_cache; /* attrs root cho frame content-only */
static int have_root = 0;
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
    if (w->clip_rects) { XFree(w->clip_rects); w->clip_rects = NULL; }
    w->n_clip = 0;
    w->shaped = 0;
}

/* đọc bounding shape của cửa sổ -> danh sách rect (toạ độ window, 0,0 = góc
 * ngoài gồm border). Trả 1 nếu cửa sổ thực sự shaped. Render clip XFixes
 * (SetPictureClipRegion) bị Xvfb bỏ qua trên picture pixmap redirect, nên
 * dùng XRenderSetPictureClipRectangles trên picture ĐÍCH (root) lúc vẽ. */
static int read_shape(Win *w) {
    if (w->clip_rects) { XFree(w->clip_rects); w->clip_rects = NULL; }
    w->n_clip = 0;
    w->shaped = 0;
    if (!shape_ok) return 0;
    if (!w->w || !w->h) return 0; /* ensure_win_pict gọi sau khi biết size */
    int n = 0, ord = 0;
    XRectangle *rects = XShapeGetRectangles(dpy, w->id, ShapeBounding, &n, &ord);
    if (!rects || n <= 0) { if (rects) XFree(rects); return 0; }
    /* bao thủ: 1 rect chi coi la shaped khi ro rang nho hon footprint.
     * w/h từ attrs có thể stale so với rects (race resize) — so area
     * tuyệt đối tránh false-shaped (clip stale làm mất nội dung). */
    if (n == 1) {
        long long rect_area = (long long)rects[0].width * rects[0].height;
        long long full_area = (long long)(w->w + 2 * w->bw) * (w->h + 2 * w->bh);
        if (rect_area * 2 >= full_area) { XFree(rects); return 0; }
    }
    w->clip_rects = rects; /* giữ nguyên để dịch lúc vẽ */
    w->n_clip = n;
    w->shaped = 1;
    if (opt_debug) {
        fprintf(stderr, "dani-comp: shaped 0x%lx rects=%d first=%d,%d %ux%u\n",
            (unsigned long)w->id, n, rects[0].x, rects[0].y,
            rects[0].width, rects[0].height);
    }
    return 1;
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
    w->x = a.x; w->y = a.y; w->bw = a.border_width; w->bh = a.border_width;
    w->depth = a.depth;
    w->opacity = read_opacity(w->id);
    w->fullscreen = has_atom(w->id, A_WM_STATE, A_WM_STATE_FS);
    w->dock = has_atom(w->id, A_WM_WINTYPE, A_WM_WINTYPE_DOCK);
}

static Win *win_add(Window id) {
    Win *w = win_get(id);
    if (w) { win_refresh_flags(w); return w; }
    /* Kiểm tra window còn sống TRƯỚC khi calloc/XDamageCreate: cửa sổ
     * tạo rồi hủy nhanh (dialog Save của Chrome...) có thể đã chết lúc
     * MapNotify/repaint phát hiện. Không check = zombie entry + Damage
     * rò rỉ server-side, save càng nhiều càng chậm (restart hết vì
     * server free resource của client đã disconnect). */
    {
        XWindowAttributes chk;
        if (!XGetWindowAttributes(dpy, id, &chk)) return NULL;
        /* InputOnly (vd window clipboard ẩn của Chrome) không bao giờ có
         * pixmap: track nó chỉ tổ tốn Damage object + BadMatch noise. */
        if (chk.class == InputOnly || chk.depth == 0) return NULL;
    }
    w = calloc(1, sizeof(*w));
    if (!w) return NULL;
    w->id = id;
    w->opacity = OPAQUE;
    w->born = now_ms();
    w->next = wins;
    wins = w;
    /* theo dõi damage + prop của cửa sổ này */
    XSelectInput(dpy, id, PropertyChangeMask | StructureNotifyMask);
    if (shape_ok)
        XShapeSelectInput(dpy, id, ShapeNotifyMask); /* bounding shape đổi */
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

/* Quét rác định kỳ: xóa entry của window đã chết mà DestroyNotify bị
 * sót (chết trước khi select, XID recycle...). Chạy tối đa 1 lần/5s,
 * mỗi entry tốn 1 round-trip GetWindowAttributes — vài chục window =
 * không đáng kể. Chỉ log khi DANI_COMP_DEBUG. */
static long long last_gc = 0;
static void gc_sweep(long long now) {
    if (now - last_gc < 5000) return;
    last_gc = now;
    XWindowAttributes chk;
    Win **p = &wins;
    int n = 0, dead = 0;
    while (*p) {
        Win *w = *p;
        n++;
        if (!XGetWindowAttributes(dpy, w->id, &chk)) {
            *p = w->next;
            if (w->damage) XDamageDestroy(dpy, w->damage);
            win_free_pix(w);
            free(w);
            dead++;
            continue;
        }
        p = &(*p)->next;
    }
    if (opt_debug)
        fprintf(stderr, "dani-comp: gc tracked=%d dead=%d\n", n, dead);
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
    if (a < 0.0) a = 0.0;
    if (a > 1.0) a = 1.0;
    return a;
}

/* Hệ số dim của cửa sổ nền. 1.0 = không dim. Dim KHÔNG làm bằng alpha:
 * giảm alpha thì cửa sổ dưới hiện xuyên qua (hộp thoại Save as nhìn thấy
 * trang web phía sau). Vẽ cửa sổ đủ đục rồi phủ đen mới đúng. */
static double win_dim(const Win *w) {
    if (opt_dim >= 1.0) return 1.0;
    if (w->override || w->fullscreen || w->dock) return 1.0;
    if (w->id == active_win) return 1.0;
    return opt_dim;
}

static int should_shadow(const Win *w) {
    if (!opt_shadow) return 0;
    if (w->override) return 0;   /* bar/tray/menu: không bóng */
    if (w->dock) return 0;
    if (w->shaped) return 0;     /* bóng chữ nhật quanh hình tròn: xấu */
    if (w->fullscreen) return 0; /* fullscreen phủ hết: bóng vô nghĩa */
    return 1;
}

/* P4: cửa sổ vẽ HOÀN TOÀN đục (alpha ~1, không shape) -> có thể che khuất
 * mọi thứ dưới nó. Tính thuần, không đụng cờ fading (khác win_alpha).
 * Lưu ý: chỉ xét _NET_WM_WINDOW_OPACITY là chưa đủ — terminal trong suốt
 * (alacritty opacity=0.65) dùng visual ARGB depth 32, alpha nằm từng pixel
 * mà property vẫn OPAQUE. Coi depth 32 là chưa đục: hướng an toàn (chỉ tốn
 * 1 blit nền, không sai hình). */
static int win_is_opaque(const Win *w, long long now) {
    if (w->depth == 32) return 0; /* ARGB: trong suốt từng pixel có thể */
    if (w->opacity != OPAQUE) return 0;
    if (w->shaped) return 0;
    if (opt_fade && !w->override && w->born > 0 && now - w->born < opt_fade_ms)
        return 0; /* đang fade-in */
    return 1;
}

/* đảm bảo back buffer + snapshot nền đúng size root hiện tại */
static void ensure_targets(int w, int h, Visual *vis, int depth) {
    if (back_buf != None && w == back_w && h == back_h) return;
    if (back_pict != None) { XRenderFreePicture(dpy, back_pict); back_pict = None; }
    if (back_buf != None) { XFreePixmap(dpy, back_buf); back_buf = None; }
    if (bg_pix != None) { XFreePixmap(dpy, bg_pix); bg_pix = None; }
    back_w = back_h = 0;
    if (w <= 0 || h <= 0 || !vis) return;
    back_buf = XCreatePixmap(dpy, root, (unsigned)w, (unsigned)h, (unsigned)depth);
    bg_pix = XCreatePixmap(dpy, root, (unsigned)w, (unsigned)h, (unsigned)depth);
    if (back_buf == None || bg_pix == None) {
        if (back_buf != None) { XFreePixmap(dpy, back_buf); back_buf = None; }
        if (bg_pix != None) { XFreePixmap(dpy, bg_pix); bg_pix = None; }
        return;
    }
    XRenderPictFormat *fmt = XRenderFindVisualFormat(dpy, vis);
    if (fmt)
        back_pict = XRenderCreatePicture(dpy, back_buf, fmt, 0, NULL);
    if (back_pict == None) {
        XFreePixmap(dpy, back_buf); back_buf = None;
        XFreePixmap(dpy, bg_pix); bg_pix = None;
        return;
    }
    if (opt_debug) fprintf(stderr, "dani-comp: backbuf %dx%d\n", w, h);
    back_w = w; back_h = h;
    bg_dirty = 1; /* buffer mới: nền chưa có */
}

/* Thử snapshot nền mà không chạm vào root visible: copy trực tiếp từ
 * pixmap wallpaper (_XROOTPMAP_ID do feh/nitrogen đặt). Không gây chớp.
 * Trả 1 nếu copy được. */
static int capture_from_rootmap(int w, int h, int depth) {
    Atom rt; int rf; unsigned long n = 0, extra = 0;
    unsigned char *data = NULL;
    Pixmap src = None;
    Window rr; int x, y; unsigned bw, dep, sw2, sh2;
    if (A_XROOTPMAP == None) return 0;
    if (XGetWindowProperty(dpy, root, A_XROOTPMAP, 0, 1, False, XA_PIXMAP,
            &rt, &rf, &n, &extra, &data) != Success || !data)
        return 0;
    if (rt != XA_PIXMAP || rf != 32 || n != 1) { XFree(data); return 0; }
    /* data là CARD32 pixmap id: tránh alias Pixmap (unsigned long 64-bit).
     * Copy ra biến local rồi free ngay để khỏi use-after-free. */
    { unsigned long id = *(unsigned long *)data; src = (Pixmap)id; }
    XFree(data);
    data = NULL;
    if (src == None) return 0;
    /* pixmap chết (feh thoát sau khi set?) hoặc nhỏ hơn màn hình
     * thì bỏ, dùng fallback clear. */
    if (!XGetGeometry(dpy, src, &rr, &x, &y, &sw2, &sh2, &bw, &dep))
        return 0;
    if ((int)sw2 < w || (int)sh2 < h) return 0;
    if ((int)dep != depth) return 0; /* depth lệch: fallback clear cho an toàn */
    XCopyArea(dpy, src, bg_pix, present_gc,
        0, 0, (unsigned)w, (unsigned)h, 0, 0);
    XSync(dpy, False); /* hiếm (chỉ khi nền đổi) nên sync cho chắc ăn */
    return 1;
}

/* chụp nền root (wallpaper) vào bg_pix. Chỉ gọi khi bg_dirty — resize,
 * wallpaper đổi, Expose ngoài. Repaint thường reuse snapshot nên không
 * bao giờ clear root thật -> hết chớp menu chuột phải. */
static void recapture_background(int w, int h, int depth) {
    if (capture_from_rootmap(w, h, depth)) { bg_dirty = 0; return; }
    /* fallback: không có wallpaper pixmap (xsetroot -solid...): clear root
     * thật rồi copy. Chớp 1 frame nhưng chỉ khi nền đổi hoặc lần đầu
     * khởi động, không phải mỗi lần mở menu. */
    XClearArea(dpy, root, 0, 0, 0, 0, False);
    XCopyArea(dpy, root, bg_pix, present_gc,
        0, 0, (unsigned)w, (unsigned)h, 0, 0);
    XSync(dpy, False);
    bg_dirty = 0;
}

/* vẽ bóng: 3 lớp chữ nhật đen mờ lệch xuống dưới. Solid fill tạo 1 lần
 * rồi reuse (trước đây create/free 3 cái mỗi cửa sổ mỗi frame). */
static void paint_shadow(int x, int y, int w, int h) {
    static const struct { int grow; int dy; unsigned short alpha; } layers[] = {
        { 9, 4, 0x0e00 }, /* ngoài cùng, nhạt nhất */
        { 5, 3, 0x1600 },
        { 2, 2, 0x2600 }, /* sát cửa sổ, đậm nhất */
    };
    /* P5: cửa sổ (mờ đục) sẽ phủ phần giữa -> chỉ vẽ vành bóng, khỏi blend
     * phần bị che (overdraw phí). Vành = bao ngoài trừ hộp cửa sổ = 4 rect. */
    int ox = x - 9, oy = y - 9 + 4, ow = w + 18, oh = h + 18; /* bao ngoài mọi lớp */
    XRectangle ring[4];
    int nr = 0;
    if (y > oy)                 ring[nr++] = (XRectangle){ (short)ox, (short)oy, (unsigned short)ow, (unsigned short)(y - oy) };
    if (oy + oh > y + h)        ring[nr++] = (XRectangle){ (short)ox, (short)(y + h), (unsigned short)ow, (unsigned short)(oy + oh - (y + h)) };
    if (x > ox)                 ring[nr++] = (XRectangle){ (short)ox, (short)y, (unsigned short)(x - ox), (unsigned short)h };
    if (ox + ow > x + w)        ring[nr++] = (XRectangle){ (short)(x + w), (short)y, (unsigned short)(ox + ow - (x + w)), (unsigned short)h };
    /* giao với hộp damage nếu đang clip theo frame content-only */
    if (cur_clip_on) {
        XRectangle clipped[4];
        int m = 0;
        for (int i = 0; i < nr; i++)
            if (rect_intersect(&ring[i], &cur_clip, &clipped[m])) m++;
        nr = m;
        for (int i = 0; i < nr; i++) ring[i] = clipped[i];
    }
    if (nr == 0) return;
    XRenderSetPictureClipRectangles(dpy, back_pict, 0, 0, ring, nr);
    for (unsigned i = 0; i < sizeof(layers) / sizeof(layers[0]); i++) {
        if (shadow_fill[i] == None) {
            XRenderColor c = { .red = 0, .green = 0, .blue = 0, .alpha = layers[i].alpha };
            shadow_fill[i] = XRenderCreateSolidFill(dpy, &c);
        }
        Picture p = shadow_fill[i];
        if (!p) continue;
        XRenderComposite(dpy, PictOpOver, p, None, back_pict,
            0, 0, 0, 0,
            x - layers[i].grow, y - layers[i].grow + layers[i].dy,
            (unsigned)(w + layers[i].grow * 2), (unsigned)(h + layers[i].grow * 2));
    }
    back_clip_reset(); /* trả clip về hộp damage (hoặc None) */
}

/* gắn (lại) pixmap khi chưa có. Còn pict là còn đúng: mọi đổi geometry
 * (Configure/Map) đều đã free pixmap ở event/pre-pass, nên khỏi
 * GetWindowAttributes mỗi cửa sổ mỗi frame (tiết kiệm n round-trip). */
static int ensure_win_pict(Win *w) {
    XWindowAttributes a;
    if (w->pict != None) return 1;
    if (!XGetWindowAttributes(dpy, w->id, &a)) return 0;
    w->x = a.x; w->y = a.y;
    w->bw = a.border_width; w->bh = a.border_width;
    w->depth = a.depth;
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
    read_shape(w); /* shape tương thích với geometry mới */
    return 1;
}

/* mask solid cho alpha a: tìm slot gần đúng (±0.003), miss thì reuse slot
 * cũ nhất. Steady state (active/inactive) hit liên tục, khỏi create/free
 * mỗi cửa sổ mỗi frame; lúc fade thì miss nhưng đó là lúc anim anyway. */
static Picture alpha_mask(double a) {
    unsigned best = 0;
    double best_dd = 2.0;
    for (unsigned i = 0; i < ALPHA_SLOTS; i++) {
        double dd = a - alpha_val[i];
        if (dd < 0) dd = -dd;
        if (dd < best_dd) { best_dd = dd; best = i; }
    }
    if (best_dd <= 0.003 && alpha_slot[best] != None) return alpha_slot[best];
    /* miss: lấy slot round-robin */
    unsigned s = alpha_next % ALPHA_SLOTS;
    alpha_next++;
    if (alpha_slot[s] != None) XRenderFreePicture(dpy, alpha_slot[s]);
    XRenderColor mc = { .red = 0, .green = 0, .blue = 0,
        .alpha = (unsigned short)(a * 65535.0) };
    alpha_slot[s] = XRenderCreateSolidFill(dpy, &mc);
    alpha_val[s] = a;
    return alpha_slot[s];
}

/* P6: shape có thể được set ngay sau map và ShapeNotify bị bỏ lỡ (race
 * register/select). Thay vì đọc lại mỗi frame trong 500ms (tốn round-trip +
 * ép 60Hz), chỉ recheck tại 2 mốc: 100ms và 400ms sau map. Trả 1 nếu có cửa
 * sổ vừa thành shaped (cần vẽ lại). Chỉ xét cửa sổ thường (born>0). */
static const long long SHAPE_THR[2] = { 100, 400 };
static int shape_poll(long long now) {
    int changed = 0;
    if (!shape_ok) return 0;
    for (Win *w = wins; w; w = w->next) {
        if (!w->mapped || w->shaped || w->born <= 0 || w->shape_checked >= 2)
            continue;
        if (w->pict == None) continue; /* chưa realize: chờ frame vẽ pixmap */
        long long age = now - w->born;
        if (age >= SHAPE_THR[w->shape_checked]) {
            if (read_shape(w)) changed = 1;
            w->shape_checked++;
        }
    }
    return changed;
}

/* ms tới lần recheck shape kế (để chốt timeout của select). -1 = không có. */
static long long shape_next_wake(long long now) {
    long long next = -1;
    if (!shape_ok) return -1;
    for (Win *w = wins; w; w = w->next) {
        if (!w->mapped || w->shaped || w->born <= 0 || w->shape_checked >= 2)
            continue;
        long long due = w->born + SHAPE_THR[w->shape_checked] - now;
        if (due < 0) due = 0;
        if (next < 0 || due < next) next = due;
    }
    return next;
}

static void repaint(void) {
    Window r, p, *kids = NULL;
    unsigned nk = 0;
    long long now = now_ms();
    XWindowAttributes ra;
    fading = 0;
    shape_poll(now); /* P6: recheck shape cửa sổ non tại mốc 100/400ms */
    /* P1: full-frame khi stack/geometry/nền/appearance đổi hoặc không có hộp
     * damage; ngược lại (gõ chữ, video) chỉ vẽ lại hộp bao damage. */
    int full = full_dirty || stack_dirty || bg_dirty || !dmg_have;
    /* frame content-only (gõ chữ/fade tick): reuse attrs root, khỏi 1 RT.
     * Resize root luôn sinh ConfigureNotify -> stack_dirty/bg_dirty nên
     * cache không bao giờ stale. */
    if (!have_root || stack_dirty || bg_dirty) {
        if (!XGetWindowAttributes(dpy, root, &ra)) return;
        root_cache = ra;
        have_root = 1;
    } else {
        ra = root_cache;
    }
    ensure_targets(ra.width, ra.height, ra.visual, ra.depth);
    if (back_pict == None) return;
    if (present_gc == None)
        present_gc = XCreateGC(dpy, root, 0, NULL);
    if (present_gc == None) return;

    /* Double-buffer: dựng frame trên back_buf (khởi đầu = snapshot nền),
     * present bằng 1 blit -> root visible không bao giờ thấy trạng thái
     * clear-trắng giữa frame -> hết chớp menu. Dùng exposures=False ở
     * recapture để khỏi tự sinh Expose -> vòng lặp vô hạn. */
    /* Stacking order: chỉ query khi stack đổi; frame content-only reuse
     * cache (tiết kiệm 1 QueryTree + alloc/free mỗi frame). */
    if (stack_dirty || !stack_cache) {
        if (!XQueryTree(dpy, root, &r, &p, &kids, &nk)) return;
        free(stack_cache);
        stack_cache = NULL; stack_nk = 0;
        if (nk > 0) {
            stack_cache = malloc(nk * sizeof(Window));
            if (stack_cache) {
                memcpy(stack_cache, kids, nk * sizeof(Window));
                stack_nk = nk;
            }
        }
    } else {
        nk = stack_nk;
        kids = stack_cache; /* mượn tạm, KHÔNG free ở cuối */
    }
    int kids_owned = (kids != stack_cache);
    /* Pre-pass: chỉ khi stack đổi (map/unmap/configure...). Frame content
     * thuần (gõ chữ/fade tick) bỏ qua: tiết kiệm n round-trip, event đã
     * giữ cache (x/y/mapped) tươi. */
    if (stack_dirty) {
        for (unsigned i = 0; i < nk; i++) {
            if (kids[i] == cm_win) continue;
            Win *w = win_get(kids[i]);
            if (!w) continue; /* cửa sổ lạ: vòng vẽ lo (win_add) */
            XWindowAttributes qa;
            int vs = 0;
            if (XGetWindowAttributes(dpy, w->id, &qa))
                vs = (qa.map_state == IsViewable ||
                      (qa.override_redirect && qa.map_state != IsUnmapped));
            if (vs != w->mapped) {
                win_free_pix(w);
                if (vs && !w->override) w->born = now_ms(); /* remap: fade lại từ đầu */
                w->mapped = vs;
                win_refresh_flags(w);
            } else if (w->pict == None) {
                /* Configure đã invalidate pixmap ở event (không refresh ở
                 * đó để bão resize chỉ tốn 1 lần/frame): refresh tại đây */
                win_refresh_flags(w);
            }
        }
        stack_dirty = 0;
    }

    if (bg_dirty)
        recapture_background(ra.width, ra.height, ra.depth);
    /* P1: chốt hộp clip cho frame content-only (kẹp trong màn hình). */
    cur_clip_on = 0;
    if (!full) {
        int x1 = dmg_x1 < 0 ? 0 : dmg_x1;
        int y1 = dmg_y1 < 0 ? 0 : dmg_y1;
        int x2 = dmg_x2 > ra.width ? ra.width : dmg_x2;
        int y2 = dmg_y2 > ra.height ? ra.height : dmg_y2;
        if (x2 > x1 && y2 > y1) {
            cur_clip.x = (short)x1; cur_clip.y = (short)y1;
            cur_clip.width = (unsigned short)(x2 - x1);
            cur_clip.height = (unsigned short)(y2 - y1);
            cur_clip_on = 1;
        } else {
            full = 1; /* hộp rỗng sau khi kẹp -> vẽ full cho chắc */
        }
    }
    back_clip_reset(); /* clip back_pict về hộp (hoặc bỏ clip nếu full) */
    /* P4: tìm cửa sổ đục trên cùng phủ TRỌN vùng vẽ (box). Có -> bỏ qua mọi
     * cửa sổ dưới nó và bỏ luôn copy nền (bị che hết). */
    XRectangle box = cur_clip_on ? cur_clip
        : (XRectangle){ 0, 0, (unsigned short)ra.width, (unsigned short)ra.height };
    unsigned start = 0;
    int occlude = 0;
    for (int i = (int)nk - 1; i >= 0; i--) {
        if (kids[i] == cm_win) continue;
        Win *ow = win_get(kids[i]);
        if (!ow || !ow->mapped || ow->pict == None) continue;
        if (!win_is_opaque(ow, now)) continue;
        int ofw = ow->w + 2 * ow->bw, ofh = ow->h + 2 * ow->bh;
        if (ow->x <= box.x && ow->y <= box.y &&
            ow->x + ofw >= box.x + box.width &&
            ow->y + ofh >= box.y + box.height) {
            start = (unsigned)i; occlude = 1; break;
        }
    }
    /* khởi đầu frame = nền (chỉ hộp damage nếu content-only; bỏ nếu bị che) */
    if (!occlude) {
        if (cur_clip_on)
            XCopyArea(dpy, bg_pix, back_buf, present_gc,
                cur_clip.x, cur_clip.y, cur_clip.width, cur_clip.height,
                cur_clip.x, cur_clip.y);
        else
            XCopyArea(dpy, bg_pix, back_buf, present_gc,
                0, 0, (unsigned)ra.width, (unsigned)ra.height, 0, 0);
    }

    if (opt_debug >= 3 && !getenv("DANI_COMP_BARONLY")) {
        /* chẩn đoán: chỉ present nền, không vẽ gì. Ghost mất ->
         * composite loop vẽ bậy; ghost còn -> server tự hiển thị. */
        XCopyArea(dpy, back_buf, root, present_gc,
            0, 0, (unsigned)ra.width, (unsigned)ra.height, 0, 0);
        if (kids_owned && kids) XFree(kids);
        XFlush(dpy);
        last_paint = now;
        dirty = 0;
        return;
    }
    /* kids[0] = dưới cùng -> vẽ từ dưới lên trên. P4: bắt đầu từ occluder
     * đục trên cùng (start) nếu có -> bỏ mọi cửa sổ bị che hoàn toàn. */
    for (unsigned i = start; i < nk; i++) {
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
        int bw2 = w->bw;

        double a = win_alpha(w, now);
        if (a <= 0.0) continue;
        int had_clip = 0;
        /* footprint ngoài gồm border: NameWindowPixmap là w+2*bw × h+2*bh,
         * attrs.x/y là góc ngoài. Vẽ đủ footprint, không chỉ interior
 * (trước đây content lệch bw px và bị crop 2*bw px). */
        int fw = dw + 2 * bw2, fh = dh + 2 * bw2;
        if (opt_debug >= 2)
            fprintf(stderr, "dani-comp: draw 0x%lx at %d,%d %dx%d a=%.2f shaped=%d\n",
                (unsigned long)w->id, dx, dy, fw, fh, a, w->shaped);
        if (should_shadow(w) && a > 0.05) paint_shadow(dx, dy, fw, fh);
        /* shaped window: clip picture ĐÍCH (back_buf) theo bounding shape
         * dịch sang toạ độ root — clip trên picture nguồn bị Xvfb bỏ qua
         * với pixmap redirect (đã đo thử; picom cũng clip phía đích). */
        if (w->shaped && w->n_clip > 0 && w->clip_rects) {
            XRectangle *clip = clipbuf_ensure(w->n_clip);
            if (clip) {
                int m = 0;
                for (int ci = 0; ci < w->n_clip; ci++) {
                    XRectangle sr = {
                        (short)(w->clip_rects[ci].x + dx),
                        (short)(w->clip_rects[ci].y + dy),
                        w->clip_rects[ci].width, w->clip_rects[ci].height
                    };
                    /* giao với hộp damage nếu đang clip frame content-only */
                    if (cur_clip_on) {
                        if (rect_intersect(&sr, &cur_clip, &clip[m])) m++;
                    } else {
                        clip[m++] = sr;
                    }
                }
                XRenderSetPictureClipRectangles(dpy, back_pict, 0, 0, clip, m);
                had_clip = 1;
            }
        }
        if (a >= 0.999) {
            XRenderComposite(dpy, PictOpOver, w->pict, None, back_pict,
                0, 0, 0, 0, dx, dy, (unsigned)fw, (unsigned)fh);
        } else {
            Picture mask = alpha_mask(a);
            if (mask) {
                XRenderComposite(dpy, PictOpOver, w->pict, mask, back_pict,
                    0, 0, 0, 0, dx, dy, (unsigned)fw, (unsigned)fh);
            }
        }
        /* dim cửa sổ nền: phủ đen alpha (1-dim) LÊN TRÊN, mask bằng chính
         * pixmap cửa sổ. Chỉ tối phần cửa sổ thật sự vẽ, và không cho cửa
         * sổ dưới hiện xuyên qua. */
        double dimf = win_dim(w);
        if (dimf < 1.0) {
            Picture blk = alpha_mask((1.0 - dimf) * a);
            if (blk)
                XRenderComposite(dpy, PictOpOver, blk, w->pict, back_pict,
                    0, 0, 0, 0, dx, dy, (unsigned)fw, (unsigned)fh);
        }
        if (had_clip)
            back_clip_reset(); /* trả clip về hộp damage (hoặc None) */
    }
    if (kids_owned && kids) XFree(kids);
    /* present bằng 1 blit: content-only chỉ blit hộp damage -> hết scale
     * theo diện tích màn; full-frame blit cả màn. */
    if (cur_clip_on)
        XCopyArea(dpy, back_buf, root, present_gc,
            cur_clip.x, cur_clip.y, cur_clip.width, cur_clip.height,
            cur_clip.x, cur_clip.y);
    else
        XCopyArea(dpy, back_buf, root, present_gc,
            0, 0, (unsigned)ra.width, (unsigned)ra.height, 0, 0);
    /* P1: đã tiêu thụ hộp damage + cờ full cho frame này */
    dmg_have = 0;
    full_dirty = 0;
    /* P6: recheck shape do vòng chính lên lịch (shape_next_wake) — không còn
     * ép 60Hz suốt 500ms cho mỗi cửa sổ mới. */
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
        /* tùy chọn: XShape cho shaped window (XFixes không cần nữa) */
        {
            int err3;
            shape_ok = XShapeQueryExtension(dpy, &shape_event, &err3);
            if (opt_verbose)
                fprintf(stderr, "dani-comp: shape=%d\n", shape_ok);
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
    A_XROOTPMAP = XInternAtom(dpy, "_XROOTPMAP_ID", False);
    A_XSETROOT = XInternAtom(dpy, "_XSETROOT_ID", False);
    {
        char cm[32];
        snprintf(cm, sizeof(cm), "_NET_WM_CM_S%d", screen);
        A_CM = XInternAtom(dpy, cm, False);
    }

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
    fprintf(stderr, "dani-comp: shadow=%d fade=%d dim=%.2f fade_ms=%d shape=%d\n",
        opt_shadow, opt_fade, opt_dim, opt_fade_ms, shape_ok);
    if (opt_verbose)
        fprintf(stderr, "dani-comp: verbose on\n");

    signal(SIGTERM, on_quit);
    signal(SIGINT, on_quit);
    repaint();

    for (;;) {
        if (quit_req) {
            /* trả redirect về cho server: không thì cửa sổ đông cứng
             * offscreen sau khi compositor chết. Xóa frame composite
             * khỏi root để server vẽ lại cửa sổ thật. */
            XCompositeUnredirectSubwindows(dpy, root, CompositeRedirectManual);
            XClearArea(dpy, root, 0, 0, 0, 0, True);
            XFlush(dpy);
            return 0;
        }
        /* P2: tối đa 1 repaint / 16ms. Đủ 16ms và có việc -> vẽ ngay. */
        if (dirty && now_ms() - last_paint >= 16) { repaint(); continue; }
        /* Lịch chờ: dirty/fading -> tick tới mốc 16ms; P6 shape recheck ->
         * tick tới mốc 100/400ms; không có gì -> block tới event. */
        struct timeval tv, *tvp = NULL;
        long budget = -1; /* us; -1 = block vô hạn */
        if (dirty || fading) {
            long long elapsed = now_ms() - last_paint;
            budget = (elapsed >= 16) ? 0 : (long)((16 - elapsed) * 1000);
        }
        long long swake = shape_next_wake(now_ms());
        if (swake >= 0) {
            long sw_us = (swake > 1000000) ? 1000000 : (long)(swake * 1000);
            if (budget < 0 || sw_us < budget) budget = sw_us;
        }
        /* GC zombie window chết sót: wake tối đa 5s để quét 1 lần. */
        {
            long long gdue = last_gc + 5000 - now_ms();
            if (gdue < 0) gdue = 0;
            long g_us = (gdue > 5000000) ? 5000000 : (long)(gdue * 1000);
            if (budget < 0 || g_us < budget) budget = g_us;
        }
        if (budget >= 0) { tv.tv_sec = 0; tv.tv_usec = budget; tvp = &tv; }
        /* C8: Xlib queue đã có event (đọc vào lúc repaint round-trip)?
         * xử lý ngay, đừng block trong select — nếu không DamageNotify chưa
         * subtract sẽ kẹt và cửa sổ đông cứng. */
        if (!XPending(dpy)) {
            fd_set rfds;
            FD_ZERO(&rfds);
            FD_SET(xfd, &rfds);
            int ret = select(xfd + 1, &rfds, NULL, NULL, tvp);
            if (ret < 0) {
                if (errno == EINTR) continue;
                fprintf(stderr, "dani-comp: select: %s\n", strerror(errno));
                return 1;
            }
            if (ret == 0) {
                /* P6: tới mốc recheck shape? đọc lại; chỉ vẽ nếu thực sự đổi
                 * hoặc đang có việc — khỏi full-repaint chỉ để poll. */
                if (shape_poll(now_ms())) dirty = 1;
                gc_sweep(now_ms());
                if (dirty || fading) repaint();
                continue;
            }
        }
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
                if (dw) {
                    /* P1: đọc vùng damage (toạ độ window-local) rồi dịch sang
                     * root + nới theo border -> gom vào hộp bao frame. */
                    if (dmg_scratch == None)
                        dmg_scratch = XFixesCreateRegion(dpy, NULL, 0);
                    XDamageSubtract(dpy, dw->damage, None, dmg_scratch);
                    int nr = 0;
                    XRectangle *rr = XFixesFetchRegion(dpy, dmg_scratch, &nr);
                    if (rr && nr > 0) {
                        int m = dw->bw + 2; /* nới: damage local có thể lệch border */
                        for (int i = 0; i < nr; i++)
                            dmg_add(dw->x + rr[i].x - m,
                                    dw->y + rr[i].y - m,
                                    dw->x + rr[i].x + rr[i].width + m,
                                    dw->y + rr[i].y + rr[i].height + m);
                    } else {
                        /* không đọc được vùng: an toàn -> vẽ full */
                        full_dirty = 1;
                    }
                    if (rr) XFree(rr);
                }
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
                /* override (bar/menu) không fade: win_add đã đặt born=0,
                 * đừng ghi đè thành now */
                if (w && !w->override) { w->born = now_ms(); win_free_pix(w); }
                else if (w) win_free_pix(w);
                dirty = 1; stack_dirty = 1;
                break;
            }
            case UnmapNotify: {
                XUnmapEvent *e = &ev.xunmap;
                if (opt_debug) fprintf(stderr, "dani-comp: Unmap 0x%lx tracked=%d\n",
                    (unsigned long)e->window, win_get(e->window) != NULL);
                Win *w = win_get(e->window);
                if (w) { w->mapped = 0; win_free_pix(w); }
                dirty = 1; stack_dirty = 1;
                break;
            }
            case DestroyNotify: {
                win_del(ev.xdestroywindow.window);
                dirty = 1; stack_dirty = 1;
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
                dirty = 1; stack_dirty = 1;
                break;
            }
            case ConfigureNotify: {
                XConfigureEvent *e = &ev.xconfigure;
                if (e->window == root) {
                    /* resize root: ensure_targets tự tạo lại buffer theo
                     * size mới ở repaint tới; đánh dấu nền phải chụp lại */
                    bg_dirty = 1;
                } else {
                    /* P3: pixmap chỉ đổi khi size/border đổi (Composite spec:
                     * storage realloc khi top-level đổi size). Move thuần giữ
                     * pixmap, chỉ cập nhật vị trí — khỏi NameWindowPixmap +
                     * RenderCreatePicture + GetRectangles mỗi frame lúc kéo. */
                    Win *w = win_get(e->window);
                    if (w) {
                        if (e->width != w->w || e->height != w->h ||
                            e->border_width != w->bw)
                            win_free_pix(w);
                        w->x = e->x; w->y = e->y;
                    }
                }
                dirty = 1; stack_dirty = 1;
                break;
            }
            case CirculateNotify:
                dirty = 1; stack_dirty = 1; /* đổi stacking -> vẽ lại theo order mới */
                break;
            case PropertyNotify: {
                XPropertyEvent *e = &ev.xproperty;
                if (e->window == root && e->atom == A_ACTIVE) {
                    read_active();
                    dirty = 1; full_dirty = 1; /* dim đổi cả 2 cửa sổ, không có damage */
                } else if (e->window == root &&
                    ((A_XROOTPMAP != None && e->atom == A_XROOTPMAP) ||
                     (A_XSETROOT != None && e->atom == A_XSETROOT))) {
                    /* feh đổi wallpaper: snapshot nền cũ đã stale */
                    bg_dirty = 1;
                    dirty = 1;
                } else if (e->atom == A_OPACITY || e->atom == A_WM_STATE || e->atom == A_WM_WINTYPE) {
                    Win *w = win_get(e->window);
                    if (w) win_refresh_flags(w);
                    dirty = 1; full_dirty = 1; /* opacity/state đổi cả cửa sổ */
                }
                break;
            }
            case Expose:
                /* Expose ngoài paint burst là sự cố: 1 phần root visible bị
                 * lật bởi thứ khác ngoài compositor (grab toàn màn hình,
                 * OSD, X ở dưới át lên...). Nếu chính blit present của
                 * mình may mắn sinh Expose thì count==0 -> bỏ qua.
                 * count>0 mới là expose thật -> coi snapshot nền stale,
                 * chụp lại (có thể tốn 1 frame clear, nhưng chỉ khi bất
                 * thường chứ không phải mỗi menu chuột phải). */
                if (ev.xexpose.window == root && ev.xexpose.count > 0) {
                    bg_dirty = 1;
                    dirty = 1;
                }
                break;
            case SelectionClear:
                if (ev.xselectionclear.selection == A_CM) {
                    fprintf(stderr, "dani-comp: replaced, exiting\n");
                    XCompositeUnredirectSubwindows(dpy, root, CompositeRedirectManual);
                    XClearArea(dpy, root, 0, 0, 0, 0, True);
                    XFlush(dpy);
                    return 0;
                }
                break;
            default:
                if (shape_ok && ev.type == shape_event + (int)ShapeNotify) {
                    /* bounding shape đổi (xeyes, menu bo góc...): đọc lại */
                    Win *w = win_get(((XShapeEvent *)&ev)->window);
                    if (w) { win_free_pix(w); read_shape(w); }
                    dirty = 1; stack_dirty = 1;
                }
                break;
            }
        }
        /* P2: throttle ở đầu vòng giữ nhịp <=60Hz. Đủ 16ms thì vẽ; chưa đủ
         * thì để dirty, vòng sau tick nốt phần còn lại. */
        if (dirty && now_ms() - last_paint >= 16) repaint();
    }
    return 0;
}
