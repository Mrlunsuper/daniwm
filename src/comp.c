/* comp.c - dani-comp: compositor đơn giản cho daniwm.
 *
 * Tính năng (cố tình minimal, ~600 dòng):
 *   - bóng mềm 7 lớp (XRender, không blur convolution)
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
 *   - XPresent nếu có: pool 3 pixmap, chỉ reuse sau IdleNotify. Buffer
 *     dựng hình riêng không bao giờ gửi cho scanout; fallback XCopyArea
 *     khi Present timeout. Nhịp render mặc định giới hạn khoảng 120Hz.
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
#ifdef WITH_XPRESENT
#include <X11/extensions/Xpresent.h>
#include <X11/extensions/Xrandr.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <sys/select.h>
#include <stdint.h>
#include <limits.h>
#include "animation.h"
#include "animation-settings.h"
#include "animation-lifecycle.h"
#include "layout-animation.h"
#include "exit-layer.h"
#include "workspace-animation.h"
#include "overview-animation.h"

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
static Atom A_WM_WINTYPE_DIALOG;
static Atom A_XROOTPMAP;  /* _XROOTPMAP_ID (feh đổi wallpaper) */
static Atom A_XSETROOT;   /* _XSETROOT_ID */
static Atom A_ANIMATION_SETTINGS, A_WM_CHECK, A_ANIMATION_CONTROL;
static Atom A_LAYOUT_SETTINGS, A_LAYOUT_TRANSACTION;
static Window settings_owner;
static int opt_wm_settings;
static int opt_reduce_motion;
static int opt_force_reduce_motion;
static int opt_close_ms; /* standalone remains opening-only unless explicitly enabled */
static int opt_layout;
static int opt_layout_ms;
static unsigned layout_count;
static size_t layout_bytes;
static Atom A_HIDE_REASON, A_CLOSE_SETTINGS, A_CURRENT_DESKTOP;
static Window reason_owner;
static uint32_t reason_sequence;
static ExitPool exits;
static int exit_xerror;
static unsigned exit_bpp[33], exit_pad[33];
static Atom A_WS_SETTINGS, A_WS_TXN, A_MON_EPOCH;
static int opt_workspace_ms;
static Atom A_OV_SETTINGS, A_OV_ROLE;
static int opt_overview_ms;
/* Outgoing images of one workspace switch, held until all members unmap
 * and the desktop commits. Then they enter the exit pool together. */
static struct {
    int active, committed;
    uint32_t to, count, kept;
    long long prepared;
    Window members[WORKSPACE_ANIMATION_MEMBERS];
    ExitLayer layers[WORKSPACE_ANIMATION_MEMBERS];
} ws_scene;
static uint32_t ws_last_sequence;
static int ws_xerror;

/* options */
static int opt_shadow = 1;
static int opt_fade = 1;
static double opt_dim = 0.92; /* 1.0 = tắt dim */
static int opt_fade_ms = 160;
static int opt_verbose = 0;
#define FRAME_INTERVAL_MS 8 /* ~120Hz: giảm input-to-photon latency */

/* damage ext */
static int dmg_event = 0, dmg_error = 0;

/* render ext error base: picture chết theo window gây RenderBadPicture
 * (wire code = base + BadPicture, đo thực tế = base+1) — cần cho
 * xerror_ignore phân biệt race vô hại với lỗi Render thật. */
static int render_errbase = 0;

/* shape ext (bounding shape cho shaped window) */
static int shape_ok = 0, shape_event = 0;

/* present ext (hardware vsync). present_ok luôn 0 khi build không có
 * XPresent -> mọi frame đi qua XCopyArea (cùng cấu trúc rẽ nhánh). */
static int present_ok = 0;
#ifdef WITH_XPRESENT
static int present_opcode = 0;     /* major opcode, nhận diện XGE cookie */
static int present_event = 0;      /* event base (chỉ để log) */
static XID present_eid = None;     /* event ID từ XPresentSelectInput */
static uint32_t present_serial = 0;
static uint32_t present_pending_serial = 0;
static int present_pending = 0;    /* tối đa 1 request chưa Complete */
static long long present_sent_ms = 0;
#define PRESENT_TIMEOUT_MS 200
#define PRESENT_BUFFERS 3
/* back_buf là ảnh canonical, KHÔNG gửi cho Present. Mỗi slot giữ một
 * snapshot đầy đủ; busy chỉ được hạ bởi IdleNotify đúng pixmap/serial.
 * Complete (kể cả flip) không có nghĩa GPU đã trả pixmap. */
static struct {
    Pixmap pixmap;
    uint32_t serial;
    int busy;
    long long sent_ms;
} present_bufs[PRESENT_BUFFERS];
#endif

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

static void dmg_bbox_add(int x1, int y1, int x2, int y2) {
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

/* thêm 1 rect damage (toạ độ root, x2/y2 là mép phải/dưới) vào hộp bao.
 * Chỉ giữ bbox (không giữ list): vùng damage của cửa sổ con (icon tray)
 * server không báo lên damage của cha, nên clip theo list sẽ bỏ sót vùng
 * đó mãi mãi (tray đen). Bbox bao phủ rộng hơn nên luôn vẽ đủ từ pict. */
static void dmg_add(int x1, int y1, int x2, int y2) {
    dmg_bbox_add(x1, y1, x2, y2);
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

static int rect_intersect(const XRectangle *a, const XRectangle *b, XRectangle *out);

/* đặt clip đích về vùng damage (hoặc bỏ clip nếu full-frame).
 * Luôn clip bằng bbox (cur_clip): copy nền + composite + present đều dùng
 * cùng một vùng nên không bao giờ lệch nhau (trước đây clip list rect hẹp
 * hơn bbox gây đen vùng icon tray mà server không báo damage).
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
    int overview;        /* the WM overview window, bound at map */
    int fullscreen;
    int dock;
    Atom wintype;        /* _NET_WM_WINDOW_TYPE đầu tiên, None nếu không có */
    unsigned opacity;    /* _NET_WM_WINDOW_OPACITY, mặc định ~0U */
    long long born;      /* ms, shape rechecks (separate from fade lifecycle) */
    uint64_t generation;
    Animation opening;
    Pixmap layout_pixmap;
    Picture layout_pict;
    int layout_x, layout_y, layout_target_x, layout_target_y;
    int layout_w, layout_h;
    int layout_old_x, layout_old_y, layout_pending;
    Animation layout;
    int painted;          /* painted candidate, validated before retention */
    int hide_reason;      /* applies only to this tracked generation */
    XRectangle *clip_rects; /* bounding shape (window coords), NULL nếu không shaped */
    int n_clip;
    int shaped;
    int shape_checked;   /* P6: số lần đã recheck shape sau map (0,1,2 = xong) */
    Win *next;
};
static Win *wins;
static uint64_t next_generation;
static void layout_release(Win *w);
typedef struct {
    unsigned count;
    uint32_t sequence;
    unsigned duration;
    Window owner;
} LayoutRequest;
static LayoutRequest layout_request;
static Window active_win = None;
static int dirty = 1;       /* cần repaint */
static int stack_dirty = 1; /* geometry/stacking đổi: cần pre-pass + re-query root */
/* cache để khỏi round-trip/alloc mỗi frame */
static Picture shadow_fill[7] = { None, None, None, None, None, None, None }; /* solid fill 7 lớp bóng */
/* mask alpha: các cửa sổ khác nhau có alpha khác nhau (active/inactive/
 * opacity riêng) nên cache 4 giá trị gần nhất thay vì 1. */
#define ALPHA_SLOTS 8
static Picture alpha_slot[ALPHA_SLOTS] = { None, None, None, None, None, None, None, None };
static double alpha_val[ALPHA_SLOTS] = { -1.0, -1.0, -1.0, -1.0, -1.0, -1.0, -1.0, -1.0 };
static unsigned alpha_next = 0;
/* cache stacking order: frame content-only reuse, khỏi QueryTree mỗi frame */
static Window *stack_cache = NULL;
static unsigned stack_cap = 0;
static unsigned stack_nk = 0; /* số phần tử hợp lệ trong stack_cache */
static XWindowAttributes root_cache; /* attrs root cho frame content-only */
static int have_root = 0;
static int opt_debug = 0;  /* DANI_COMP_DEBUG=1: log event/paint ra stderr */
static long n_damage = 0, n_paint = 0;
static volatile sig_atomic_t quit_req = 0; /* SIGTERM/SIGINT -> dọn rồi thoát */
static void on_quit(int sig) { (void)sig; quit_req = 1; }
static int fading = 0; /* active fade -> existing frame scheduler */
static long long last_paint;

#define OPAQUE (~0U)

/* ---- error handler (forward decl) ---- */
static int xerror_ignore(Display *d, XErrorEvent *e);

/* ---- trapped-error infrastructure (T-E1) ---- */
static int trap_depth;
static int trapped_err;
static int trapped_code;

static int xerror_trapped(Display *d, XErrorEvent *e) {
    (void)d;
    if (!trapped_err) {
        trapped_err = 1;
        trapped_code = (int)e->error_code;
    }
    return 0;
}

static void trap_errors(void) {
    if (trap_depth++ == 0) {
        /* Drain errors from requests issued before this operation. */
        XSync(dpy, False);
        trapped_err = 0;
        trapped_code = 0;
        XSetErrorHandler(xerror_trapped);
    }
}

static int untrap_errors(void) {
    if (trap_depth <= 0) return 0;
    if (--trap_depth > 0) return trapped_err;
    XSync(dpy, False);
    if (trapped_err) {
        char msg[128] = "";
        XGetErrorText(dpy, trapped_code, msg, sizeof(msg) - 1);
        msg[sizeof(msg) - 1] = 0;
        fprintf(stderr, "dani-comp: trapped X error code=%d (%s)\n", trapped_code, msg);
    }
    XSetErrorHandler(xerror_ignore);
    return trapped_err;
}

static long long now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000LL + ts.tv_nsec / 1000000LL;
}

