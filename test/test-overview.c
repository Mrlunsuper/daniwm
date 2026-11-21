/* Offline white-box tests: execute the production implementation, never XOpenDisplay. */
#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/state.h"
#include "../src/overview.h"
#include "../src/client.h"
#include "../src/monitor.h"
#include "../src/bar.h"
#include "../src/xerr.h"
#include <X11/keysym.h>
#include <X11/extensions/Xcomposite.h>
#include <X11/extensions/Xrender.h>

static int pixmaps, windows, gcs, draws, grabs, ungrabs, views, focuses;
static int trapped, require_trap;
Drag drag;
static int hover_arms;
void hover_lock_arm(void) { hover_arms++; }
static unsigned char rendered[512];
static int rendered_len;
static XEvent queued;
static int queue_count, copies, check_cells;
static Window last_raised;
static KeySym keymap[256]={[36]=XK_Return,[104]=XK_KP_Enter,[113]=XK_Left,[114]=XK_Right,[111]=XK_Up,[116]=XK_Down,[9]=XK_Escape,[32]=XK_o,[53]=XK_x};
static void copy_area(Pixmap src, unsigned w, unsigned h, int y) {
    if (src>1 && w<=192 && h<=108) { assert(h>0); if (check_cells) assert(y%240+h<=236); copies++; }
}
static void record_text(const FcChar8 *s, int n) { assert(n>=0 && n<=512); memcpy(rendered,s,n); rendered_len=n; }
static int free_pixmap(void) { assert(!require_trap || trapped); return --pixmaps; }
static int grab_result=GrabSuccess, trap_ok=1, compositor=1, draw_ok=1;
static unsigned long serial=100;
static XftFont font;
static Screen fake_screen;
static _XPrivDisplay fake_display;
Display *dpy;
Window root=1;
int screen, curws, NWS=4, barcol_ok=1;
unsigned int MOD=Mod4Mask;
unsigned long BAR_BG, BAR_ACC, BAR_DIM;
XftFont *barfont=&font;
BarColors barcol;
Client *clients;
struct MonitorGeom mons[MAXMONS]={{0,0,1000,800}};
Client *find(Window w) { for(Client *c=clients;c;c=c->next) if(c->win==w) return c; return NULL; }
void view(int ws) { assert(!overview_is_open()); curws=ws; views++; }
void focus(Client *c) { assert(c && c->ws==curws); focuses++; }
int mon_by_pointer(void) { return 0; }
const char *ws_name(int ws) { (void)ws; return "workspace"; }
void trap_errors(Display *d) { (void)d; assert(!trapped); trapped=1; }
int untrap_errors(Display *d) { (void)d; assert(trapped); trapped=0; return trap_ok; }
/* All server-facing calls are replaced, including rendering and keyboard lookup. */
#undef DefaultVisual
#undef DefaultDepth
#undef DefaultColormap
#define DefaultVisual(d,s) ((Visual *)1)
#define DefaultDepth(d,s) 24
#define DefaultColormap(d,s) 1
#define XInternAtom(...) 1
#define XGetSelectionOwner(...) (compositor ? 2 : None)
#define XCreateWindow(...) (windows++, ++serial)
#define XDestroyWindow(...) (windows--, 0)
#define XCreatePixmap(...) (pixmaps++, ++serial)
#define XFreePixmap(...) (free_pixmap(), 0)
#define XCreateGC(...) (gcs++, (GC)1)
#define XFreeGC(...) (gcs--, 0)
#define XftDrawCreate(...) (draw_ok ? (draws++, (XftDraw *)1) : NULL)
#define XftDrawDestroy(...) ((void)draws--)
#define XGrabKeyboard(...) (grabs++, grab_result)
#define XUngrabKeyboard(...) (ungrabs++, 0)
#define XMapRaised(...) 0
#define XRaiseWindow(d,w) (last_raised=(w),0)
#define XSetForeground(...) 0
#define XFillRectangle(...) 0
#define XDrawRectangle(...) 0
#define XCopyArea(d,s,t,g,sx,sy,w,h,x,y) copy_area(s,w,h,y)
#define XftDrawSetClipRectangles(...) ((void)0)
#define XftDrawStringUtf8(d,c,f,x,y,s,n) record_text(s,n)
#define XftDrawSetClip(...) ((void)0)
#define XGetWindowProperty(...) 1
#define XFree(p) free(p)
#define XLookupKeysym(e,i) keymap[(e)->keycode]
#define XEventsQueued(...) queue_count
#define XPeekEvent(d,e) (*(e)=queued,0)
#define XGetWindowAttributes(d,id,a) ((a)->map_state=IsViewable,(a)->width=640,(a)->height=480,(a)->visual=(Visual *)1,1)
#define XRenderFindVisualFormat(...) ((XRenderPictFormat *)1)
#define XCompositeNameWindowPixmap(...) (pixmaps++, ++serial)
#define XGetGeometry(d,p,r,x,y,w,h,b,dep) (*(w)=192,*(h)=108,1)
#define XRenderCreatePicture(...) (++serial)
#define XRenderFreePicture(...) ((void)0)
#define XRenderFillRectangle(...) ((void)0)
#define XRenderSetPictureTransform(...) ((void)0)
#define XRenderSetPictureFilter(...) ((void)0)
#define XRenderComposite(...) ((void)0)
#include "../src/overview.c"

