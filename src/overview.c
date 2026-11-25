/* Root-child overview. Never owns application redirection or input focus. */
#include "overview.h"
#include "state.h"
#include "client.h"
#include "monitor.h"
#include "bar.h"
#include "xerr.h"
#include "keys.h"
#include <X11/keysym.h>
#include <X11/extensions/Xrender.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#define HITS 512
static Window overlay;
static Pixmap backing;
static GC gc;
static XftDraw *xd;
static int active, pending, width, height, cols;
static int committing; /* view() runs from a click or key in the overview */
static struct { int x,y,w,h,ws; Window id; } hits[HITS];
static int nhits;
static KeyCode commit_key;
static int expose, esel, ecols=1; /* exposé mode, selected card, grid columns */
/* Selection highlight move: each card blends from start_w to its target. */
static int from_sel=-1;
static int start_w[MAXWS];
static long long sel_start, last_frame;
#define FRAME_MS 16

static long long mono_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC,&t);
    return (long long)t.tv_sec*1000+t.tv_nsec/1000000;
}
static int highlight_on(void) { return ANIMATIONS && !REDUCE_MOTION && ANIMATION_OVERVIEW_MS>0; }
/* Selected weight 0..100 of one card; ends the move when its time is up. */
static int highlight(int ws, long long now) {
    if (from_sel>=0 && (!highlight_on() || now-sel_start>=ANIMATION_OVERVIEW_MS || now<sel_start))
        from_sel=-1;
    int target=ws==pending?100:0;
    if (from_sel<0 || ws<0 || ws>=MAXWS) return target;
    int t=(int)((now-sel_start)*100/ANIMATION_OVERVIEW_MS);
    return start_w[ws]+(target-start_w[ws])*t/100;
}
int overview_is_open(void) { return active; }
/* Milliseconds to the next highlight frame, or -1 when nothing moves. */
long overview_next_ms(void) {
    if (!active || from_sel<0) return -1;
    long long left=last_frame+FRAME_MS-mono_ms();
    return left<0?0:(long)left;
}
static void draw(void);
void overview_tick(void) { if (active && from_sel>=0) { last_frame=mono_ms(); draw(); } }
int overview_committing(void) { return committing; }
int overview_selection(void) { return active ? pending : -1; }
/* Validate untrusted properties, including overlongs and surrogate codepoints.
 * Copies at most 512 bytes of s into safe; returns the byte count. */
static size_t utf8_safe(const char *s,FcChar8 *safe) {
    size_t n=strnlen(s,512), used=0;
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
    return used;
}
static int text_w(XftFont *f,const FcChar8 *s,size_t n) {
    XGlyphInfo g;
    XftTextExtentsUtf8(dpy,f,s,(int)n,&g);
    return g.xOff;
}
/* Draw s in [x, x+w), centered or left aligned; a too-wide string ends
 * with an ellipsis. */