#ifdef WITH_XPRESENT
static int present_free_slot(void) {
    for (int i = 0; i < PRESENT_BUFFERS; i++)
        if (!present_bufs[i].busy) return i;
    return -1;
}

/* Trả deadline còn lại (ms), -1 nếu không cần chờ. Một buffer flip đang
 * scanout được phép busy vô hạn khi vẫn còn slot khác để render. */
static long long present_wait_ms(long long now) {
    if (!present_ok) return -1;
    long long sent;
    if (present_pending) {
        sent = present_sent_ms;
    } else if ((dirty || fading) && present_free_slot() < 0) {
        sent = present_bufs[0].sent_ms;
        for (int i = 1; i < PRESENT_BUFFERS; i++)
            if (present_bufs[i].sent_ms < sent) sent = present_bufs[i].sent_ms;
    } else {
        return -1;
    }
    long long left = sent + PRESENT_TIMEOUT_MS - now;
    return left > 0 ? left : 0;
}

/* XFreePixmap chỉ bỏ reference của client; server giữ storage nếu còn
 * scanout. Không bao giờ ghi lại các pixmap retired này, kể cả timeout
 * hoặc resize. back_buf riêng vẫn an toàn cho fallback XCopyArea. */
static void present_buffers_retire(void) {
    for (int i = 0; i < PRESENT_BUFFERS; i++) {
        if (present_bufs[i].pixmap != None)
            XFreePixmap(dpy, present_bufs[i].pixmap);
    }
    memset(present_bufs, 0, sizeof(present_bufs));
}

static void present_disable(void) {
    present_ok = 0;
    present_pending = 0;
    present_buffers_retire();
}

static void present_complete(const XPresentCompleteNotifyEvent *pe) {
    if (pe->eid == present_eid && pe->window == root &&
        pe->kind == PresentCompleteKindPixmap && present_pending &&
        pe->serial_number == present_pending_serial)
        present_pending = 0;
}

static void present_buffer_idle(const XPresentIdleNotifyEvent *pe) {
    if (pe->eid != present_eid || pe->window != root) return;
    for (int i = 0; i < PRESENT_BUFFERS; i++) {
        if (pe->pixmap == present_bufs[i].pixmap &&
            pe->serial_number == present_bufs[i].serial)
            present_bufs[i].busy = 0;
    }
}
#endif

static void exit_release(unsigned long picture, unsigned long pixmap) {
    if (picture) XRenderFreePicture(dpy, picture);
    if (pixmap) XFreePixmap(dpy, pixmap);
}
static void ws_scene_clear(void) {
    /* Unpublish before issuing asynchronous resource releases. */
    ExitLayer held[WORKSPACE_ANIMATION_MEMBERS];
    uint32_t count = ws_scene.count;
    memcpy(held, ws_scene.layers, sizeof(held));
    memset(&ws_scene, 0, sizeof(ws_scene));
    ws_xerror = 0;
    for (uint32_t i = 0; i < count; i++)
        if (held[i].bytes) exit_release(held[i].picture, held[i].pixmap);
}
/* Drop exit images except the kinds in keep (a later hide must not cut a
 * running workspace crossfade or overview fade-out). */
#define KEEP_WORKSPACE 1
#define KEEP_OVERVIEW 2
static void exits_clear_layers(int keep) {
    for (unsigned i = 0; i < EXIT_COUNT; i++)
        if (exits.layers[i].bytes && !((keep & KEEP_WORKSPACE) && exits.layers[i].workspace) &&
            !((keep & KEEP_OVERVIEW) && exits.layers[i].overview)) {
            exit_drop(&exits, i, exit_release);
            dirty = full_dirty = 1;
        }
}
static void exits_clear(void) {
    if (ws_scene.active) ws_scene_clear();
    if (!exits.count) return;
    exit_clear(&exits, exit_release);
    dirty = full_dirty = 1;
}
static void exits_maintain(long long now) {
    if (ws_scene.active && (ws_xerror || now - ws_scene.prepared >= EXIT_TTL_MS ||
        now < ws_scene.prepared)) ws_scene_clear();
    if (!exits.count && !exit_xerror) return;
    unsigned before = exits.count;
    if (exit_xerror) { exits_clear(); exit_xerror = 0; }
    if (exits.count) exit_expire(&exits, now, exit_release);
    if (before != exits.count) dirty = full_dirty = 1;
    if (exits.count) fading = 1;
}
static int repaint_due(void) {
    long long now = now_ms();
    exits_maintain(now);
#ifdef WITH_XPRESENT
    /* Kiểm tra kể cả khi chưa dirty: Complete bị mất cũng phải timeout. */
    if (present_wait_ms(now) == 0) {
        present_disable();
        if (opt_debug)
            fprintf(stderr, "dani-comp: XPresent timeout, fallback to XCopyArea\n");
    }
    if (present_ok && (present_pending || present_free_slot() < 0)) return 0;
#endif
    if (!dirty && !fading) return 0;
    return now - last_paint >= FRAME_INTERVAL_MS;
}


static Win *win_get(Window id) {
    for (Win *w = wins; w; w = w->next)
        if (w->id == id) return w;
    return NULL;
}

/* The overview is the only override-redirect window that fades. */
static int win_fades(const Win *w) {
    if (w->overview) return opt_overview_ms > 0;
    return opt_fade && !opt_reduce_motion && opt_fade_ms > 0 &&
        !w->override && !w->dock && !w->fullscreen;
}
static void opening_start(Win *w, long long now) {
    animation_cancel(&w->opening);
    if (win_fades(w))
        animation_start(&w->opening, w->id, w->generation, now,
            (unsigned)(w->overview ? opt_overview_ms : opt_fade_ms));
}

/* Opt-in only. Invalid, absent, stale or deleted settings fail closed to
 * instant mode. No acknowledgment, compositor restart, or WM round trip. */
static Window settings_wm(void) {
    Atom type; int format; unsigned long n, extra;
    unsigned char *data = NULL;
    Window owner = None;
    if (XGetWindowProperty(dpy, root, A_WM_CHECK, 0, 1, False, XA_WINDOW,
        &type, &format, &n, &extra, &data) == Success && type == XA_WINDOW &&
        format == 32 && n == 1 && !extra && data) owner = *(unsigned long *)data;
    if (data) XFree(data);
    if (!owner) return None;
    data = NULL;
    trap_errors();
    int valid = XGetWindowProperty(dpy, owner, A_WM_CHECK, 0, 1, False, XA_WINDOW,
        &type, &format, &n, &extra, &data) == Success && type == XA_WINDOW &&
        format == 32 && n == 1 && !extra && data && *(unsigned long *)data == owner;
    if (valid) XSelectInput(dpy, owner, StructureNotifyMask);
    if (data) XFree(data);
    if (untrap_errors() || !valid) return None;
    return owner;
}
static void layout_settings_read(Window owner) {
    Atom type; int format; unsigned long n = 0, extra = 0;
    unsigned char *data = NULL;
    int duration = 0;
    if (owner && A_LAYOUT_SETTINGS &&
        XGetWindowProperty(dpy, root, A_LAYOUT_SETTINGS, 0, 3, False, XA_CARDINAL,
            &type, &format, &n, &extra, &data) == Success && type == XA_CARDINAL &&
        format == 32 && layout_animation_settings_valid((unsigned long *)data,
            n, extra, owner)) duration = (int)((unsigned long *)data)[2];
    if (data) XFree(data);
    int enabled = duration > 0 && opt_fade && !opt_reduce_motion;
    if (enabled != opt_layout || duration != opt_layout_ms) {
        opt_layout = enabled;
        opt_layout_ms = duration;
        for (Win *w = wins; w; w = w->next) layout_release(w);
        layout_request.count = 0;
        fading = 0;
        dirty = full_dirty = 1;
    }
}
static void animation_settings_read(void) {
    if (!opt_wm_settings) return;
    Window owner = settings_wm();
    Atom type; int format; unsigned long n = 0, extra = 0;
    unsigned char *data = NULL;
    int enabled = 0, reduced = 0, duration = ANIMATION_OPEN_DEFAULT;
    if (owner && XGetWindowProperty(dpy, root, A_ANIMATION_SETTINGS, 0,
        ANIMATION_SETTINGS_WORDS, False, XA_CARDINAL, &type, &format, &n,
        &extra, &data) == Success && type == XA_CARDINAL && format == 32 &&
        animation_settings_valid((unsigned long *)data, n, extra, owner)) {
        unsigned long *v = (unsigned long *)data;
        enabled = (int)v[2]; reduced = (int)v[3]; duration = (int)v[4];
    }
    if (data) XFree(data);
    reduced = reduced || opt_force_reduce_motion;
    int closing = 0;
    data = NULL;
    if (owner && A_CLOSE_SETTINGS && XGetWindowProperty(dpy, root, A_CLOSE_SETTINGS, 0,
        3, False, XA_CARDINAL, &type, &format, &n, &extra, &data) == Success &&
        type == XA_CARDINAL && format == 32 &&
        animation_close_valid((unsigned long *)data, n, extra, owner))
        closing = (int)((unsigned long *)data)[2];
    if (data) XFree(data);
    int workspace = 0;
    data = NULL;
    if (owner && A_WS_SETTINGS && XGetWindowProperty(dpy, root, A_WS_SETTINGS, 0,
        3, False, XA_CARDINAL, &type, &format, &n, &extra, &data) == Success &&
        type == XA_CARDINAL && format == 32 &&
        workspace_settings_valid((unsigned long *)data, n, extra, owner))
        workspace = (int)((unsigned long *)data)[2];
    if (data) XFree(data);
    if (!enabled || reduced) workspace = 0;
    int overview = 0;
    data = NULL;
    if (owner && A_OV_SETTINGS && XGetWindowProperty(dpy, root, A_OV_SETTINGS, 0,
        3, False, XA_CARDINAL, &type, &format, &n, &extra, &data) == Success &&
        type == XA_CARDINAL && format == 32 &&
        overview_settings_valid((unsigned long *)data, n, extra, owner))
        overview = (int)((unsigned long *)data)[2];
    if (data) XFree(data);
    if (!enabled || reduced) overview = 0;
    int changed = enabled != opt_fade || reduced != opt_reduce_motion ||
        duration != opt_fade_ms || closing != opt_close_ms || owner != settings_owner ||
        workspace != opt_workspace_ms || overview != opt_overview_ms;
    if (owner != settings_owner) ws_last_sequence = 0;
    opt_workspace_ms = workspace;
    opt_overview_ms = overview;
    opt_close_ms = closing;
    opt_fade = enabled; opt_reduce_motion = reduced; opt_fade_ms = duration;
    settings_owner = owner;
    layout_settings_read(owner);
    if (changed) {
        /* New settings affect subsequent maps only; existing images snap
         * to final alpha (including when motion is switched off mid-fade). */
        for (Win *w = wins; w; w = w->next) animation_cancel(&w->opening);
        exits_clear();
        fading = 0;
        dirty = full_dirty = 1;
    }
}