#include "../src/ewmh.h"
int nmons=1, gaps_on, gap_inner, BORDER;
static Layout layouts[MAXWS]; static int masters[MAXWS]; static float mfacts[MAXWS];
Layout *ws_layout=layouts; int *ws_nmaster=masters; float *ws_mfact=mfacts;
unsigned long BORDER_FOCUS, BORDER_NORMAL;
Client *sel;
Dock *docks;
#define XQueryTree(...) 0
#define XRestackWindows(...) 0
#define XMoveResizeWindow(...) 0
#define XMapWindow(...) 0
#define XUnmapWindow(...) 0
#define XSetWindowBorderWidth(...) 0
#define XSetWindowBorder(...) 0
#define XSync(...) 0
#define getarea(m,x,y,w,h) (*(x)=0,*(y)=0,*(w)=1280,*(h)=720)
#define ewmh_update_state(...) ((void)0)
#define drawbar() ((void)0)
#include "../src/layout.c"

/* Link the extracted production unmanage body to offline lifecycle stubs. */
static Client *saved_sel[MAXWS];
Client **ws_sel=saved_sel;
Time last_evtime;
static void test_detach(Client *c) { assert(clients==c); clients=c->next; }
#define detach test_detach
#define XUngrabPointer(...) 0
#define XDeleteProperty(...) 0
#define XSetInputFocus(...) 0
#define ewmh_set_wm_state(...) ((void)0)
#define ewmh_client_list(...) ((void)0)
#define ewmh_active(...) ((void)0)
#include "unmanage.h"
#undef detach

