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
static int queue_count, copies;
static struct { Pixmap dst; int x,y,w,h; } copy_rects[1024];
static int ncopy;
static char text_log[16384];
static Window last_raised;
static KeySym keymap[256]={[36]=XK_Return,[104]=XK_KP_Enter,[113]=XK_Left,[114]=XK_Right,[111]=XK_Up,[116]=XK_Down,[9]=XK_Escape,[32]=XK_o,[53]=XK_x};
static void copy_area(Pixmap src, Pixmap dst, unsigned w, unsigned h, int x, int y) {
    assert(w>0 && h>0);
    if (src>1 && w<=192 && h<=108) copies++;
    if (ncopy<1024) {
        copy_rects[ncopy].dst=dst; copy_rects[ncopy].x=x; copy_rects[ncopy].y=y;
        copy_rects[ncopy].w=(int)w; copy_rects[ncopy++].h=(int)h;
    }
}
static int fill_rect(unsigned w, unsigned h) { assert(w>0 && h>0); return 0; }
static void record_text(const FcChar8 *s, int n) {
    assert(n>=0 && n<=512); memcpy(rendered,s,n); rendered_len=n;
    size_t used=strlen(text_log);
    if (used+(size_t)n+2<sizeof text_log) {
        memcpy(text_log+used,s,n); text_log[used+n]='\n'; text_log[used+n+1]=0;
    }
}
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
unsigned long BAR_BG=0x1e1e2e, BAR_FG=0xcdd6f4, BAR_ACC=0x7aa2f7, BAR_DIM=0x6c7086;
float ui_scale=1.0f;
XftFont *barfont=&font;
BarColors barcol;
Client *clients;
struct MonitorGeom mons[MAXMONS]={{0,0,1000,800}};
Client *find(Window w) { for(Client *c=clients;c;c=c->next) if(c->win==w) return c; return NULL; }
void view(int ws) { assert(!overview_is_open()); curws=ws; views++; }
void focus(Client *c) { assert(c && c->ws==curws); focuses++; }
int mon_by_pointer(void) { return 0; }
static const char *workspace_name="workspace";
const char *ws_name(int ws) { (void)ws; return workspace_name; }
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
#define XFillRectangle(d,p,g,x,y,w,h) fill_rect(w,h)
#define XFillArc(d,p,g,x,y,w,h,a,b) fill_rect(w,h)
#define XDrawRectangle(...) 0
#define XCopyArea(d,s,t,g,sx,sy,w,h,x,y) copy_area(s,t,w,h,x,y)
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
static Window role_window;
void animation_overview_role(Window w) { role_window=w; }

#include "../src/ewmh.h"
int nmons=1, gaps_on, gap_inner, BORDER;
int COMPOSITOR, ANIMATIONS, REDUCE_MOTION, ANIMATION_OPEN_MS, ANIMATION_CLOSE_MS,
    ANIMATION_LAYOUT_MS, ANIMATION_OVERVIEW_MS;