static void win_free_pix(Win *w) {
    layout_release(w);
    w->painted = 0;
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
    Bool bounding_shaped = False, clip_shaped = False;
    int bx = 0, by = 0, cx = 0, cy = 0;
    unsigned bw = 0, bh = 0, cw = 0, ch = 0;
    if (!XShapeQueryExtents(dpy, w->id, &bounding_shaped,
            &bx, &by, &bw, &bh, &clip_shaped, &cx, &cy, &cw, &ch))
        return 0;
    if (!bounding_shaped) return 0;
    int n = 0, ord = 0;
    XRectangle *rects = XShapeGetRectangles(dpy, w->id, ShapeBounding, &n, &ord);
    if (n < 0) { if (rects) XFree(rects); return 0; }
    /* n == 0 is a valid explicit empty shape, not an unshaped window. */
    w->clip_rects = rects; /* giữ nguyên để dịch lúc vẽ */
    w->n_clip = n;
    w->shaped = 1;
    if (opt_debug) {
        if (n > 0) {
            fprintf(stderr, "dani-comp: shaped 0x%lx rects=%d first=%d,%d %ux%u\n",
                (unsigned long)w->id, n, rects[0].x, rects[0].y,
                rects[0].width, rects[0].height);
        } else {
            fprintf(stderr, "dani-comp: shaped 0x%lx rects=0 (empty)\n",
                (unsigned long)w->id);
        }
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

/* atom đầu tiên của _NET_WM_WINDOW_TYPE (None nếu không đặt) */
static Atom read_wintype(Window id) {
    Atom rt; int rf; unsigned long n = 0, extra = 0;
    unsigned char *data = NULL;
    Atom ty = None;
    if (A_WM_WINTYPE == None) return None;
    if (XGetWindowProperty(dpy, id, A_WM_WINTYPE, 0, 4, False, XA_ATOM,
            &rt, &rf, &n, &extra, &data) == Success && data) {
        if (rf == 32 && n >= 1) ty = ((Atom *)data)[0];
        XFree(data);
    }
    return ty;
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
    w->wintype = read_wintype(w->id);
}

static void layout_release(Win *w) {
    if (!w) return;
    animation_cancel(&w->layout);
    w->layout_pending = 0;
    if (w->layout_pict || w->layout_pixmap) {
        if (layout_count) layout_count--;
        size_t bytes = (size_t)w->layout_w * (size_t)w->layout_h * 4u;
        if (bytes <= layout_bytes) layout_bytes -= bytes; else layout_bytes = 0;
    }
    if (w->layout_pict) { XRenderFreePicture(dpy, w->layout_pict); w->layout_pict = None; }
    if (w->layout_pixmap) { XFreePixmap(dpy, w->layout_pixmap); w->layout_pixmap = None; }
    w->layout_w = w->layout_h = 0;
}
static void layout_begin(Win *w, int target_x, int target_y, long long now) {
    int start_x, start_y;
    double f;
    if (!w || !w->layout_pending || !opt_layout || opt_layout_ms <= 0 ||
        w->override || w->dock || w->fullscreen || w->shaped || w->depth == 32 ||
        w->opacity != OPAQUE || w->w <= 0 || w->h <= 0 ||
        (!w->pict && !w->layout_pict)) return;
    w->layout_pending = 0;
    if (w->layout.active) {
        f = animation_value(&w->layout, w->id, w->generation, now);
        if (f < 1.0) {
            start_x = w->layout_x + (int)((w->layout_target_x - w->layout_x) * f);
            start_y = w->layout_y + (int)((w->layout_target_y - w->layout_y) * f);
            w->layout_x = start_x; w->layout_y = start_y;
        } else {
            w->layout_x = w->x; w->layout_y = w->y;
        }
    } else {
        w->layout_x = w->layout_old_x;
        w->layout_y = w->layout_old_y;
    }
    w->layout_target_x = target_x;
    w->layout_target_y = target_y;
    if (!w->layout_pict && w->pict) {
        int lw = w->w + 2 * w->bw, lh = w->h + 2 * w->bh;
        size_t bytes = (size_t)lw * (size_t)lh * 4u;
        if (layout_count >= EXIT_COUNT || bytes > EXIT_BYTES ||
            layout_bytes > EXIT_BYTES || exits.bytes > EXIT_BYTES - layout_bytes ||
            bytes > EXIT_BYTES - layout_bytes - exits.bytes) {
            w->layout_pending = 0;
            return;
        }
        w->layout_pict = w->pict; w->pict = None;
        w->layout_pixmap = w->pixmap; w->pixmap = None;
        w->layout_w = lw; w->layout_h = lh;
        layout_count++; layout_bytes += bytes;
    }
    w->painted = 0;
    animation_start(&w->layout, w->id, w->generation, now, (unsigned)opt_layout_ms);
}
static void layout_transaction_read(void) {
    Atom type; int format; unsigned long n = 0, extra = 0;
    unsigned char *data = NULL;
    if (!opt_layout || !settings_owner || A_LAYOUT_TRANSACTION == None ||
        XGetWindowProperty(dpy, root, A_LAYOUT_TRANSACTION, 0,
            LAYOUT_ANIMATION_MAX_WORDS, False, XA_CARDINAL, &type, &format,
            &n, &extra, &data) != Success || type != XA_CARDINAL || format != 32 ||
        extra || n < LAYOUT_ANIMATION_HEADER_WORDS || !data) {
        if (data) XFree(data);
        return;
    }
    unsigned long *v = (unsigned long *)data;
    unsigned count = (unsigned)v[4];
    if (v[0] != LAYOUT_ANIMATION_VERSION || v[1] != (unsigned long)settings_owner ||
        v[2] == 0 || v[3] == 0 || v[3] > LAYOUT_ANIMATION_MAX_MS ||
        count == 0 || count > LAYOUT_ANIMATION_MAX_CLIENTS ||
        n != LAYOUT_ANIMATION_HEADER_WORDS + count * LAYOUT_ANIMATION_ENTRY_WORDS ||
        (layout_request.sequence && (uint32_t)v[2] <= layout_request.sequence)) {
        XFree(data); return;
    }
    layout_request.count = 0;
    layout_request.sequence = (uint32_t)v[2];
    layout_request.duration = (unsigned)v[3];
    layout_request.owner = settings_owner;
    for (unsigned i = 0; i < count; i++) {
        unsigned long *q = v + LAYOUT_ANIMATION_HEADER_WORDS +
            i * LAYOUT_ANIMATION_ENTRY_WORDS;
        Win *w = win_get((Window)q[0]);
        int old_w = w && w->layout_pict ? w->layout_w - 2 * w->bw : (w ? w->w : 0);
        int old_h = w && w->layout_pict ? w->layout_h - 2 * w->bh : (w ? w->h : 0);
        if (!w || !w->mapped || (!w->pict && !w->layout_pict) || w->shaped || w->depth == 32 ||
            w->opacity != OPAQUE || q[3] != (unsigned long)old_w ||
            q[4] != (unsigned long)old_h)
            continue;
        /* The WM moves windows before it sets this property, so the
         * ConfigureNotify is already applied. Not moved yet: skip. */
        w->layout_old_x = (int)q[1]; w->layout_old_y = (int)q[2];
        if (w->x == w->layout_old_x && w->y == w->layout_old_y) continue;
        w->layout_pending = 1;
        layout_begin(w, w->x, w->y, now_ms());
        w->layout_pending = 0;
        if (w->layout.active) layout_request.count++;
    }
    XFree(data);
    if (layout_request.count) { opt_layout_ms = (int)v[3]; dirty = full_dirty = 1; fading = 1; }
}
static void win_del(Window id);

/* Bind the WM overview role to this map of the window. Only the window the
 * WM published gets it; menus and other override windows stay instant. */
static void overview_mark(Win *w) {
    w->overview = 0;
    if (!w->override || opt_overview_ms <= 0 || !settings_owner) return;
    Atom type; int format; unsigned long n = 0, extra = 0;
    unsigned char *data = NULL;
    if (XGetWindowProperty(dpy, root, A_OV_ROLE, 0, 3, False, XA_CARDINAL, &type, &format,
        &n, &extra, &data) == Success && type == XA_CARDINAL && format == 32 &&
        overview_role_window((unsigned long *)data, n, extra, settings_owner) == w->id)
        w->overview = 1;
    if (data) XFree(data);
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
    /* Fail instant rather than reuse a generation after counter exhaustion. */
    w->generation = next_generation == UINT64_MAX ? 0 : ++next_generation;
    w->opacity = OPAQUE;
    w->born = now_ms();
    w->next = wins;
    wins = w;
    /* theo dõi damage + prop của cửa sổ này */
    XSelectInput(dpy, id, PropertyChangeMask | StructureNotifyMask);
    if (shape_ok)
        XShapeSelectInput(dpy, id, ShapeNotifyMask); /* bounding shape đổi */
    trap_errors();
    w->damage = XDamageCreate(dpy, id, XDamageReportNonEmpty);
    if (untrap_errors()) {
        w->damage = None;
        win_del(id);
        return NULL;
    }
    win_refresh_flags(w);
    /* override_redirect (bar của daniwm, menu...) không fade: hiện ngay */
    if (w->override) w->born = 0;
    overview_mark(w);
    opening_start(w, now_ms());
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
    if (win_fades(w)) {
        double f = animation_sample(&w->opening, w->id, w->generation, now);
        a *= f;
        if (w->opening.active) fading = 1;
    } else animation_cancel(&w->opening);
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
    if (w->dock) return 0;
    /* override: bar/tray/menu không bóng; dani-run (DIALOG) được bóng
     * như cửa sổ thường */
    if (w->override && w->wintype != A_WM_WINTYPE_DIALOG) return 0;
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
    if (w->layout.active) return 0;
    if (w->depth == 32) return 0; /* ARGB: trong suốt từng pixel có thể */
    if (w->opacity != OPAQUE) return 0;
    if (w->shaped) return 0;
    if (win_fades(w) && animation_value(&w->opening, w->id, w->generation, now) < 1.0)
        return 0;
    return 1;
}

/* Transfer only an already painted named pixmap. The server keeps its
 * storage alive across unmap/destroy while this compositor owns references.
 * No client query, capture acknowledgment, or WM delay occurs here. */
static int exit_layer_from(Win *w, long long now, ExitLayer *layer) {
    if ((w->override && !w->overview) || w->dock || w->fullscreen || !w->generation || !w->painted ||
        !w->pixmap || !w->pict || w->depth < 1 || w->depth > 32 ||
        w->n_clip < 0 || w->n_clip > 256 || (w->shaped && !w->n_clip)) return 0;
    /* Confirm pending Render requests on the compositor connection only.
     * The WM has already committed removal; this never queries the client. */
    XSync(dpy, False);
    if (!w->painted || exit_xerror || ws_xerror) return 0;
    memset(layer, 0, sizeof(*layer));
    layer->identity = w->id; layer->generation = w->generation;
    layer->pixmap = w->pixmap; layer->picture = w->pict;
    layer->x = w->x; layer->y = w->y;
    layer->width = w->w + 2 * w->bw; layer->height = w->h + 2 * w->bh;
    if (layer->width <= 0 || layer->height <= 0 ||
        !exit_size((unsigned)layer->width, (unsigned)layer->height,
            exit_bpp[w->depth], exit_pad[w->depth], &layer->bytes)) return 0;
    layer->retained = now;
    layer->alpha = (double)w->opacity / OPAQUE *
        animation_value(&w->opening, w->id, w->generation, now);
    layer->dim = win_dim(w);
    layer->shaped = w->shaped; layer->nclip = (unsigned)w->n_clip;
    for (unsigned i = 0; i < layer->nclip; i++) {
        layer->clip[i].x = w->clip_rects[i].x;
        layer->clip[i].y = w->clip_rects[i].y;
        layer->clip[i].width = w->clip_rects[i].width;
        layer->clip[i].height = w->clip_rects[i].height;
    }
    return 1;
}
static void closing_detach(Win *w, long long now) {
    if (!opt_fade || opt_reduce_motion || opt_close_ms <= 0 || w->hide_reason || w->overview ||
        (opt_wm_settings && !settings_owner) ||
        (reason_owner && !opt_wm_settings)) return;
    ExitLayer layer;
    if (!exit_layer_from(w, now, &layer)) return;
    /* Anchor immediately below the next surviving live generation. Unknown
     * stacking means skip rather than arbitrarily put an image on top. */
    int found = 0;
    for (unsigned i = 0; i < stack_nk; i++) {
        if (stack_cache[i] == w->id) { found = 1; layer.rank = i; continue; }
        if (!found) continue;
        Win *above = win_get(stack_cache[i]);
        if (above && above->mapped) {
            layer.above = above->id; layer.above_generation = above->generation;
            break;
        }
    }
    if (!found || layer.alpha <= 0) return;
    /* A stale XID anchor is validated by generation again at paint time. */
    animation_start(&layer.animation, layer.identity, layer.generation, now, (unsigned)opt_close_ms);
    if (!exit_admit(&exits, &layer)) return;
    w->pixmap = None; w->pict = None; w->painted = 0;
    fading = 1; dirty = full_dirty = 1;
}

/* Overview hide: keep its painted image and fade it out above everything.
 * The window is already gone, so the image takes no input. */
static void overview_detach(Win *w, long long now) {
    ExitLayer layer;
    if (opt_overview_ms <= 0 || !exit_layer_from(w, now, &layer) || layer.alpha <= 0) return;
    layer.overview = 1;
    layer.rank = UINT_MAX;
    for (unsigned i = 0; i < EXIT_COUNT; i++)
        if (exits.layers[i].overview) exit_drop(&exits, i, exit_release);
    animation_start(&layer.animation, layer.identity, layer.generation, now,
        (unsigned)opt_overview_ms);
    if (!exit_admit(&exits, &layer)) {
        if (opt_debug) fprintf(stderr, "dani-comp: overview bypass: pool\n");
        return;
    }
    w->pixmap = None; w->pict = None; w->painted = 0;
    fading = 1; dirty = full_dirty = 1;
}

static int ws_member(Window id) {
    for (uint32_t i = 0; i < ws_scene.count; i++) if (ws_scene.members[i] == id) return (int)i;
    return -1;
}
/* All members kept and the desktop committed: start every image at once.
 * Ranks come from the server stack now, so a restack after PREPARE still
 * draws in order. Any failure frees all images: the switch stays instant. */
static void ws_admit(long long now) {
    if (!ws_scene.active || !ws_scene.committed || ws_scene.kept != ws_scene.count) return;
    Window r, p, *kids = NULL;
    unsigned nk = 0;
    int ok = XQueryTree(dpy, root, &r, &p, &kids, &nk) &&
        exits.count + ws_scene.count <= EXIT_COUNT;
    size_t bytes = 0;
    for (uint32_t i = 0; ok && i < ws_scene.count; i++) {
        ExitLayer *l = &ws_scene.layers[i];
        unsigned k = 0;
        while (k < nk && kids[k] != l->identity) k++;
        if (k == nk || l->bytes > EXIT_BYTES - bytes) ok = 0;
        l->rank = k;
        bytes += l->bytes;
    }
    if (kids) XFree(kids);
    if (!ok || bytes > EXIT_BYTES - exits.bytes) {
        if (opt_debug) fprintf(stderr, "dani-comp: workspace bypass: pool\n");
        ws_scene_clear();
        return;
    }
    for (uint32_t i = 0; i < ws_scene.count; i++) {
        ExitLayer *l = &ws_scene.layers[i];
        l->retained = now;
        animation_start(&l->animation, l->identity, l->generation, now,
            (unsigned)opt_workspace_ms);
        if (exit_admit(&exits, l)) memset(l, 0, sizeof(*l));
        else ok = 0;
    }
    ws_scene_clear();
    if (!ok) for (unsigned i = 0; i < EXIT_COUNT; i++)
        if (exits.layers[i].workspace) exit_drop(&exits, i, exit_release);
    fading = 1; dirty = full_dirty = 1;
}
/* Member UnmapNotify: keep its already painted image. Anything unexpected
 * ends the scene. */
static void ws_retain(Win *w, long long now) {
    int i = ws_member(w->id);
    if (i < 0) return;
    ExitLayer layer;
    if (ws_scene.layers[i].bytes || w->hide_reason != ANIMATION_HIDE_WORKSPACE ||
        !exit_layer_from(w, now, &layer)) {
        if (opt_debug) fprintf(stderr, "dani-comp: workspace bypass: member 0x%lx reason=%d painted=%d\n",
            (unsigned long)w->id, w->hide_reason, w->painted);
        ws_scene_clear();
        return;
    }
    layer.workspace = 1;
    ws_scene.layers[i] = layer;
    ws_scene.kept++;
    w->pixmap = None; w->pict = None; w->painted = 0;
    ws_admit(now);
}
static long ws_desktop(void) {
    Atom type; int format; unsigned long n = 0, extra = 0;
    unsigned char *data = NULL;
    long desktop = -1;
    if (XGetWindowProperty(dpy, root, A_CURRENT_DESKTOP, 0, 1, False, XA_CARDINAL,
        &type, &format, &n, &extra, &data) == Success && type == XA_CARDINAL &&
        format == 32 && n == 1 && !extra && data)
        desktop = (long)(uint32_t)*(unsigned long *)data;
    if (data) XFree(data);
    return desktop;
}
/* PREPARE from the WM. Newest wins: every earlier image ends here. Any
 * doubt means no scene, so the switch is instant. */
static void ws_prepare(uint32_t sequence) {
    if (!workspace_sequence_newer(sequence, ws_last_sequence)) return;
    ws_last_sequence = sequence;
    exits_clear();
    if (opt_workspace_ms <= 0 || !settings_owner || reason_owner != settings_owner) return;
    Atom type; int format; unsigned long n = 0, extra = 0;
    unsigned char *data = NULL;
    unsigned long epoch = 0;
    if (XGetWindowProperty(dpy, root, A_MON_EPOCH, 0, 1, False, XA_CARDINAL,
        &type, &format, &n, &extra, &data) == Success && type == XA_CARDINAL &&
        format == 32 && n == 1 && !extra && data) epoch = *(unsigned long *)data;
    if (data) XFree(data);
    if (!epoch) return;
    data = NULL; n = extra = 0;
    WorkspaceTxn t;
    int valid = XGetWindowProperty(dpy, root, A_WS_TXN, 0, WORKSPACE_ANIMATION_WORDS, False,
        XA_CARDINAL, &type, &format, &n, &extra, &data) == Success &&
        type == XA_CARDINAL && format == 32 &&
        workspace_txn_decode(&t, (unsigned long *)data, n, extra, settings_owner, epoch);
    if (data) XFree(data);
    long long now = now_ms();
    if (!valid || t.sequence != sequence || !t.count ||
        (uint32_t)((uint32_t)now - t.sent_ms) > WORKSPACE_ANIMATION_DEADLINE_MS) {
        if (opt_debug) fprintf(stderr, "dani-comp: workspace bypass: PREPARE %u\n", sequence);
        return;
    }
    for (uint32_t i = 0; i < t.count; i++) {
        Win *w = win_get(t.members[i]);
        if (!w || !w->mapped || w->override || w->dock || w->fullscreen) {
            if (opt_debug) fprintf(stderr, "dani-comp: workspace bypass: member 0x%lx\n",
                (unsigned long)t.members[i]);
            return;
        }
    }
    ws_scene.active = 1;
    ws_scene.to = t.to;
    ws_scene.count = t.count;
    ws_scene.prepared = now;
    for (uint32_t i = 0; i < t.count; i++) ws_scene.members[i] = t.members[i];
}

/* đảm bảo back buffer + snapshot nền đúng size root hiện tại */
static void ensure_targets(int w, int h, Visual *vis, int depth) {
    if (back_buf != None && w == back_w && h == back_h) return;
#ifdef WITH_XPRESENT
    present_buffers_retire(); /* resize: không reuse slot của size cũ */
#endif
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

/* vẽ bóng: 7 lớp đen mờ chồng nhau, alpha tăng dần từ ngoài vào trong
 * (đuôi gaussian: rìa ngoài chỉ ~2% nên không còn viền cứng, gần cửa sổ
 * đậm dần ~30% như picom radius 12). Mỗi lớp 1 lần composite, chỉ trong
 * vành bóng nên nhẹ hơn blur convolution nhiều. */
static void paint_shadow(int x, int y, int w, int h) {
    static const struct { int grow; int dy; unsigned short alpha; } layers[] = {
        { 12, 5, 0x0600 }, /* ngoài cùng, nhạt nhất */
        { 10, 4, 0x0800 },
        { 8, 4, 0x0a00 },
        { 6, 3, 0x0c00 },
        { 4, 2, 0x0e00 },
        { 3, 2, 0x1000 },
        { 2, 1, 0x1200 }, /* sát cửa sổ, đậm nhất */
    };
    /* P5: cửa sổ (mờ đục) sẽ phủ phần giữa -> chỉ vẽ vành bóng, khỏi blend
     * phần bị che (overdraw phí). Vành = bao ngoài trừ hộp cửa sổ = 4 rect. */
    int ox = x - 12, oy = y - 12 + 5, ow = w + 24, oh = h + 24; /* bao ngoài mọi lớp */
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
    trap_errors();
    Pixmap pm = XCompositeNameWindowPixmap(dpy, w->id);
    int pm_error = untrap_errors();
    if (pm_error || !pm) return 0;
    XRenderPictFormat *fmt = XRenderFindVisualFormat(dpy, a.visual);
    if (!fmt) { XFreePixmap(dpy, pm); return 0; }
    trap_errors();
    Picture pict = XRenderCreatePicture(dpy, pm, fmt, 0, NULL);
    int pict_error = untrap_errors();
    if (pict_error || !pict) { XFreePixmap(dpy, pm); return 0; }
    if ((opt_close_ms > 0 || opt_workspace_ms > 0 || opt_overview_ms > 0) &&
        opt_fade && !opt_reduce_motion) {
        Window rr; int px, py; unsigned pw, ph, pb, pd;
        trap_errors();
        int valid = XGetGeometry(dpy, pm, &rr, &px, &py, &pw, &ph, &pb, &pd);
        int error = untrap_errors();
        if (error || !valid || pw != (unsigned)(a.width + 2 * a.border_width) ||
            ph != (unsigned)(a.height + 2 * a.border_width) || pd != (unsigned)a.depth) {
            XRenderFreePicture(dpy, pict); XFreePixmap(dpy, pm);
            return 0;
        }
    }
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
        if (w->pict == None) continue; /* chưa realize: không poll/busy-loop */
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
        if (w->pict == None) continue; /* khớp shape_poll(): chưa có lịch */
        long long due = w->born + SHAPE_THR[w->shape_checked] - now;
        if (due < 0) due = 0;
        if (next < 0 || due < next) next = due;
    }
    return next;
}

/* skip_top: leave overview images for the final call above everything. */
static void paint_exits(Window before, long long now, unsigned char *drawn, int skip_top) {
    /* Rank order is explicit even if pool slots have been recycled. */
    for (unsigned pass = 0; pass < EXIT_COUNT; pass++) {
        unsigned best = EXIT_COUNT;
        for (unsigned i = 0; i < EXIT_COUNT; i++) {
            ExitLayer *s = &exits.layers[i];
            if (!s->bytes || drawn[i] || s->above != before || (skip_top && s->overview)) continue;
            if (best == EXIT_COUNT || s->rank < exits.layers[best].rank) best = i;
        }
        if (best == EXIT_COUNT) break;
        drawn[best] = 1;
        ExitLayer *s = &exits.layers[best];
        Win *anchor = before ? win_get(before) : NULL;
        if (before && (!anchor || !anchor->mapped || anchor->generation != s->above_generation)) {
            exit_drop(&exits, best, exit_release);
            continue;
        }
        double a = s->alpha * (1.0 - animation_sample(&s->animation,
            s->identity, s->generation, now));
        if (a <= 0) continue;
        /* Retained pictures never enter the opaque fast path. Restore all
         * mutable Render state explicitly before reusing their references. */
        XTransform identity = {{{65536, 0, 0}, {0, 65536, 0}, {0, 0, 65536}}};
        XRenderPictureAttributes pa = {0}; pa.clip_mask = None;
        XRenderSetPictureTransform(dpy, s->picture, &identity);
        XRenderSetPictureFilter(dpy, s->picture, FilterNearest, NULL, 0);
        XRenderChangePicture(dpy, s->picture, CPClipMask, &pa);
        if (s->shaped) {
            XRectangle clips[256];
            for (unsigned i = 0; i < s->nclip; i++)
                clips[i] = (XRectangle){(short)(s->x + s->clip[i].x),
                    (short)(s->y + s->clip[i].y), s->clip[i].width, s->clip[i].height};
            XRenderSetPictureClipRectangles(dpy, back_pict, 0, 0, clips, (int)s->nclip);
        }
        XRenderComposite(dpy, PictOpOver, s->picture, alpha_mask(a), back_pict,
            0, 0, 0, 0, s->x, s->y, (unsigned)s->width, (unsigned)s->height);
        if (s->dim < 1.0)
            XRenderComposite(dpy, PictOpOver, alpha_mask((1.0 - s->dim) * a),
                s->picture, back_pict, 0, 0, 0, 0, s->x, s->y,
                (unsigned)s->width, (unsigned)s->height);
        back_clip_reset();
    }
}

static void repaint(void) {
    Window r, p, *kids = NULL;
    unsigned nk = 0;
    long long now = now_ms();
    XWindowAttributes ra;
    int was_fading = fading;
    exits_maintain(now);
    fading = exits.count != 0;
    for (Win *w = wins; w; w = w->next)
        if (w->layout.active) fading = 1;
    shape_poll(now); /* P6: recheck shape cửa sổ non tại mốc 100/400ms */
    /* P1: full-frame khi stack/geometry/nền/appearance đổi hoặc không có hộp
     * damage; ngược lại (gõ chữ, video) chỉ vẽ lại hộp bao damage. */
    int full = full_dirty || stack_dirty || bg_dirty || was_fading || exits.count || !dmg_have;
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
        if (nk > 0) {
            if (nk > stack_cap) {
                Window *new_cache = realloc(stack_cache, nk * sizeof(Window));
                if (!new_cache) { XFree(kids); return; }
                stack_cache = new_cache;
                stack_cap = nk;
            }
            memcpy(stack_cache, kids, nk * sizeof(Window));
            stack_nk = nk;
            XFree(kids);
            kids = stack_cache;
        } else {
            stack_nk = 0;
            nk = 0;
        }
    } else {
        nk = stack_nk;
        kids = stack_cache; /* mượn tạm, KHÔNG free ở cuối */
    }
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
                if (opt_debug && w->overview && !vs)
                    fprintf(stderr, "dani-comp: overview bypass: unmapped before its UnmapNotify\n");
                win_free_pix(w);
                if (vs && !w->override) w->born = now_ms();
                w->mapped = vs;
                win_refresh_flags(w);
                if (vs && !w->hide_reason) opening_start(w, now);
                else animation_cancel(&w->opening);
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
    for (int i = exits.count ? -1 : (int)nk - 1; i >= 0; i--) {
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
        XFlush(dpy);
        last_paint = now;
        dirty = 0;
        return;
    }
    /* kids[0] = dưới cùng -> vẽ từ dưới lên trên. P4: bắt đầu từ occluder
     * đục trên cùng (start) nếu có -> bỏ mọi cửa sổ bị che hoàn toàn. */
    unsigned char exits_drawn[EXIT_COUNT] = {0};
    for (unsigned i = start; i <= nk; i++) {
        Window id = i < nk ? kids[i] : None;
        if (exits.count) {
            Win *upper = id ? win_get(id) : NULL;
            /* A newly mapped dock/menu/overview must stay above detached
             * normal-client visuals even if absent from the old stack. */
            if (upper && upper->mapped && (upper->dock || upper->override || upper->fullscreen))
                paint_exits(None, now, exits_drawn, 1);
            paint_exits(id, now, exits_drawn, 0);
        }
        if (i == nk) break;
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
        if (!w->layout.active && w->layout_pict && !w->layout_pending)
            layout_release(w);
        int layout_visual = w->layout.active && w->layout_pict != None;
        if (layout_visual) {
            double done = animation_value(&w->layout, w->id, w->generation, now);
            if (done >= 1.0) {
                layout_release(w);
                layout_visual = 0;
            }
        }
        if (!layout_visual && !ensure_win_pict(w)) continue;
        Picture draw_pict = layout_visual ? w->layout_pict : w->pict;
        if (layout_visual) {
            XTransform identity = {{{65536, 0, 0}, {0, 65536, 0}, {0, 0, 65536}}};
            XRenderPictureAttributes pa = {0}; pa.clip_mask = None;
            XRenderSetPictureTransform(dpy, draw_pict, &identity);
            XRenderSetPictureFilter(dpy, draw_pict, FilterNearest, NULL, 0);
            XRenderChangePicture(dpy, draw_pict, CPClipMask, &pa);
        }
        int dx = w->x, dy = w->y;
        int dw = w->w, dh = w->h;
        if (layout_visual) {
            double lf = animation_value(&w->layout, w->id, w->generation, now);
            dx = w->layout_x + (int)((w->layout_target_x - w->layout_x) * lf);
            dy = w->layout_y + (int)((w->layout_target_y - w->layout_y) * lf);
            fading = 1;
        }
        if (dw <= 0 || dh <= 0) continue;
        if (w->shaped && w->n_clip <= 0) continue; /* empty bounding shape */
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
            XRenderComposite(dpy, PictOpOver, draw_pict, None, back_pict,
                0, 0, 0, 0, dx, dy, (unsigned)fw, (unsigned)fh);
        } else {
            Picture mask = alpha_mask(a);
            if (mask) {
                XRenderComposite(dpy, PictOpOver, draw_pict, mask, back_pict,
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
                XRenderComposite(dpy, PictOpOver, blk, draw_pict, back_pict,
                    0, 0, 0, 0, dx, dy, (unsigned)fw, (unsigned)fh);
        }
        if (had_clip)
            back_clip_reset(); /* trả clip về hộp damage (hoặc None) */
        /* Candidate only: closing_detach drains outstanding asynchronous
         * errors before it transfers these references out of live state. */
        if ((opt_close_ms > 0 || opt_workspace_ms > 0 || opt_overview_ms > 0) &&
            opt_fade && !opt_reduce_motion)
            w->painted = 1;
    }
    /* Missing/destroyed stacking anchors cancel rather than float to top. */
    if (exits.count) for (unsigned i = 0; i < EXIT_COUNT; i++)
        if (exits.layers[i].bytes && !exits_drawn[i]) exit_drop(&exits, i, exit_release);
    /* Mỗi Present slot phải nhận snapshot ĐẦY ĐỦ từ canonical back_buf:
     * slot idle có thể chứa frame cũ nhiều lượt; chỉ copy bbox sẽ làm các
     * vùng ngoài damage quay về nội dung cũ khi driver flip cả pixmap.
     * update vẫn là bbox, offset luôn 0 (root/pixmap cùng hệ toạ độ). */
#ifdef WITH_XPRESENT
    if (present_ok) {
        int slot = present_free_slot(); /* repaint_due đã chờ slot idle */
        if (slot < 0) return; /* phòng thủ: không ghi vào pixmap busy */
        if (present_bufs[slot].pixmap == None)
            present_bufs[slot].pixmap = XCreatePixmap(dpy, root,
                (unsigned)ra.width, (unsigned)ra.height, (unsigned)ra.depth);
        if (present_bufs[slot].pixmap == None) {
            present_disable();
        } else {
            Pixmap target = present_bufs[slot].pixmap;
            XCopyArea(dpy, back_buf, target, present_gc,
                0, 0, (unsigned)ra.width, (unsigned)ra.height, 0, 0);
            XserverRegion update = None;
            if (cur_clip_on)
                update = XFixesCreateRegion(dpy, &cur_clip, 1);
            uint32_t serial = present_serial++;
            XPresentPixmap(dpy, root, target, serial, None, update,
                0, 0, 0, None, None, PresentOptionNone, 0, 0, 0, NULL, 0);
            if (update != None) XFixesDestroyRegion(dpy, update);
            present_pending_serial = serial;
            present_pending = 1;
            present_sent_ms = now_ms();
            present_bufs[slot].serial = serial;
            present_bufs[slot].busy = 1;
            present_bufs[slot].sent_ms = present_sent_ms;
        }
    }
#endif
    if (!present_ok) {
        if (cur_clip_on)
            XCopyArea(dpy, back_buf, root, present_gc,
                cur_clip.x, cur_clip.y, cur_clip.width, cur_clip.height,
                cur_clip.x, cur_clip.y);
        else
            XCopyArea(dpy, back_buf, root, present_gc,
                0, 0, (unsigned)ra.width, (unsigned)ra.height, 0, 0);
    }
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
    /* Never perform Xlib cleanup from an error callback. Drain at loop top. */
    for (unsigned i = 0; i < EXIT_COUNT; i++) {
        ExitLayer *s = &exits.layers[i];
        if (s->bytes && (e->resourceid == s->pixmap || e->resourceid == s->picture))
            exit_xerror = 1;
    }
    for (uint32_t i = 0; i < ws_scene.count; i++) {
        ExitLayer *s = &ws_scene.layers[i];
        if (s->bytes && (e->resourceid == s->pixmap || e->resourceid == s->picture))
            ws_xerror = 1;
    }
    for (Win *w = wins; w; w = w->next)
        if (e->resourceid == w->id || e->resourceid == w->pixmap || e->resourceid == w->pict)
            w->painted = 0;
    /* BadWindow, BadDamage và RenderBadPicture là race bình thường: cửa
     * sổ hủy ở server kéo theo Damage/pixmap redirect/Picture chết cùng.
     * Entry zombie còn lại tới gc_sweep nên repaint composite bằng picture
     * chết (RenderBadPicture) và cleanup XDamageDestroy dính BadDamage —
     * server bỏ qua các request này. Chỉ log ở debug mode. Log mọi lỗi
     * khác (BadMatch, BadPictOp... = bug thật). */
    if (e->error_code != BadWindow &&
        e->error_code != dmg_error + BadDamage &&
        e->error_code != render_errbase + BadPicture) {
        char msg[128] = "";
        XGetErrorText(dpy, e->error_code, msg, sizeof(msg));
        fprintf(stderr, "dani-comp: Xerror %s req=%d/%d res=0x%lx\n",
            msg, (int)e->request_code, (int)e->minor_code, (unsigned long)e->resourceid);
    } else if (opt_debug) {
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
        "         [--dim 0.5..1] [--fade-ms 0..1000] [--close-ms 0..1000]\n"
        "         [--reduce-motion] [--wm-settings] [-v] [-h]\n"
        "  --dim 0.92  độ sáng cửa sổ inactive (0 hoặc 1 = tắt dim)\n", prog);
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
            if (opt_dim <= 0.0) opt_dim = 1.0; /* 0 = tắt dim (khỏi bẫy clamp 0.5) */
            if (opt_dim < 0.5) opt_dim = 0.5;
            if (opt_dim > 1.0) opt_dim = 1.0;
        } else if ((!strcmp(argv[i], "--fade-ms") || !strcmp(argv[i], "--fade-time")) && i + 1 < argc) {
            char *end;
            const char *value = argv[++i];
            errno = 0;
            long duration = strtol(value, &end, 10);
            if (!*value || end == value || *end || errno || duration < 0 || duration > 1000) {
                fprintf(stderr, "dani-comp: bad fade duration '%s' (want integer 0..1000)\n", value);
                return 1;
            }
            opt_fade_ms = (int)duration;
        } else if (!strcmp(argv[i], "--close-ms") && i + 1 < argc) {
            char *end;
            const char *value = argv[++i];
            errno = 0;
            long duration = strtol(value, &end, 10);
            if (!*value || end == value || *end || errno || duration < 0 || duration > 1000) {
                fprintf(stderr, "dani-comp: bad close duration '%s' (want integer 0..1000)\n", value);
                return 1;
            }
            opt_close_ms = (int)duration;
        } else if (!strcmp(argv[i], "--reduce-motion")) {
            opt_reduce_motion = opt_force_reduce_motion = 1;
        } else if (!strcmp(argv[i], "--wm-settings")) {
            opt_wm_settings = 1;
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
            render_errbase = err2; /* cho xerror_ignore (RenderBadPicture) */
        }
        /* tùy chọn: XShape cho shaped window (XFixes không cần nữa) */
        {
            int err3;
            shape_ok = XShapeQueryExtension(dpy, &shape_event, &err3);
            if (opt_verbose)
                fprintf(stderr, "dani-comp: shape=%d\n", shape_ok);
        }
#ifdef WITH_XPRESENT
        /* tùy chọn: XPresent cho hardware vsync (events là XGE cookies:
         * cần opcode + evtype, không phải event base kiểu ShapeNotify) */
        {
            int err4;
            present_ok = XPresentQueryExtension(dpy, &present_opcode, &present_event, &err4);
            if (getenv("DANI_COMP_NO_PRESENT")) present_ok = 0;
            if (opt_verbose)
                fprintf(stderr, "dani-comp: present=%d opcode=%d event=%d\n",
                    present_ok, present_opcode, present_event);
        }
#endif
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
    A_WM_WINTYPE_DIALOG = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE_DIALOG", False);
    A_XROOTPMAP = XInternAtom(dpy, "_XROOTPMAP_ID", False);
    A_XSETROOT = XInternAtom(dpy, "_XSETROOT_ID", False);
    A_ANIMATION_CONTROL = XInternAtom(dpy, ANIMATION_CONTROL_ATOM, False);
    A_LAYOUT_SETTINGS = XInternAtom(dpy, LAYOUT_ANIMATION_SETTINGS_ATOM, False);
    A_LAYOUT_TRANSACTION = XInternAtom(dpy, LAYOUT_ANIMATION_ATOM, False);
    A_HIDE_REASON = XInternAtom(dpy, ANIMATION_REASON_ATOM, False);
    A_CURRENT_DESKTOP = XInternAtom(dpy, "_NET_CURRENT_DESKTOP", False);
    A_CLOSE_SETTINGS = XInternAtom(dpy, ANIMATION_CLOSE_ATOM, False);
    A_WM_CHECK = XInternAtom(dpy, "_NET_SUPPORTING_WM_CHECK", False);
    A_WS_SETTINGS = XInternAtom(dpy, WORKSPACE_SETTINGS_ATOM, False);
    A_WS_TXN = XInternAtom(dpy, WORKSPACE_ANIMATION_ATOM, False);
    A_MON_EPOCH = XInternAtom(dpy, MONITOR_EPOCH_ATOM, False);
    A_OV_SETTINGS = XInternAtom(dpy, OVERVIEW_SETTINGS_ATOM, False);
    A_OV_ROLE = XInternAtom(dpy, OVERVIEW_ROLE_ATOM, False);
    int formats_n;
    XPixmapFormatValues *formats = XListPixmapFormats(dpy, &formats_n);
    if (formats) {
        for (int i = 0; i < formats_n; i++) if (formats[i].depth > 0 && formats[i].depth <= 32) {
            exit_bpp[formats[i].depth] = (unsigned)formats[i].bits_per_pixel;
            exit_pad[formats[i].depth] = (unsigned)formats[i].scanline_pad;
        }
        XFree(formats);
    }
    if (opt_wm_settings) {
        A_ANIMATION_SETTINGS = XInternAtom(dpy, ANIMATION_SETTINGS_ATOM, False);
        A_WM_CHECK = XInternAtom(dpy, "_NET_SUPPORTING_WM_CHECK", False);
        opt_fade = 0; /* no valid WM settings -> instant, never CLI fallback */
    }
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
            char *cmname = XGetAtomName(dpy, A_CM);
            fprintf(stderr, "dani-comp: cannot own %s\n", cmname ? cmname : "?");
            if (cmname) XFree(cmname);
            return 1;
        }
    }

    {
        unsigned long version = ANIMATION_SETTINGS_VERSION;
        XChangeProperty(dpy, cm_win, A_ANIMATION_CONTROL, XA_CARDINAL, 32,
            PropModeReplace, (unsigned char *)&version, 1);
    }
    XSelectInput(dpy, root,
        SubstructureNotifyMask | PropertyChangeMask | StructureNotifyMask | ExposureMask);
    animation_settings_read();
    reason_owner = opt_wm_settings ? settings_owner : opt_close_ms > 0 ? settings_wm() : None;

