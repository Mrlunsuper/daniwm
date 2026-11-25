/* Root-child overview. Never owns application redirection or input focus. */
#include "overview.h"
#include "state.h"
#include "client.h"
#include "monitor.h"
#include "bar.h"
#include "xerr.h"
#include "keys.h"
#include <X11/keysym.h>
#include <X11/extensions/Xcomposite.h>
#include <X11/extensions/Xrender.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

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
static void text_color(int x,int y,int w,const char *s,const XftColor *color) {
    if (!xd || !barfont || !s || w<=0) return;
    XRectangle clip={(short)x,(short)(y-barfont->ascent),(unsigned short)w,(unsigned short)barfont->height};
    XftDrawSetClipRectangles(xd,0,0,&clip,1);
    FcChar8 safe[512];
    size_t used=utf8_safe(s,safe);
    XftDrawStringUtf8(xd,color,barfont,x,y,safe,(int)used);
    XftDrawSetClip(xd,NULL);
}
static int text_w(XftFont *f,const FcChar8 *s,size_t n) {
    XGlyphInfo g;
    XftTextExtentsUtf8(dpy,f,s,(int)n,&g);
    return g.xOff;
}
/* Draw s centered in [x, x+w); a too-wide string ends with an ellipsis. */
static void text_center(XftFont *f,int x,int y,int w,const char *s,const XftColor *color) {
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
    XftDrawStringUtf8(xd,color,f,x+(tw<w?(w-tw)/2:0),y,safe,(int)n);
    XftDrawSetClip(xd,NULL);
}
static void text(int x,int y,int w,const char *s) {
    text_color(x,y,w,s,&barcol.title);
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
static void rounded(int x,int y,int w,int h,int r,unsigned long color) {
    if (w<=0 || h<=0) return;
    if (r>w/2) r=w/2;
    if (r>h/2) r=h/2;
    if (r<1) { rect(x,y,w,h,color); return; }
    if (bpic) {
        /* Triangle fan around the center; the A8 mask smooths the arcs. */
        XPointFixed pt[2+4*17];
        int n=0;
        const double cx[4]={x+w-r,x+w-r,x+r,x+r}, cy[4]={y+r,y+h-r,y+h-r,y+r};
        pt[n].x=XDoubleToFixed(x+w/2.0); pt[n++].y=XDoubleToFixed(y+h/2.0);
        for (int k=0;k<4;k++) for (int i=0;i<=16;i++) {
            double c=quarter[i], sn=quarter[16-i];
            double dx=k==0?sn:k==1?c:k==2?-sn:-c, dy=k==0?-c:k==1?sn:k==2?c:-sn;
            pt[n].x=XDoubleToFixed(cx[k]+dx*r); pt[n++].y=XDoubleToFixed(cy[k]+dy*r);
        }
        pt[n++]=pt[1];
        XRenderColor rc={(unsigned short)((color>>16&255)*257),(unsigned short)((color>>8&255)*257),
                         (unsigned short)((color&255)*257),0xffff};
        Picture fill=XRenderCreateSolidFill(dpy,&rc);
        XRenderCompositeTriFan(dpy,PictOpOver,fill,bpic,XRenderFindStandardFormat(dpy,PictStandardA8),
                               0,0,pt,n);
        XRenderFreePicture(dpy,fill);
        return;
    }
    rect(x+r,y,w-2*r,h,color);
    rect(x,y+r,w,h-2*r,color);
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
/* One window card: cached preview (or a placeholder) above the title. */
static void card(int cx,int cy,int cardw,int cardh,int ws,Client *c,unsigned long well) {
    int line=barfont->height+S(8), inset=S(8), imageh=cardh-line-2*inset;
    rounded(cx,cy,cardw,cardh,S(8),well);
    hit(cx,cy,cardw,cardh,ws,c->win);
    int cached=0;
    for (int i=0;i<CACHE;i++) if (cache[i].id==c->win && cache[i].pm) {
        int w=cache[i].w; if (w>cardw-2*inset) w=cardw-2*inset;
        int h=cache[i].h; if (h>imageh) h=imageh;
        if (w>0 && h>0) {
            XCopyArea(dpy,cache[i].pm,backing,gc,(cache[i].w-w)/2,(cache[i].h-h)/2,
                      w,h,cx+(cardw-w)/2,cy+inset+(imageh-h)/2);
            cached=1;
        }
        break;
    }
    if (!cached && imageh>=line)
        text_color(cx+inset,cy+inset+imageh/2+barfont->ascent/2,
                   cardw-2*inset,"Preview unavailable",&barcol.sep);
    unsigned char *title=NULL; Atom type; int format; unsigned long n,after;
    trap_errors(dpy);
    if (XGetWindowProperty(dpy,c->win,A_NET_WM_NAME,0,128,False,AnyPropertyType,&type,&format,&n,&after,&title)==Success && title && format==8)
        text(cx+inset,cy+cardh-inset-barfont->descent,cardw-2*inset,(char *)title);
    else text(cx+inset,cy+cardh-inset-barfont->descent,cardw-2*inset,"Window");
    if (title) XFree(title);
    untrap_errors(dpy);
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
    int cellh=S(12)+tile+S(10)+fh+S(12), headh=fh+S(14), footh=fh+S(22);
    unsigned long panel=blend(BAR_BG,BAR_FG,9), rule=blend(panel,BAR_FG,10);
    int n=0;
    for (Client *c=clients;c;c=c->next) if (c->ws==curws) n++;
    if (snap) XCopyArea(dpy,snap,backing,gc,0,0,width,height,0,0);
    else rect(0,0,width,height,blend(BAR_BG,0,30));
    int maxcols=(width-2*S(48)-2*pad)/cellw;
    if (maxcols<1) maxcols=1;
    ecols=n<maxcols?(n>0?n:1):maxcols;
    int rows=n?(n+ecols-1)/ecols:1;
    int pw=ecols*cellw+2*pad, ph=pad+headh+rows*cellh+footh+pad/2;
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
    int pad=S(12), gap=S(16), radius=S(12);
    int fh=barfont->height, line=fh+S(8);
    int margin=S(28), header=2*line+S(16), footer=line+S(12);
    int rows=(NWS+cols-1)/cols;
    /* Keep the controller usable even on very small or heavily scaled outputs. */
    if (width<2*margin+cols*S(96) ||
        height<2*margin+header+footer+rows*(3*line+pad))
        margin=gap=header=footer=0;
    int gw=width-2*margin, gh=height-2*margin-header-footer;
    int cw=(gw-(cols-1)*gap)/cols, ch=(gh-(rows-1)*gap)/rows;
    unsigned long backdrop=blend(BAR_BG,0,22);
    unsigned long surface=blend(BAR_BG,BAR_FG,5);
    unsigned long edge=blend(BAR_BG,BAR_FG,14);
    unsigned long well=blend(BAR_BG,0,12);
    long long now=mono_ms();
    rect(0,0,width,height,backdrop);
    if (header) {
        char summary[80];
        text(margin,margin+barfont->ascent,gw,"Workspaces");
        snprintf(summary,sizeof summary,"%d workspaces  /  Choose your next space",NWS);
        text_color(margin,margin+barfont->ascent+line,gw,summary,&barcol.sep);
    }
    if (footer)
        text_color(margin,height-margin-barfont->descent,gw,
                   "Arrow keys: select    Enter / click: open    Esc: cancel    Previews: cached",&barcol.sep);
    for (int ws=0;ws<NWS && ws<MAXWS;ws++) {
        int row=ws/cols, inrow=NWS-row*cols;
        if (inrow>cols) inrow=cols;
        int x=margin+(gw-(inrow*cw+(inrow-1)*gap))/2+(ws%cols)*(cw+gap);
        int y=margin+header+row*(ch+gap);
        int weight=highlight(ws,now);
        int selected=weight>=50; /* text colour and border width switch half-way */
        unsigned long fill=blend(surface,blend(surface,BAR_ACC,10),(unsigned)weight);
        if (gap) rounded(x,y+S(3),cw,ch,radius,blend(backdrop,0,30));
        rounded(x,y,cw,ch,radius,blend(edge,BAR_ACC,(unsigned)weight));
        int border=selected?2:1;
        rounded(x+border,y+border,cw-2*border,ch-2*border,radius-border,fill);
        hit(x,y,cw,ch,ws,None);
        if (cw<2*pad+line || ch<3*line+pad) continue;
        int badge=line, badgew=badge+S(8), by=y+pad;
        rounded(x+pad,by,badgew,badge,S(6),blend(edge,BAR_ACC,(unsigned)weight));
        char label[128], number[8];
        snprintf(number,sizeof number,"%d",ws+1);
        /* The badge uses the theme's active-workspace contrast pair. */
        text_color(x+pad+S(5),by+(badge-fh)/2+barfont->ascent,badgew-S(8),number,
                   selected?&barcol.ws_acttx:&barcol.title);
        const char *name=ws_name(ws);
        if (name && *name) snprintf(label,sizeof label,"%s",name);
        else snprintf(label,sizeof label,"Workspace %d",ws+1);
        int statusw=cw>S(260)?S(76):0;
        text(x+2*pad+badgew,by+(badge-fh)/2+barfont->ascent,
             cw-3*pad-badgew-statusw,label);
        if (statusw && ws==curws)
            text_color(x+cw-pad-statusw,by+(badge-fh)/2+barfont->ascent,
                       statusw,"CURRENT",&barcol.ws_occ);
        int count=0;
        for (Client *c=clients;c;c=c->next) if (c->ws==ws) count++;
        int top=by+badge+pad, bottom=y+ch-pad-line;
        int available=bottom-top, cardgap=S(10);
        int cardcols=cw-2*pad>=2*(TW+2*S(8))+cardgap?2:1;
        int cardw=(cw-2*pad-(cardcols-1)*cardgap)/cardcols;
        int cardh=TH+line+2*S(8);
        int cardrows=(available+cardgap)/(cardh+cardgap);
        if (cardrows<1 && available>=line+2*S(8)) { cardrows=1; cardh=available; }
        int shown=0, capacity=cardrows*cardcols;
        if (!count && available>=line) {
            text_color(x+pad,top+available/2+barfont->ascent/2,cw-2*pad,
                       "Empty workspace",&barcol.sep);
        }
        for (Client *c=clients;c && shown<capacity;c=c->next) if (c->ws==ws) {
            int cx=x+pad+(shown%cardcols)*(cardw+cardgap);
            int cy=top+(shown/cardcols)*(cardh+cardgap);
            card(cx,cy,cardw,cardh,ws,c,well);
            shown++;
        }
        if (count>shown) snprintf(label,sizeof label,"%d windows  /  +%d more",count,count-shown);
        else snprintf(label,sizeof label,"%d window%s",count,count==1?"":"s");
        text_color(x+pad,y+ch-pad-barfont->descent,cw-2*pad,label,&barcol.sep);
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
    if (backing) XFreePixmap(dpy,backing);
    if (snap) XFreePixmap(dpy,snap);
    if (overlay) { animation_overview_role(None); XDestroyWindow(dpy,overlay); }
    untrap_errors(dpy);
    xd=NULL; gc=NULL; backing=None; snap=None; bpic=None; overlay=None;
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
    XRenderPictFormat *pf=XRenderFindVisualFormat(dpy,DefaultVisual(dpy,screen));
    bpic=pf?XRenderCreatePicture(dpy,backing,pf,0,NULL):None;
    if (expose && gc) snapshot(mons[m].x,mons[m].y); /* before the overlay covers it */
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
        /* Exposé: the card under the pointer becomes the selection. */
        if (expose && e->xmotion.window==overlay)
            for (int i=0;i<nhits;i++) if (e->xmotion.x>=hits[i].x && e->xmotion.x<hits[i].x+hits[i].w && e->xmotion.y>=hits[i].y && e->xmotion.y<hits[i].y+hits[i].h) {
                if (i!=esel) { esel=i; draw(); }
                break;
            }
        return 1;
    case EnterNotify: case LeaveNotify: case FocusIn: case FocusOut: case ButtonRelease: return 1;
    default: return 0; /* GenericEvent cookie remains owned by main. */
    }
}