Window checkwin;
int XChangeProperty(Display *d, Window w, Atom p, Atom t, int f, int m,
                    const unsigned char *data, int n) {
    (void)d; (void)w; (void)p; (void)t; (void)f; (void)m; (void)data; (void)n;
    return 0;
}
int XFlush(Display *d) { (void)d; return 0; }
static Layout layouts[MAXWS]; static int masters[MAXWS]; static float mfacts[MAXWS];
Layout *ws_layout=layouts; int *ws_nmaster=masters; float *ws_mfact=mfacts;
unsigned long BORDER_FOCUS, BORDER_NORMAL;
Client *sel;
Dock *docks;
#define XQueryTree(...) 0
#define XRestackWindows(...) 0
#define XMoveResizeWindow(...) 0
#define XMapWindow(...) 0
void animation_unmap(Window window, unsigned reason) { (void)window; (void)reason; }
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
/* Preview copies and click targets must stay inside their workspace/card. */
static void check_geometry(void) {
    int workspaces=0;
    for (int i=0;i<nhits;i++) {
        assert(hits[i].x>=0 && hits[i].y>=0 && hits[i].w>0 && hits[i].h>0);
        assert(hits[i].x+hits[i].w<=width && hits[i].y+hits[i].h<=height);
        if (!hits[i].id) { workspaces++; continue; }
        int parent=0;
        for (int j=0;j<i;j++) if (!hits[j].id && hits[j].ws==hits[i].ws) {
            assert(hits[i].x>hits[j].x && hits[i].y>hits[j].y);
            assert(hits[i].x+hits[i].w<hits[j].x+hits[j].w);
            assert(hits[i].y+hits[i].h<hits[j].y+hits[j].h);
            parent=1;
        }
        assert(parent);
    }
    if (width>=cols && height>=(NWS+cols-1)/cols) assert(workspaces==NWS);
    for (int i=0;i<ncopy;i++) if (copy_rects[i].dst==backing) {
        int inside=0;
        for (int j=0;j<nhits;j++) if (hits[j].id &&
            copy_rects[i].x>=hits[j].x && copy_rects[i].y>=hits[j].y &&
            copy_rects[i].x+copy_rects[i].w<=hits[j].x+hits[j].w &&
            copy_rects[i].y+copy_rects[i].h<=hits[j].y+hits[j].h-barfont->height)
            inside=1;
        assert(inside);
    }
}
int main(void) {
    font.height=16; font.ascent=12; font.descent=4;
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
    assert(role_window && role_window==overlay); /* marked before it maps */
    int g=grabs; unsigned long s=serial;
    overview_show(0); assert(grabs==g && serial==s);
    overview_select(2); assert(overview_selection()==2 && curws==0 && views==0 && focuses==0);
    overview_select(-1); overview_select(NWS); assert(overview_selection()==2);
    int arms=hover_arms;
    commit(None); assert(curws==2 && views==1 && !overview_is_open());
    assert(role_window==None);
    assert(hover_arms==arms+1);
    assert(overview_selection()==-1);
    int u=ungrabs; clean(); clean(); assert(ungrabs==u);
    grab_result=AlreadyGrabbed; overview_show(0); assert(!overview_is_open() && role_window==None); clean();
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
    clients=nine; copies=ncopy=0; overview_show(0); assert(copies==9);
    check_geometry(); clean(); clients=NULL;
    /* Default config has NULL workspace names: always render numbered labels. */
    workspace_name=NULL; text_log[0]=0;
    overview_show(0);
    assert(strstr(text_log,"Workspace 1\n") && strstr(text_log,"Workspace 9\n"));
    assert(strstr(text_log,"Empty workspace") && strstr(text_log,"Esc: cancel"));
    clean(); workspace_name="Development"; text_log[0]=0;
    overview_show(0); assert(strstr(text_log,"Development\n")); clean();
    workspace_name="workspace";
    /* Dense workspaces show an overflow count, never overlapping cards. */
    Client many[30]={0};
    NWS=5; mons[0].w=1920; mons[0].h=1080;
    for (int i=0;i<30;i++) { many[i].win=500+i; many[i].ws=0; many[i].next=i<29?&many[i+1]:NULL; }
    clients=many; compositor=0; text_log[0]=0; ncopy=0;
    overview_show(0); check_geometry();
    assert(strstr(text_log,"30 windows  /  +") && strstr(text_log,"Preview unavailable"));
    int cards=0; for (int i=0;i<nhits;i++) if (hits[i].id) cards++;
    assert(cards>=2 && cards<30);
    int firstcard=-1, secondcard=-1;
    for (int i=0;i<nhits;i++) if (hits[i].id) {
        if (firstcard<0) firstcard=i;
        else { secondcard=i; break; }
    }
    assert(secondcard>=0 && hits[firstcard].y==hits[secondcard].y);
    assert(hits[firstcard].x+hits[firstcard].w<hits[secondcard].x);
    /* Gutters are not workspace targets; clicking them does not commit. */
    XEvent gutter={0}; gutter.type=ButtonPress; gutter.xbutton.window=overlay;
    assert(overview_event(&gutter) && overview_is_open());
    click_card(500); assert(!overview_is_open() && curws==0);
    clean(); clients=NULL; compositor=1;
    /* All workspace counts, compact outputs and configured scale factors. */
    const int sizes[][2]={{320,240},{800,600},{1280,720},{1920,1080},{3440,1440}};
    for (int sc=0;sc<3;sc++) {
        ui_scale=sc==0?0.75f:sc==1?1.0f:2.0f;
        font.height=S(16); font.ascent=S(12); font.descent=font.height-font.ascent;
        for (unsigned z=0;z<sizeof sizes/sizeof sizes[0];z++) {
            mons[0].w=sizes[z][0]; mons[0].h=sizes[z][1];
            for (NWS=1;NWS<=MAXWS;NWS++) {
                clients=nine;
                for (int i=0;i<9;i++) nine[i].ws=i%NWS;
                ncopy=0; overview_show(0); check_geometry(); clean();
                clients=NULL;
            }
        }
    }
    ui_scale=1.0f; font.height=16; font.ascent=12; font.descent=4; NWS=9;
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
    clean();
    /* 4B highlight: off means no move and no timer. */
    mons[0].w=1280; mons[0].h=720; NWS=5; curws=0;
    ANIMATIONS=0; ANIMATION_OVERVIEW_MS=200;
    overview_show(0); overview_select(1);
    assert(from_sel<0 && overview_next_ms()==-1 && highlight(1,mono_ms())==100);
    clean();
    ANIMATIONS=1; REDUCE_MOTION=1;
    overview_show(0); overview_select(1); assert(from_sel<0 && overview_next_ms()==-1); clean();
    REDUCE_MOTION=0; ANIMATION_OVERVIEW_MS=0;
    overview_show(0); overview_select(1); assert(from_sel<0 && overview_next_ms()==-1); clean();
    /* On: the old card fades out while the new one fades in. */
    ANIMATION_OVERVIEW_MS=200;
    overview_show(0); overview_select(1);
    assert(from_sel==0 && pending==1);
    long next=overview_next_ms(); assert(next>=0 && next<=FRAME_MS);
    long long t0=sel_start;
    assert(highlight(1,t0)==0 && highlight(0,t0)==100 && highlight(2,t0)==0);
    assert(highlight(1,t0+100)==50 && highlight(0,t0+100)==50);
    assert(highlight(1,t0+199)>=99 && highlight(0,t0+199)<=1);
    assert(highlight(1,t0+200)==100 && from_sel<0 && overview_next_ms()==-1);
    /* The selection stays pending; commit takes the new target mid-move. */
    int v0=views;
    overview_select(2); assert(from_sel==1 && curws==0 && views==v0);
    commit(None); assert(curws==2 && from_sel<0 && overview_next_ms()==-1);
    /* A second change mid-move starts from the current weights: no jump. */
    curws=0; overview_show(0); overview_select(1);
    sel_start-=100;
    overview_select(2);
    long long t1=sel_start;
    int w0=highlight(0,t1), w1=highlight(1,t1), w2=highlight(2,t1);
    assert(w0>=45 && w0<=55 && w1>=45 && w1<=55 && w2==0);
    assert(highlight(2,t1+100)==50 && highlight(0,t1+100)<=w0/2+1 && highlight(1,t1+100)<=w1/2+1);
    assert(highlight(0,t1+200)==0 && highlight(1,t1+200)==0 && highlight(2,t1+200)==100);
    clean();
    /* Hide in the middle stops the move. */
    overview_show(0); overview_select(3); assert(from_sel>=0);
    overview_hide(0); assert(from_sel<0 && overview_next_ms()==-1);
    clean(); free(fake_display);
    puts("overview offline tests: OK");
    return 0;
}
