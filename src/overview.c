/* Root-child overview. Never owns application redirection or input focus. */
#include "overview.h"
#include "state.h"
#include "client.h"
#include "monitor.h"
#include "bar.h"
#include "xerr.h"
#include <X11/keysym.h>
#include <X11/extensions/Xcomposite.h>
#include <X11/extensions/Xrender.h>
#include <stdio.h>
#include <string.h>

#define CACHE 64
#define TW 192
#define TH 108
#define HITS 512
static struct { Window id; Pixmap pm; int w,h; } cache[CACHE];
static unsigned victim;
static Window overlay;
static Pixmap backing;
static GC gc;
static XftDraw *xd;
static int active, pending, width, height, cols;
static struct { int x,y,w,h,ws; Window id; } hits[HITS];
static int nhits;
static KeyCode commit_key;

int overview_is_open(void) { return active; }
int overview_selection(void) { return active ? pending : -1; }
void overview_drop(Window id) {
    for (int i=0;i<CACHE;i++) if (cache[i].id==id) {
        if (cache[i].pm) XFreePixmap(dpy,cache[i].pm);
        memset(&cache[i],0,sizeof cache[i]);
    }
}
void overview_capture(Window id) {
    Client *c=find(id);
    if (!c || c->ws<0 || c->ws>=NWS) return;
    char name[64];
    snprintf(name,sizeof name,"_NET_WM_CM_S%d",screen);
    if (!XGetSelectionOwner(dpy,XInternAtom(dpy,name,False))) return;
    XWindowAttributes a;
    Pixmap src=None, dst=None;
    Picture sp=None,dp=None;
    int dw=0,dh=0;
    trap_errors(dpy);
    if (XGetWindowAttributes(dpy,id,&a) && a.map_state==IsViewable && a.width>0 && a.height>0) {
        XRenderPictFormat *sf=XRenderFindVisualFormat(dpy,a.visual);
        XRenderPictFormat *df=XRenderFindVisualFormat(dpy,DefaultVisual(dpy,screen));
        src=XCompositeNameWindowPixmap(dpy,id);
        Window r; int x,y; unsigned w,h,b,depth;
        if (sf && df && XGetGeometry(dpy,src,&r,&x,&y,&w,&h,&b,&depth) && w && h) {
            double scale=(double)TW/w;
            if ((double)TH/h<scale) scale=(double)TH/h;
            if (scale>1) scale=1;
            dw=(int)(w*scale); dh=(int)(h*scale);
            if (dw<1) dw=1;
            if (dh<1) dh=1;
            dst=XCreatePixmap(dpy,root,dw,dh,DefaultDepth(dpy,screen));
            sp=XRenderCreatePicture(dpy,src,sf,0,NULL);
            dp=XRenderCreatePicture(dpy,dst,df,0,NULL);
            XRenderColor bg={0x1818,0x1818,0x1818,0xffff};
            XRenderFillRectangle(dpy,PictOpSrc,dp,&bg,0,0,dw,dh);
            XTransform t={{{XDoubleToFixed(1/scale),0,0},{0,XDoubleToFixed(1/scale),0},{0,0,XDoubleToFixed(1)}}};
            XRenderSetPictureTransform(dpy,sp,&t);
            XRenderSetPictureFilter(dpy,sp,FilterBilinear,NULL,0);
            XRenderComposite(dpy,PictOpOver,sp,None,dp,0,0,0,0,0,0,dw,dh);
        }
    }
    if (sp) XRenderFreePicture(dpy,sp);
    if (dp) XRenderFreePicture(dpy,dp);
    if (src) XFreePixmap(dpy,src);
    int ok=untrap_errors(dpy);
    if (!ok || !dst) {
        /* Creation returns an XID even when the asynchronous request failed. */
        trap_errors(dpy);
        if (dst) XFreePixmap(dpy,dst);
        untrap_errors(dpy);
        return;
    }
    int slot=-1;
    for (int i=0;i<CACHE;i++) if (cache[i].id==id) { slot=i; break; }
    if (slot<0) slot=(int)(victim++%CACHE);
    if (cache[slot].pm) XFreePixmap(dpy,cache[slot].pm);
    cache[slot].id=id; cache[slot].pm=dst;
    cache[slot].w=dw; cache[slot].h=dh;
}
static void text(int x,int y,int w,const char *s) {
    if (!xd || !barfont || w<=0) return;
    XRectangle clip={(short)x,(short)(y-barfont->ascent),(unsigned short)w,(unsigned short)barfont->height};
    XftDrawSetClipRectangles(xd,0,0,&clip,1);
    size_t n=strnlen(s,512), used=0;
    FcChar8 safe[512];
    /* Validate untrusted properties, including overlongs and surrogate codepoints. */
    for (size_t i=0;i<n;) {
        unsigned char c=(unsigned char)s[i];
        size_t len=c<0x80?1:c>=0xc2 && c<=0xdf?2:c>=0xe0 && c<=0xef?3:c>=0xf0 && c<=0xf4?4:0;
        int valid=len && len<=n-i;
        for (size_t j=1;valid && j<len;j++)
            if (((unsigned char)s[i+j]&0xc0)!=0x80) valid=0;
        if (valid && len>=3) {
            unsigned char b=(unsigned char)s[i+1];
            if ((c==0xe0 && b<0xa0) || (c==0xed && b>=0xa0) ||
                (c==0xf0 && b<0x90) || (c==0xf4 && b>=0x90)) valid=0;
        }
        if (valid) { memcpy(safe+used,s+i,len); used+=len; i+=len; }
        else { safe[used++]='?'; i++; }
    }
    XftDrawStringUtf8(xd,&barcol.title,barfont,x,y,safe,(int)used);
    XftDrawSetClip(xd,NULL);
}
static void hit(int x,int y,int w,int h,int ws,Window id) {
    if (w>0 && h>0 && nhits<HITS) { hits[nhits].x=x; hits[nhits].y=y; hits[nhits].w=w; hits[nhits].h=h; hits[nhits].ws=ws; hits[nhits++].id=id; }
}
static void draw(void) {
    if (!active) return;
    nhits=0;
    XSetForeground(dpy,gc,BAR_BG); XFillRectangle(dpy,backing,gc,0,0,width,height);
    int rows=(NWS+cols-1)/cols, cw=width/cols, ch=height/rows;
    for (int ws=0;ws<NWS && ws<MAXWS;ws++) {
        int x=(ws%cols)*cw,y=(ws/cols)*ch;
        XSetForeground(dpy,gc,ws==pending?BAR_ACC:BAR_DIM);
        if (cw>2 && ch>2) XDrawRectangle(dpy,backing,gc,x+1,y+1,cw-3,ch-3);
        hit(x,y,cw,ch,ws,None);
        text(x+8,y+24,cw-16,ws_name(ws));
        if (cw<=16) continue;
        int cy=y+36;
        for (Client *c=clients;c;c=c->next) if (c->ws==ws && cy+24<y+ch) {
            int room=y+ch-4-cy-28;
            int imageh=room>TH?TH:room;
            if (imageh<0) imageh=0;
            int cardh=imageh+28;
            if (cy+cardh>y+ch-4) break;
            hit(x+6,cy,cw-12,cardh,ws,c->win);
            if (cardh>28) for (int i=0;i<CACHE;i++) if (cache[i].id==c->win && cache[i].pm) {
                int w=cache[i].w; if (w>cw-16) w=cw-16;
                int h=cache[i].h; if (h>imageh) h=imageh;
                if (w>0 && h>0) XCopyArea(dpy,cache[i].pm,backing,gc,0,0,w,h,x+8,cy);
            }
            unsigned char *title=NULL; Atom type; int format; unsigned long n,after;
            trap_errors(dpy);
            if (XGetWindowProperty(dpy,c->win,A_NET_WM_NAME,0,128,False,AnyPropertyType,&type,&format,&n,&after,&title)==Success && title && format==8)
                text(x+8,cy+cardh-6,cw-16,(char *)title);
            else text(x+8,cy+cardh-6,cw-16,"Window");
            if (title) XFree(title);
            untrap_errors(dpy);
            cy+=cardh+4;
        }
    }
    XCopyArea(dpy,backing,overlay,gc,0,0,width,height,0,0);
}
void overview_top(void) { if (active) XRaiseWindow(dpy,overlay); }
void overview_hide(int unused) {
    (void)unused;
    if (active) {
        hover_lock_arm(); /* Destroying the cover must not change stationary hover focus. */
        XUngrabKeyboard(dpy,CurrentTime);
    }
    active=0; commit_key=0;
    /* Failed asynchronous creation can leave nonzero but invalid handles. */
    trap_errors(dpy);
    if (xd) XftDrawDestroy(xd);
    if (gc) XFreeGC(dpy,gc);
    if (backing) XFreePixmap(dpy,backing);
    if (overlay) XDestroyWindow(dpy,overlay);
    untrap_errors(dpy);
    xd=NULL; gc=NULL; backing=None; overlay=None;
}
void overview_show(int unused) {
    (void)unused;
    if (active || drag.win != None || NWS<1 || NWS>MAXWS || !barfont || !barcol_ok) return;
    for (Client *c=clients;c;c=c->next) overview_capture(c->win);
    int m=mon_by_pointer();
    width=mons[m].w; height=mons[m].h;
    if (width<1 || height<1) return;
    cols=1; while (cols*cols<NWS) cols++;
    XSetWindowAttributes a={0}; a.override_redirect=True; a.background_pixel=BAR_BG;
    a.event_mask=ExposureMask|ButtonPressMask|PointerMotionMask|KeyPressMask|KeyReleaseMask;
    trap_errors(dpy);
    overlay=XCreateWindow(dpy,root,mons[m].x,mons[m].y,width,height,0,CopyFromParent,InputOutput,CopyFromParent,CWOverrideRedirect|CWBackPixel|CWEventMask,&a);
    backing=XCreatePixmap(dpy,root,width,height,DefaultDepth(dpy,screen));
    gc=XCreateGC(dpy,backing,0,NULL);
    xd=XftDrawCreate(dpy,backing,DefaultVisual(dpy,screen),DefaultColormap(dpy,screen));
    XMapRaised(dpy,overlay);
    int ok=untrap_errors(dpy);
    if (!ok || !xd || XGrabKeyboard(dpy,overlay,False,GrabModeAsync,GrabModeAsync,CurrentTime)!=GrabSuccess) { overview_hide(0); return; }
    pending=curws; active=1; draw();
}
void overview_toggle(int unused) { if (active) overview_hide(unused); else overview_show(unused); }
void overview_select(int ws) { if (active && ws>=0 && ws<NWS) { pending=ws; draw(); } }
static void commit(Window id) {
    int ws=pending;
    overview_hide(0);
    view(ws);
    Client *c=id?find(id):NULL;
    if (c && c->ws==ws) focus(c);
}
int overview_event(XEvent *e) {
    if (!active) return 0;
    switch(e->type) {
    case Expose: if (e->xexpose.window==overlay) draw(); return e->xexpose.window==overlay;
    case KeyPress: {
        KeySym k=XLookupKeysym(&e->xkey,0);
        if (k==XK_Escape || ((e->xkey.state&MOD) && k==XK_o)) overview_hide(0);
        else if (commit_key) return 1; /* Keep the grab through held-Enter repeats. */
        else if (k==XK_Return || k==XK_KP_Enter) commit_key=e->xkey.keycode;
        else if (k==XK_Left) overview_select(pending-1);
        else if (k==XK_Right) overview_select(pending+1);
        else if (k==XK_Up) overview_select(pending-cols);
        else if (k==XK_Down) overview_select(pending+cols);
        return 1;
    }
    case KeyRelease:
        if (commit_key && e->xkey.keycode==commit_key) {
            /* Core autorepeat emits a same-time Release/Press pair. Peek
             * without removing the press; the active grab consumes it next. */
            XEvent next;
            if (XEventsQueued(dpy,QueuedAfterReading)>0) {
                XPeekEvent(dpy,&next);
                if (next.type==KeyPress && next.xkey.keycode==commit_key &&
                    next.xkey.time==e->xkey.time) return 1;
            }
            commit(None);
        }
        return 1;
    case ButtonPress:
        if (e->xbutton.window!=overlay) { overview_hide(0); return 1; }
        for (int i=nhits-1;i>=0;i--) if (e->xbutton.x>=hits[i].x && e->xbutton.x<hits[i].x+hits[i].w && e->xbutton.y>=hits[i].y && e->xbutton.y<hits[i].y+hits[i].h) {
            pending=hits[i].ws; commit(hits[i].id); break;
        }
        return 1;
    case MotionNotify: case EnterNotify: case LeaveNotify: case FocusIn: case FocusOut: case ButtonRelease: return 1;
    default: return 0; /* GenericEvent cookie remains owned by main. */
    }
}