#ifdef WITH_XPRESENT
    /* XPresent: đăng ký nhận events trên root */
    if (present_ok) {
        present_eid = XPresentSelectInput(dpy, root,
            PresentCompleteNotifyMask | PresentIdleNotifyMask);
        present_ok = (present_eid != None);
    }
#endif

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
            exits_clear();
            /* trả redirect về cho server: không thì cửa sổ đông cứng
             * offscreen sau khi compositor chết. Xóa frame composite
             * khỏi root để server vẽ lại cửa sổ thật. */
            for (int i = 0; i < 7; i++)
                if (shadow_fill[i] != None) XRenderFreePicture(dpy, shadow_fill[i]);
            if (present_gc != None) XFreeGC(dpy, present_gc);
            if (dmg_scratch != None) XFixesDestroyRegion(dpy, dmg_scratch);
            XCompositeUnredirectSubwindows(dpy, root, CompositeRedirectManual);
            XClearArea(dpy, root, 0, 0, 0, 0, True);
            XFlush(dpy);
            XCloseDisplay(dpy);
            return 0;
        }
        /* P2: tối đa 1 repaint / FRAME_INTERVAL_MS. Đủ thời gian và có việc -> vẽ ngay.
         * (repaint_due cũng chặn khi flip XPresent đang chờ -> hết chớp.) */
        if (repaint_due()) { repaint(); continue; }
        /* Lịch chờ: dirty/fading -> tick tới mốc FRAME_INTERVAL_MS; P6 shape recheck ->
         * tick tới mốc 100/400ms; không có gì -> block tới event. */
        struct timeval tv, *tvp = NULL;
        long budget = -1; /* us; -1 = block vô hạn */
        if (dirty || fading) {
            long long elapsed = now_ms() - last_paint;
#ifdef WITH_XPRESENT
            if (!present_ok || (!present_pending && present_free_slot() >= 0))
#endif
                budget = (elapsed >= FRAME_INTERVAL_MS) ? 0 :
                    (long)((FRAME_INTERVAL_MS - elapsed) * 1000);
        }