static void clean(void) {
    overview_hide(0);
    for(int i=0;i<CACHE;i++) overview_drop(cache[i].id);
    assert(!pixmaps && !windows && !gcs && !draws);
}
static void key(KeySym k) {
    XEvent e={0}; e.type=KeyPress;
    for (int i=0;i<256;i++) if (keymap[i]==k) e.xkey.keycode=i;
    assert(e.xkey.keycode); e.xkey.state=MOD;
    assert(overview_event(&e));
}
static void click_card(Window id) {
    for(int i=nhits-1;i>=0;i--) if(hits[i].id==id) {
        XEvent e={0}; e.type=ButtonPress; e.xbutton.window=overlay;
        e.xbutton.x=hits[i].x; e.xbutton.y=hits[i].y;
        assert(overview_event(&e)); return;
    }
    assert(!"missing card");
}
int main(void) {
    /* Fake display also makes accidental Default* macro dereferences harmless. */
    fake_display=calloc(1,sizeof *fake_display); assert(fake_display);
    fake_display->screens=&fake_screen; dpy=(Display *)fake_display;
    clean(); clean();
    compositor=0;
    drag.win=42;
    overview_show(0);
    assert(!overview_is_open());
    XEvent release={0}; release.type=ButtonRelease;
    assert(!overview_event(&release)); /* normal dispatch can end the drag */
    assert(drag.win==42);
    drag.win=None;
    overview_show(0); assert(overview_is_open());
    int g=grabs; unsigned long s=serial;
    overview_show(0); assert(grabs==g && serial==s);
    overview_select(2); assert(overview_selection()==2 && curws==0 && views==0 && focuses==0);
    overview_select(-1); overview_select(NWS); assert(overview_selection()==2);
    int arms=hover_arms;
    commit(None); assert(curws==2 && views==1 && !overview_is_open());
    assert(hover_arms==arms+1);
    assert(overview_selection()==-1);
    int u=ungrabs; clean(); clean(); assert(ungrabs==u);
    grab_result=AlreadyGrabbed; overview_show(0); assert(!overview_is_open()); clean();
    grab_result=GrabSuccess; trap_ok=0; g=grabs; overview_show(0);
    assert(!overview_is_open() && grabs==g); clean(); trap_ok=1;
    draw_ok=0; overview_show(0); assert(!overview_is_open()); clean(); draw_ok=1;
    Client c={0}; c.win=42; c.ws=1; clients=&c;
    overview_show(0); clients=NULL; click_card(42); assert(curws==1 && focuses==0); clean();
    clients=&c; overview_show(0); c.ws=3; click_card(42); assert(curws==1 && focuses==0); clean();
    c.ws=1; overview_show(0); click_card(42); assert(focuses==1); clean();
    compositor=1;
    for(int i=0;i<CACHE+17;i++) { c.win=1000+i; overview_capture(c.win); }
    assert(pixmaps==CACHE);
    c.win=1000+CACHE+16; overview_capture(c.win); assert(pixmaps==CACHE);
    overview_drop(c.win); assert(pixmaps==CACHE-1); overview_drop(c.win); assert(pixmaps==CACHE-1);
    trap_ok=0; require_trap=1; c.win=9000; overview_capture(c.win); assert(pixmaps==CACHE-1); trap_ok=1; require_trap=0;
    clean(); clients=NULL;
    overview_show(0); XEvent e={0}; e.type=GenericEvent; assert(!overview_event(&e));
    key(XK_x);
    overview_select(0); key(XK_Right); assert(pending==1);
    key(XK_Down); assert(pending==3); key(XK_Up); key(XK_Left); assert(pending==0);
    arms=hover_arms;
    key(XK_Escape); assert(!active);
    assert(hover_arms==arms+1);
    overview_hide(0); assert(hover_arms==arms+1);
    overview_show(0); key(XK_o); assert(!active);
    overview_show(0); overview_select(2);
    int before_views=views, before_ungrabs=ungrabs;
    key(XK_Return); key(XK_Return);
    assert(active && views==before_views && ungrabs==before_ungrabs);
    e.type=KeyRelease; e.xkey.keycode=36; e.xkey.time=100;
    queued=e; queued.type=KeyPress; queue_count=1;
    assert(overview_event(&e)); assert(active);
    assert(overview_event(&queued)); assert(views==before_views);
    queue_count=0; e.xkey.time=200; assert(overview_event(&e));
    assert(!active && views==before_views+1 && ungrabs==before_ungrabs+1);
    assert(!overview_event(&e));
    overview_show(0); key(XK_KP_Enter); key(XK_Escape); assert(!active && !commit_key);
    overview_show(0);
    text(0,20,100,"ok\xc0\xaf\xed\xa0\x80\xf4\x90\x80\x80");
    assert(rendered_len==11 && !memcmp(rendered,"ok?????????",11));
    char bounded[513]; memset(bounded,'a',sizeof bounded); bounded[511]='\xe2'; bounded[512]=0;
    text(0,20,100,bounded); assert(rendered_len==512 && rendered[511]=='?');
    clean();
    Client full={0}, fl={0}; Dock dock={0};
    full.win=400; full.ws=curws; full.fullscreen=1; full.next=&fl;
    fl.win=401; fl.ws=curws; fl.floating=1; dock.win=402;
    clients=&full; docks=&dock;
    overview_show(0); arrange(); assert(last_raised==overlay);
    overview_hide(0); last_raised=0; arrange(); assert(last_raised==full.win);
    clean(); clients=NULL; docks=NULL;
    NWS=9; mons[0].w=1280; mons[0].h=720;
    Client nine[9]={0};
    for (int i=0;i<9;i++) { nine[i].win=200+i; nine[i].ws=i; nine[i].next=i<8?&nine[i+1]:NULL; }
    clients=nine; copies=0; check_cells=1; overview_show(0); assert(copies==9); clean(); clients=NULL; check_cells=0;
    mons[0].w=1; mons[0].h=1;
    overview_show(0); assert(overview_is_open());
    for(int i=0;i<nhits;i++) assert(hits[i].w>0 && hits[i].h>0);
    clean();
    overview_show(0);
    int selection=overview_selection();
    unmanage(999999); /* tooltip or stale overlay DestroyNotify */
    assert(overview_is_open() && overview_selection()==selection);
    Client *removed=calloc(1,sizeof *removed); assert(removed);
    removed->win=12345; removed->ws=curws; clients=removed; sel=removed;
    unmanage(removed->win);
    assert(!overview_is_open() && !clients && !sel);
    clean(); free(fake_display);
    puts("overview offline tests: OK");
    return 0;
}