static void text_fit(XftFont *f,int x,int y,int w,const char *s,const XftColor *color,int center) {
    if (!xd || !f || !s || w<=0) return;
    static const FcChar8 ell[]="\xe2\x80\xa6";
    FcChar8 safe[512+sizeof ell];
    size_t n=utf8_safe(s,safe), m=0;
    /* No per-glyph fallback here: drop characters the font cannot draw
     * (e.g. the Claude Code status mark) and the space they leave twice. */
    int dropped=0;
    for (size_t i=0;i<n;) {
        unsigned char c=safe[i];
        size_t len=c<0x80?1:c<0xe0?2:c<0xf0?3:4;
        FcChar32 cp=len==1?c:len==2?c&0x1fu:len==3?c&0x0fu:c&0x07u;
        for (size_t j=1;j<len;j++) cp=(cp<<6)|(safe[i+j]&0x3fu);
        if (!XftCharExists(dpy,f,cp)) dropped=1;
        else if (!(dropped && c==' ' && m>0 && safe[m-1]==' ')) {
            memmove(safe+m,safe+i,len); m+=len; dropped=0;
        }
        i+=len;
    }
    n=m;
    if (!n) return;
    int tw=text_w(f,safe,n);
    if (tw>w) {
        int ew=text_w(f,ell,3);
        while (n>0 && tw+ew>w) {
            do n--; while (n>0 && (safe[n]&0xc0)==0x80);
            tw=text_w(f,safe,n);
        }
        memcpy(safe+n,ell,3); n+=3; tw+=ew;
    }
    XRectangle clip={(short)x,(short)(y-f->ascent),(unsigned short)w,(unsigned short)f->height};
    XftDrawSetClipRectangles(xd,0,0,&clip,1);
    XftDrawStringUtf8(xd,color,f,x+(center && tw<w?(w-tw)/2:0),y,safe,(int)n);
    XftDrawSetClip(xd,NULL);
}
static void text_center(XftFont *f,int x,int y,int w,const char *s,const XftColor *color) {
    text_fit(f,x,y,w,s,color,1);
}
/* Derive surfaces from the configured theme without allocating server colors. */
static unsigned long blend(unsigned long a,unsigned long b,unsigned weight) {
    unsigned long rgb=0;
    for (int shift=0;shift<=16;shift+=8)
        rgb|=((((a>>shift)&255)*(100-weight)+((b>>shift)&255)*weight)/100)<<shift;
    return rgb;
}
static void rect(int x,int y,int w,int h,unsigned long color) {
    if (w<=0 || h<=0) return;
    XSetForeground(dpy,gc,color);
    XFillRectangle(dpy,backing,gc,x,y,(unsigned)w,(unsigned)h);
}
static Picture bpic; /* backing as a Render picture, for antialiased corners */
/* cos(i*90/16 degrees); the sine is the same table read backwards. */
static const double quarter[17]={1,0.99518473,0.98078528,0.95694034,0.92387953,0.88192126,
    0.83146961,0.77301045,0.70710678,0.63439328,0.55557023,0.47139674,0.38268343,
    0.29028468,0.19509032,0.09801714,0};
/* Antialiased circle masks, one per radius. Triangles are slow on the
 * server (3-8 ms per call), so each radius is drawn only once per open. */