#ifdef WITH_XPRESENT
        /* Complete và thiếu Idle đều có deadline, kể cả khi không dirty.
         * Không poll 0us trong lúc chờ GPU; event vẫn đánh thức select. */
        long long pwait = present_wait_ms(now_ms());
        if (pwait >= 0 && (budget < 0 || pwait * 1000 < budget))
            budget = (long)(pwait * 1000);
#endif
        long long swake = shape_next_wake(now_ms());
        if (swake >= 0) {
            long sw_us = (swake > 1000000) ? 1000000 : (long)(swake * 1000);
            if (budget < 0 || sw_us < budget) budget = sw_us;
        }
        /* Retention deadlines are independent of Present completion/idle.
         * Even a stalled backend must release images at duration/TTL. */
        if (exits.count) for (unsigned i = 0; i < EXIT_COUNT; i++) {
            ExitLayer *s = &exits.layers[i];
            if (!s->bytes) continue;
            long long end = s->animation.start_ms + s->animation.duration_ms;
            if (s->retained + EXIT_TTL_MS < end) end = s->retained + EXIT_TTL_MS;
            long long left = end - now_ms();
            if (left < 0) left = 0;
            if (budget < 0 || left * 1000 < budget) budget = (long)(left * 1000);
        }
        if (ws_scene.active) {
            long long left = ws_scene.prepared + EXIT_TTL_MS - now_ms();
            if (left < 0) left = 0;
            if (budget < 0 || left * 1000 < budget) budget = (long)(left * 1000);
        }
        /* GC zombie window chết sót: wake tối đa 5s để quét 1 lần. */
        {
            long long gdue = last_gc + 5000 - now_ms();
            if (gdue < 0) gdue = 0;
            long g_us = (gdue > 5000000) ? 5000000 : (long)(gdue * 1000);
            if (budget < 0 || g_us < budget) budget = g_us;
        }
        if (budget >= 0) {
            tv.tv_sec = budget / 1000000;
            tv.tv_usec = budget % 1000000;
            tvp = &tv;
        }
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
                if (repaint_due()) repaint();
                continue;
            }
        }
        while (XPending(dpy)) {
            /* Event storms must not postpone resource expiry until paint. */
            if (exits.count) exits_maintain(now_ms());
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
                    /* Xóa region TRƯỚC subtract: nếu subtract dính BadDamage
                     * (window đã destroy, damage đã chết — xem log Xerror),
                     * region giữ nguyên rỗng thay vì giữ rect CŨ của lần
                     * subtract trước — tránh add nhầm damage stale gây repaint
                     * bậy đè lên vùng khác (tray/terminal). */
                    XFixesSetRegion(dpy, dmg_scratch, NULL, 0);
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
                if (e->event != root || e->window == cm_win) break;
                if (opt_debug) fprintf(stderr, "dani-comp: Map 0x%lx\n", (unsigned long)e->window);
                if (exits.count) exit_cancel_identity(&exits, e->window, exit_release);
                if (ws_member(e->window) >= 0) ws_scene_clear();
                Win *prior = win_get(e->window);
                int hidden = prior && prior->hide_reason;
                Win *w = win_add(e->window);
                if (w) {
                    w->generation = next_generation == UINT64_MAX ? 0 : ++next_generation;
                    w->hide_reason = 0;
                }
                /* override (bar/menu) không fade: win_add đã đặt born=0,
                 * đừng ghi đè thành now */
                if (w) {
                    if (!w->override) w->born = now_ms();
                    win_free_pix(w);
                    overview_mark(w);
                    /* A new overview replaces any old overview image. */
                    if (w->overview) for (unsigned i = 0; i < EXIT_COUNT; i++)
                        if (exits.layers[i].overview) exit_drop(&exits, i, exit_release);
                    if (hidden) animation_cancel(&w->opening);
                    else opening_start(w, now_ms());
                }
                dirty = 1; stack_dirty = 1;
                break;
            }
            case UnmapNotify: {
                XUnmapEvent *e = &ev.xunmap;
                if (e->event != root || e->send_event) break;
                if (opt_debug) fprintf(stderr, "dani-comp: Unmap 0x%lx tracked=%d\n",
                    (unsigned long)e->window, win_get(e->window) != NULL);
                Win *w = win_get(e->window);
                if (w && w->mapped) {
                    if (ws_scene.active) ws_retain(w, now_ms());
                    if (w->overview) overview_detach(w, now_ms());
                    closing_detach(w, now_ms());
                    w->mapped = 0;
                    animation_cancel(&w->opening);
                    win_free_pix(w);
                }
                dirty = 1; stack_dirty = 1;
                break;
            }
            case DestroyNotify: {
                if (ev.xdestroywindow.event != root && ev.xdestroywindow.window != reason_owner &&
                    ev.xdestroywindow.window != settings_owner) break;
                if (ev.xdestroywindow.window == reason_owner) {
                    reason_owner = None; reason_sequence = 0;
                    exits_clear();
                }
                if (opt_wm_settings && ev.xdestroywindow.window == settings_owner)
                    animation_settings_read();
                int member = ws_member(ev.xdestroywindow.window);
                if (member >= 0 && !ws_scene.layers[member].bytes) ws_scene_clear();
                Win *w = win_get(ev.xdestroywindow.window);
                if (w && w->mapped) closing_detach(w, now_ms());
                win_del(ev.xdestroywindow.window);
                dirty = 1; stack_dirty = 1;
                break;
            }
            case CreateNotify:
                /* An XID beginning another lifetime cannot inherit a ghost. */
                if (exits.count) exit_cancel_identity(&exits, ev.xcreatewindow.window, exit_release);
                if (ws_member(ev.xcreatewindow.window) >= 0) ws_scene_clear();
                break;
            case ReparentNotify: {
                XReparentEvent *e = &ev.xreparent;
                if (exits.count) exit_cancel_identity(&exits, e->window, exit_release);
                if (ws_member(e->window) >= 0) ws_scene_clear();
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
                    exits_clear();
                    for (Win *w = wins; w; w = w->next) layout_release(w);
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
                if (e->window == root && e->atom == A_WM_CHECK) {
                    Window owner = opt_wm_settings || opt_close_ms > 0 ? settings_wm() : None;
                    if (owner != reason_owner) reason_sequence = 0;
                    reason_owner = owner;
                    exits_clear();
                    for (Win *w = wins; w; w = w->next) w->hide_reason = 0;
                    animation_settings_read();
                } else if (opt_wm_settings && e->window == root &&
                    (e->atom == A_ANIMATION_SETTINGS || e->atom == A_CLOSE_SETTINGS ||
                     e->atom == A_LAYOUT_SETTINGS || e->atom == A_WS_SETTINGS ||
                     e->atom == A_OV_SETTINGS)) {
                    animation_settings_read();
                } else if (e->window == root && e->atom == A_LAYOUT_TRANSACTION) {
                    layout_transaction_read();
                } else if (e->window == root && e->atom == A_CURRENT_DESKTOP) {
                    /* Only the desktop its PREPARE named commits a scene. */
                    if (ws_scene.active && !ws_scene.committed &&
                        ws_desktop() == (long)ws_scene.to) {
                        exits_clear_layers(KEEP_OVERVIEW);
                        ws_scene.committed = 1;
                        ws_admit(now_ms());
                    } else {
                        /* An overview commit fades the overview over the new desktop. */
                        if (ws_scene.active) ws_scene_clear();
                        exits_clear_layers(KEEP_OVERVIEW);
                    }
                    for (Win *w = wins; w; w = w->next) layout_release(w);
                } else if (e->window == root && e->atom == A_MON_EPOCH) {
                    exits_clear();
                } else if (e->window == root && e->atom == A_ACTIVE) {
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
            case ClientMessage:
                if (ev.xclient.window == root && ev.xclient.message_type == A_LAYOUT_TRANSACTION &&
                    ev.xclient.format == 32 && ev.xclient.data.l[0] == LAYOUT_ANIMATION_VERSION &&
                    (Window)ev.xclient.data.l[1] == settings_owner &&
                    (uint32_t)ev.xclient.data.l[2] > layout_request.sequence)
                    layout_transaction_read();
                if (ev.xclient.window == root && ev.xclient.message_type == A_WS_TXN &&
                    ev.xclient.format == 32 &&
                    ev.xclient.data.l[0] == WORKSPACE_ANIMATION_VERSION && settings_owner &&
                    (Window)ev.xclient.data.l[1] == settings_owner)
                    ws_prepare((uint32_t)ev.xclient.data.l[2]);
                if (ev.xclient.window == root && ev.xclient.message_type == A_HIDE_REASON &&
                    ev.xclient.format == 32) {
                    unsigned long words[5];
                    for (int i = 0; i < 5; i++) words[i] = (uint32_t)ev.xclient.data.l[i];
                    if (animation_reason_valid(words, reason_owner, reason_sequence)) {
                        reason_sequence = (uint32_t)words[3];
                        Win *w = win_get(words[2]);
                        if (w) w->hide_reason = (int)words[4];
                        /* A hide invalidates close visuals: never let a recently
                         * closed app follow a workspace switch. Reload ends all. */
                        if (words[4] == ANIMATION_HIDE_RELOAD) exits_clear();
                        else exits_clear_layers(KEEP_WORKSPACE | KEEP_OVERVIEW);
                    }
                }
                if (ev.xclient.window == cm_win && ev.xclient.message_type == A_ANIMATION_CONTROL &&
                    ev.xclient.format == 32 && ev.xclient.data.l[0] == ANIMATION_SETTINGS_VERSION)
                    quit_req = 1;
                break;
            case SelectionClear:
                if (ev.xselectionclear.selection == A_CM) {
                    fprintf(stderr, "dani-comp: replaced, exiting\n");
                    exits_clear();
                    for (int i = 0; i < 7; i++)
                        if (shadow_fill[i] != None) XRenderFreePicture(dpy, shadow_fill[i]);
                    if (present_gc != None) XFreeGC(dpy, present_gc);
                    if (dmg_scratch != None) XFixesDestroyRegion(dpy, dmg_scratch);
                    XCompositeUnredirectSubwindows(dpy, root, CompositeRedirectManual);
                    XClearArea(dpy, root, 0, 0, 0, 0, True);
                    XFlush(dpy);
                    XCloseDisplay(dpy);
                    return 0;
                }
                break;
            case GenericEvent: {
#ifdef WITH_XPRESENT
                /* PresentCompleteNotify là XGE (type=35): complete về dạng
                 * cookie, không phải plain event — check ev.type ==
                 * present_event + ... kiểu ShapeNotify là dead code. */
                XGenericEventCookie *cook = &ev.xcookie;
                if (present_ok && cook->extension == present_opcode &&
                    XGetEventData(dpy, cook)) {
                    if (cook->evtype == PresentCompleteNotify) {
                        XPresentCompleteNotifyEvent *pe =
                            (XPresentCompleteNotifyEvent *)cook->data;
                        present_complete(pe);
                        if (opt_debug >= 2)
                            fprintf(stderr, "dani-comp: present complete serial=%u ust=%llu msc=%llu mode=%u\n",
                                pe->serial_number, (unsigned long long)pe->ust,
                                (unsigned long long)pe->msc, pe->mode);
                    } else if (cook->evtype == PresentIdleNotify) {
                        XPresentIdleNotifyEvent *pe =
                            (XPresentIdleNotifyEvent *)cook->data;
                        present_buffer_idle(pe);
                    }
                    XFreeEventData(dpy, cook);
                }
#endif
                break;
            }
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
        /* P2: throttle ở đầu vòng giữ nhịp <=~120Hz. Đủ thời gian thì vẽ; chưa đủ
         * thì để dirty, vòng sau tick nốt phần còn lại. */
        if (repaint_due()) repaint();
    }
    return 0;
}