#define NMASKS 8
static struct { int r; Pixmap pix; Picture pic; } masks[NMASKS];
static Picture circle_mask(int r) {
    int k=0;
    while (k<NMASKS && masks[k].r && masks[k].r!=r) k++;
    if (k==NMASKS) return None;
    if (masks[k].r) return masks[k].pic;
    XRenderPictFormat *a8=XRenderFindStandardFormat(dpy,PictStandardA8);
    if (!a8) return None;
    Pixmap pix=XCreatePixmap(dpy,root,(unsigned)(2*r),(unsigned)(2*r),8);
    Picture pic=XRenderCreatePicture(dpy,pix,a8,0,NULL);
    XRenderColor clear={0,0,0,0}, white={0xffff,0xffff,0xffff,0xffff};
    XRenderFillRectangle(dpy,PictOpSrc,pic,&clear,0,0,(unsigned)(2*r),(unsigned)(2*r));
    /* Triangle fan around the center; the A8 mask format smooths the arc. */
    XPointFixed pt[2+4*17];
    int n=0;
    pt[n].x=pt[n].y=XDoubleToFixed(r); n++;
    for (int q=0;q<4;q++) for (int i=0;i<=16;i++) {
        double c=quarter[i], sn=quarter[16-i];
        double dx=q==0?sn:q==1?c:q==2?-sn:-c, dy=q==0?-c:q==1?sn:q==2?c:-sn;
        pt[n].x=XDoubleToFixed(r+dx*r); pt[n++].y=XDoubleToFixed(r+dy*r);
    }
    pt[n++]=pt[1];
    Picture fill=XRenderCreateSolidFill(dpy,&white);
    XRenderCompositeTriFan(dpy,PictOpOver,fill,pic,a8,0,0,pt,n);
    XRenderFreePicture(dpy,fill);
    masks[k].r=r; masks[k].pix=pix; masks[k].pic=pic;
    return pic;
}
static void rounded(int x,int y,int w,int h,int r,unsigned long color) {
    if (w<=0 || h<=0) return;
    if (r>w/2) r=w/2;
    if (r>h/2) r=h/2;
    if (r<1) { rect(x,y,w,h,color); return; }
    rect(x+r,y,w-2*r,h,color);
    rect(x,y+r,w,h-2*r,color);
    Picture m=bpic?circle_mask(r):None;
    if (m) {
        /* Each corner is one quadrant of the circle mask. */
        XRenderColor rc={(unsigned short)((color>>16&255)*257),(unsigned short)((color>>8&255)*257),
                         (unsigned short)((color&255)*257),0xffff};
        Picture fill=XRenderCreateSolidFill(dpy,&rc);
        XRenderComposite(dpy,PictOpOver,fill,m,bpic,0,0,0,0,x,y,(unsigned)r,(unsigned)r);
        XRenderComposite(dpy,PictOpOver,fill,m,bpic,0,0,r,0,x+w-r,y,(unsigned)r,(unsigned)r);
        XRenderComposite(dpy,PictOpOver,fill,m,bpic,0,0,0,r,x,y+h-r,(unsigned)r,(unsigned)r);
        XRenderComposite(dpy,PictOpOver,fill,m,bpic,0,0,r,r,x+w-r,y+h-r,(unsigned)r,(unsigned)r);
        XRenderFreePicture(dpy,fill);
        return;
    }
    XSetForeground(dpy,gc,color);
    int d=2*r;
    XFillArc(dpy,backing,gc,x,y,d,d,90*64,90*64);
    XFillArc(dpy,backing,gc,x+w-d,y,d,d,0,90*64);
    XFillArc(dpy,backing,gc,x,y+h-d,d,d,180*64,90*64);
    XFillArc(dpy,backing,gc,x+w-d,y+h-d,d,d,270*64,90*64);
}
static void hit(int x,int y,int w,int h,int ws,Window id) {
    if (w>0 && h>0 && nhits<HITS) { hits[nhits].x=x; hits[nhits].y=y; hits[nhits].w=w; hits[nhits].h=h; hits[nhits].ws=ws; hits[nhits++].id=id; }
}
/* Semibold face of the bar font for exposé tiles; NULL = barfont. */
static XftFont *bigfont;
static XftFont *big(void) {
    if (!bigfont) {
        /* pixelsize overrides any size= in the configured pattern. */
        char pat[320];
        snprintf(pat,sizeof pat,"%.256s:pixelsize=%d:weight=demibold",
                 font_name?font_name:"sans",barfont->height*3/2);
        bigfont=XftFontOpenName(dpy,screen,pat);
    }
    return bigfont?bigfont:barfont;
}
/* Dimmed copy of the screen under the exposé panel; None = solid backdrop. */
static Pixmap snap;
static void snapshot(int mx,int my) {
    XRenderPictFormat *fmt=XRenderFindVisualFormat(dpy,DefaultVisual(dpy,screen));
    if (!fmt) return;
    snap=XCreatePixmap(dpy,root,width,height,DefaultDepth(dpy,screen));
    XSetSubwindowMode(dpy,gc,IncludeInferiors);
    XCopyArea(dpy,root,snap,gc,mx,my,width,height,0,0);
    XSetSubwindowMode(dpy,gc,ClipByChildren);
    Picture p=XRenderCreatePicture(dpy,snap,fmt,0,NULL);
    XRenderColor dim={0,0,0,0xb400};
    XRenderFillRectangle(dpy,PictOpOver,p,&dim,0,0,width,height);
    XRenderFreePicture(dpy,p);
}
/* Tile label: up to 4 capitals of the class (WCVF, ST), else 2 letters. */
static void abbrev(const char *cls,char out[5]) {
    int n=0;
    for (const char *p=cls;*p && n<4;p++) if (*p>='A' && *p<='Z') out[n++]=*p;
    if (n<2) {
        n=0;
        for (const char *p=cls;*p && n<2;p++)
            if ((*p>='a' && *p<='z') || (*p>='A' && *p<='Z') || (*p>='0' && *p<='9'))
                out[n++]=(char)(*p>='a' && *p<='z'?*p-32:*p);
    }
    out[n]=0;
}
/* Stable tile color per class from the macOS system palette. */
static unsigned long tile_color(const char *cls) {
    static const unsigned long pal[]={0x0A84FF,0x5E5CE6,0xBF5AF2,0xFF375F,0xFF453A,
                                      0xFF9F0A,0x30D158,0x40C8E0,0xAC8E68};
    unsigned h=5381;
    for (const char *p=cls;*p;p++) h=h*33u+(unsigned char)*p;
    return blend(pal[h%(sizeof pal/sizeof pal[0])],0,18);
}
/* Exposé: a centered Cmd+Tab style panel, one tile per window. The
 * workspace name sits on top and the selected window's title below. */
static void draw_expose(void) {
    XftFont *f=big();
    int tile=S(88), cellw=tile+S(36), pad=S(18), r=S(22), fh=barfont->height;
    int cellh=S(12)+tile+S(10)+fh+S(12), headh=fh+S(14), footh=fh+S(22), gapline=S(16);
    unsigned long panel=blend(BAR_BG,BAR_FG,9), rule=blend(panel,BAR_FG,10);
    int n=0;
    for (Client *c=clients;c;c=c->next) if (c->ws==curws) n++;
    if (snap) XCopyArea(dpy,snap,backing,gc,0,0,width,height,0,0);
    else rect(0,0,width,height,blend(BAR_BG,0,30));
    int maxcols=(width-2*S(48)-2*pad)/cellw;
    if (maxcols<1) maxcols=1;
    ecols=n<maxcols?(n>0?n:1):maxcols;
    int rows=n?(n+ecols-1)/ecols:1;
    int pw=ecols*cellw+2*pad, ph=pad+headh+rows*cellh+gapline+footh+pad/2;
    if (pw<S(420)) pw=S(420);
    if (pw>width) pw=width;
    int px=(width-pw)/2, py=(height-ph)/2;
    rounded(px-1,py-1,pw+2,ph+2,r+1,rule);
    rounded(px,py,pw,ph,r,panel);
    char head[160];
    const char *name=ws_name(curws);
    snprintf(head,sizeof head,"%s  \xc2\xb7  %d window%s",name && *name?name:"Workspace",n,n==1?"":"s");
    text_center(barfont,px+pad,py+pad+barfont->ascent,pw-2*pad,head,&barcol.task_text);
    int fy=py+ph-pad/2-footh;
    rect(px+pad,fy,pw-2*pad,1,rule);
    int ty=fy+(footh-fh)/2+barfont->ascent;
    if (!n) text_center(barfont,px+pad,ty,pw-2*pad,"No windows",&barcol.task_text);
    int i=0;
    trap_errors(dpy);
    for (Client *c=clients;c;c=c->next) if (c->ws==curws) {
        int row=i/ecols, inrow=n-row*ecols;
        if (inrow>ecols) inrow=ecols;
        int x=px+(pw-inrow*cellw)/2+(i%ecols)*cellw, y=py+pad+headh+row*cellh;
        int selected=i==esel;
        if (selected) rounded(x+S(4),y,cellw-S(8),cellh,S(16),blend(panel,BAR_FG,14));
        hit(x,y,cellw,cellh,curws,c->win);
        XClassHint hint={0};
        const char *cls=XGetClassHint(dpy,c->win,&hint) && hint.res_class?hint.res_class:"?";
        char ab[5];
        abbrev(cls,ab);
        int tx=x+(cellw-tile)/2, tyy=y+S(12);
        rounded(tx,tyy,tile,tile,S(20),tile_color(cls));
        text_center(f,tx+S(4),tyy+(tile-f->height)/2+f->ascent,tile-S(8),ab,&barcol.ws_acttx);
        text_center(barfont,x+S(6),tyy+tile+S(10)+barfont->ascent,cellw-S(12),cls,
                    selected?&barcol.title:&barcol.task_text);
        if (selected) {
            unsigned char *title=NULL; Atom type; int format; unsigned long nn,after;
            if (XGetWindowProperty(dpy,c->win,A_NET_WM_NAME,0,128,False,AnyPropertyType,&type,&format,&nn,&after,&title)==Success && title && format==8)
                text_center(barfont,px+pad,ty,pw-2*pad,(char *)title,&barcol.title);
            else text_center(barfont,px+pad,ty,pw-2*pad,cls,&barcol.title);
            if (title) XFree(title);
        }
        if (hint.res_class) XFree(hint.res_class);
        if (hint.res_name) XFree(hint.res_name);
        i++;
    }
    untrap_errors(dpy);
    XCopyArea(dpy,backing,overlay,gc,0,0,width,height,0,0);
}
/* Window of the selected exposé card, or None. */
static Window expose_pick(void) {
    int i=0;
    for (Client *c=clients;c;c=c->next) if (c->ws==curws && i++==esel) return c->win;
    return None;
}
static void expose_move(int d) {
    int n=0;
    for (Client *c=clients;c;c=c->next) if (c->ws==curws) n++;
    if (esel+d>=0 && esel+d<n) { esel+=d; draw(); }
}
static void draw(void) {
    if (!active) return;
    nhits=0;
    if (expose) { draw_expose(); return; }
    int fh=barfont->height, pad=S(16), gap=S(24), r=S(18), margin=S(48);
    int headh=fh+S(18), rowh=fh+S(14), tagh=fh+S(4);
    int tagw=text_w(barfont,(const FcChar8 *)"WWWW",4)+S(14);
    int rows=(NWS+cols-1)/cols;
    unsigned long panel=blend(BAR_BG,BAR_FG,9), rule=blend(panel,BAR_FG,10);
    long long now=mono_ms();
    if (snap) XCopyArea(dpy,snap,backing,gc,0,0,width,height,0,0);
    else rect(0,0,width,height,blend(BAR_BG,0,30));
    if (width<2*margin+cols*S(160) || height<2*margin+rows*(headh+rowh+2*pad))
        margin=gap=0;
    /* All cards share one height: enough lines for the fullest workspace. */
    int most=0;
    for (int ws=0;ws<NWS;ws++) {
        int n=0;
        for (Client *c=clients;c;c=c->next) if (c->ws==ws) n++;
        if (n>most) most=n;
    }
    int maxh=(height-2*margin-(rows-1)*gap)/rows;
    int fit=(maxh-headh-2*pad)/rowh, lines=most<3?3:most;
    if (lines>fit) lines=fit;
    if (lines<1) lines=1;
    int cw=(width-2*margin-(cols-1)*gap)/cols, ch=headh+lines*rowh+2*pad;
    if (cw>S(400)) cw=S(400);
    if (ch>maxh) ch=maxh;
    int y0=(height-(rows*ch+(rows-1)*gap))/2;
    for (int ws=0;ws<NWS && ws<MAXWS;ws++) {
        int row=ws/cols, inrow=NWS-row*cols;
        if (inrow>cols) inrow=cols;
        int x=(width-(inrow*cw+(inrow-1)*gap))/2+(ws%cols)*(cw+gap), y=y0+row*(ch+gap);
        int weight=highlight(ws,now);
        int selected=weight>=50; /* text colour and ring width switch half-way */
        int ring=selected?S(2):1;
        rounded(x-ring,y-ring,cw+2*ring,ch+2*ring,r+ring,blend(rule,BAR_ACC,(unsigned)weight));
        rounded(x,y,cw,ch,r,blend(panel,BAR_ACC,(unsigned)weight/8));
        hit(x,y,cw,ch,ws,None);
        if (cw<2*pad+tagw || ch<headh+2*pad) continue;
        /* Header: number badge, name, and a marker on the current workspace. */
        int bs=fh+S(6), by=y+pad, base=by+(bs-fh)/2+barfont->ascent;
        char number[8], label[64];
        snprintf(number,sizeof number,"%d",ws+1);
        rounded(x+pad,by,bs,bs,S(6),selected?BAR_ACC:blend(panel,BAR_FG,16));
        text_center(barfont,x+pad,base,bs,number,selected?&barcol.ws_acttx:&barcol.title);
        const char *name=ws_name(ws);
        if (name && *name) snprintf(label,sizeof label,"%s",name);
        else snprintf(label,sizeof label,"Workspace %d",ws+1);
        int curw=ws==curws?text_w(barfont,(const FcChar8 *)"Current",7)+S(8):0;
        text_fit(barfont,x+pad+bs+S(10),base,cw-2*pad-bs-S(10)-curw,label,&barcol.title,0);
        if (curw) text_fit(barfont,x+cw-pad-curw+S(8),base,curw-S(8),"Current",&barcol.task_text,0);
        /* One line per window: class tag and title. */
        int count=0;
        for (Client *c=clients;c;c=c->next) if (c->ws==ws) count++;
        int top=y+pad+headh, room=(y+ch-pad-top)/rowh;
        int cap=count>room?room-1:count, shown=0;
        if (!count && room>0)
            text_center(barfont,x+pad,top+(y+ch-pad-top-fh)/2+barfont->ascent,cw-2*pad,
                        "No windows",&barcol.task_text);
        trap_errors(dpy);
        for (Client *c=clients;c && shown<cap;c=c->next) if (c->ws==ws) {
            int ry=top+shown*rowh, tx=x+pad+tagw+S(10);
            XClassHint hint={0};
            const char *cls=XGetClassHint(dpy,c->win,&hint) && hint.res_class?hint.res_class:"?";
            char ab[5];
            abbrev(cls,ab);
            rounded(x+pad,ry+(rowh-tagh)/2,tagw,tagh,S(6),tile_color(cls));
            text_center(barfont,x+pad,ry+(rowh-fh)/2+barfont->ascent,tagw,ab,&barcol.ws_acttx);
            unsigned char *title=NULL; Atom type; int format; unsigned long nn,after;
            const char *t=cls;
            if (XGetWindowProperty(dpy,c->win,A_NET_WM_NAME,0,128,False,AnyPropertyType,&type,&format,&nn,&after,&title)==Success && title && format==8)
                t=(const char *)title;
            text_fit(barfont,tx,ry+(rowh-fh)/2+barfont->ascent,x+cw-pad-tx,t,
                     c==sel?&barcol.title:&barcol.task_text,0);
            if (title) XFree(title);
            if (hint.res_class) XFree(hint.res_class);
            if (hint.res_name) XFree(hint.res_name);
            hit(x+pad,ry,cw-2*pad,rowh,ws,c->win);
            shown++;
        }
        untrap_errors(dpy);
        if (count>shown && room>0) {
            snprintf(label,sizeof label,"+%d more",count-shown);
            text_fit(barfont,x+pad,top+shown*rowh+(rowh-fh)/2+barfont->ascent,cw-2*pad,
                     label,&barcol.task_text,0);
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
    active=0; commit_key=0; from_sel=-1; expose=0;
    if (bigfont) { XftFontClose(dpy,bigfont); bigfont=NULL; }
    /* Failed asynchronous creation can leave nonzero but invalid handles. */
    trap_errors(dpy);
    if (xd) XftDrawDestroy(xd);
    if (gc) XFreeGC(dpy,gc);
    if (bpic) XRenderFreePicture(dpy,bpic);
    for (int k=0;k<NMASKS && masks[k].r;k++) {
        XRenderFreePicture(dpy,masks[k].pic);
        XFreePixmap(dpy,masks[k].pix);
        masks[k].r=0;
    }
    if (backing) XFreePixmap(dpy,backing);
    if (snap) XFreePixmap(dpy,snap);
    if (overlay) { animation_overview_role(None); XDestroyWindow(dpy,overlay); }
    untrap_errors(dpy);
    xd=NULL; gc=NULL; backing=None; snap=None; bpic=None; overlay=None;
}
void overview_show(int unused) {
    (void)unused;
    if (active || drag.win != None || NWS<1 || NWS>MAXWS || !barfont || !barcol_ok) return;
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
    XRenderPictFormat *pf=XRenderFindVisualFormat(dpy,DefaultVisual(dpy,screen));
    bpic=pf?XRenderCreatePicture(dpy,backing,pf,0,NULL):None;
    if (gc) snapshot(mons[m].x,mons[m].y); /* before the overlay covers it */
    if (overlay) animation_overview_role(overlay);
    XMapRaised(dpy,overlay);
    int ok=untrap_errors(dpy);
    if (!ok || !xd || XGrabKeyboard(dpy,overlay,False,GrabModeAsync,GrabModeAsync,CurrentTime)!=GrabSuccess) { overview_hide(0); return; }
    pending=curws; active=1; draw();
}
void overview_toggle(int unused) { if (active) overview_hide(unused); else overview_show(unused); }
void expose_toggle(int unused) {
    if (active) { overview_hide(unused); return; }
    expose=1; esel=0;
    int i=0;
    for (Client *c=clients;c;c=c->next) if (c->ws==curws) { if (c==sel) esel=i; i++; }
    overview_show(unused);
    if (!active) expose=0;
}
void overview_select(int ws) {
    if (!active || expose || ws<0 || ws>=NWS) return;
    if (ws!=pending && highlight_on()) {
        long long now=mono_ms();
        for (int i=0;i<MAXWS;i++) start_w[i]=highlight(i,now);
        from_sel=pending; sel_start=last_frame=now;
    }
    pending=ws; draw();
}
static void commit(Window id) {
    int ws=pending;
    overview_hide(0);
    committing=1; view(ws); committing=0;
    Client *c=id?find(id):NULL;
    if (c && c->ws==ws) focus(c);
}
int overview_event(XEvent *e) {
    if (!active) return 0;
    switch(e->type) {
    case Expose: if (e->xexpose.window==overlay) draw(); return e->xexpose.window==overlay;
    case KeyPress: {
        KeySym k=XLookupKeysym(&e->xkey,0);
        if (k==XK_Escape || ((e->xkey.state&MOD) && (k==XK_o || (expose && k==XK_w)))) overview_hide(0);
        else if (commit_key) return 1; /* Keep the grab through held-Enter repeats. */
        else if (k==XK_Return || k==XK_KP_Enter) commit_key=e->xkey.keycode;
        else if (expose) {
            if (k==XK_Left) expose_move(-1);
            else if (k==XK_Right) expose_move(1);
            else if (k==XK_Up) expose_move(-ecols);
            else if (k==XK_Down) expose_move(ecols);
        }
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
            commit(expose?expose_pick():None);
        }
        return 1;
    case ButtonPress:
        if (e->xbutton.window!=overlay) { overview_hide(0); return 1; }
        for (int i=nhits-1;i>=0;i--) if (e->xbutton.x>=hits[i].x && e->xbutton.x<hits[i].x+hits[i].w && e->xbutton.y>=hits[i].y && e->xbutton.y<hits[i].y+hits[i].h) {
            pending=hits[i].ws; commit(hits[i].id); break;
        }
        return 1;
    case MotionNotify:
        /* The tile or workspace card under the pointer becomes the selection. */
        if (e->xmotion.window==overlay)
            for (int i=0;i<nhits;i++) if (e->xmotion.x>=hits[i].x && e->xmotion.x<hits[i].x+hits[i].w && e->xmotion.y>=hits[i].y && e->xmotion.y<hits[i].y+hits[i].h) {
                if (expose && i!=esel) { esel=i; draw(); }
                else if (!expose && hits[i].ws!=pending) overview_select(hits[i].ws);
                break;
            }
        return 1;
    case EnterNotify: case LeaveNotify: case FocusIn: case FocusOut: case ButtonRelease: return 1;
    default: return 0; /* GenericEvent cookie remains owned by main. */
    }
}
