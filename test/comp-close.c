/* Production named-pixmap transfer, never connects to inherited DISPLAY. */
#define main comp_main
#include "../src/comp.c"
#undef main
#include <X11/Xutil.h>
#include <assert.h>
#ifdef WITH_XCB_RES
#include <xcb/xcb.h>
#include <xcb/res.h>
static xcb_connection_t *observer;
static Window anchor;
static void resources(uint64_t *count, uint64_t *bytes) {
    XSync(dpy,False);
    xcb_res_query_client_resources_reply_t *r=xcb_res_query_client_resources_reply(observer,
        xcb_res_query_client_resources(observer,(uint32_t)anchor),NULL);
    assert(r); *count=0;
    xcb_res_type_iterator_t it=xcb_res_query_client_resources_types_iterator(r);
    for(;it.rem;xcb_res_type_next(&it)) *count+=it.data->count;
    free(r);
    xcb_res_query_client_pixmap_bytes_reply_t *p=xcb_res_query_client_pixmap_bytes_reply(observer,
        xcb_res_query_client_pixmap_bytes(observer,(uint32_t)anchor),NULL);
    assert(p); *bytes=((uint64_t)p->bytes_overflow<<32)|p->bytes; free(p);
}
#endif

static unsigned long pixel(int x, int y) {
    XImage *im = XGetImage(dpy, back_buf, x,y,1,1,AllPlanes,ZPixmap);
    assert(im); unsigned long p=XGetPixel(im,0,0); XDestroyImage(im); return p;
}
static Window create(Display *owner) {
    Window w=XCreateSimpleWindow(owner,DefaultRootWindow(owner),40,40,100,80,2,0xffffff,0x336699);
    XMapWindow(owner,w); XSync(owner,False);
    assert(win_add(w)); dirty=stack_dirty=full_dirty=1; repaint();
    return w;
}
static void detach(Window id, long long now) {
    Win *w=win_get(id); assert(w && w->painted);
    closing_detach(w,now); w->mapped=0; animation_cancel(&w->opening); win_free_pix(w);
}
int main(int argc,char **argv) {
    assert(argc==2 && argv[1][0]==':');
    dpy=XOpenDisplay(argv[1]); assert(dpy);
    Display *owner=XOpenDisplay(argv[1]); assert(owner);
    screen=DefaultScreen(dpy); root=RootWindow(dpy,screen);
    int ev,err;
    assert(XDamageQueryExtension(dpy,&dmg_event,&dmg_error));
    assert(XRenderQueryExtension(dpy,&ev,&render_errbase));
    shape_ok=XShapeQueryExtension(dpy,&shape_event,&err);
    XSetErrorHandler(xerror_ignore);
    XCompositeRedirectSubwindows(dpy,root,CompositeRedirectManual);
    int n; XPixmapFormatValues *formats=XListPixmapFormats(dpy,&n); assert(formats);
    for(int i=0;i<n;i++) if(formats[i].depth>0 && formats[i].depth<=32) {
        exit_bpp[formats[i].depth]=(unsigned)formats[i].bits_per_pixel;
        exit_pad[formats[i].depth]=(unsigned)formats[i].scanline_pad;
    }
    XFree(formats);
    opt_shadow=0; opt_dim=1; opt_fade=1; opt_fade_ms=0; opt_close_ms=1000;
    Window id=create(owner);
    /* Source goes away BEFORE detachment. Neither detach nor paint may query it. */
    XDestroyWindow(owner,id); XSync(owner,False);
    long long now=now_ms(); detach(id,now); assert(exits.count==1);
    win_del(id);
    dirty=stack_dirty=full_dirty=1; repaint();
    assert(pixel(41,41)==0xffffff && pixel(50,50)==0x336699);
    ExitLayer *s=NULL;
    for(unsigned i=0;i<EXIT_COUNT;i++) if(exits.layers[i].bytes) s=&exits.layers[i];
    assert(s); s->animation.start_ms=now_ms()-500;
    dirty=full_dirty=1; repaint();
    unsigned long p=pixel(50,50); assert((p&255)>50 && (p&255)<110);
    exits_maintain(now+1000); assert(!exits.count && !exits.bytes);
    dirty=full_dirty=1; repaint(); assert(pixel(50,50)==0);
    puts("PASS: named pixmap survives destroy-before-detach, border/content fade and TTL cleanup");
    /* Warm fixed alpha cache before comparing server resource baselines. */
    for (unsigned i=0;i<ALPHA_SLOTS;i++) (void)alpha_mask((double)i/ALPHA_SLOTS);
#ifdef WITH_XCB_RES
    observer=xcb_connect(argv[1],NULL); assert(!xcb_connection_has_error(observer));
    anchor=XCreateSimpleWindow(dpy,root,0,0,1,1,0,0,0);
    uint64_t before_count,before_bytes,after_count,after_bytes;
    resources(&before_count,&before_bytes);
#endif
    for(unsigned i=0;i<256;i++) {
        id=create(owner); Win *w=win_get(id);
        if(i%4==0) {
            w->hide_reason=ANIMATION_HIDE_WORKSPACE;
            detach(id,now_ms()); assert(!exits.count);
        } else if(i%4==1) {
            w->hide_reason=ANIMATION_HIDE_MONOCLE;
            detach(id,now_ms()); assert(!exits.count);
        } else {
            detach(id,now_ms()); assert(exits.count==1);
            /* Reusing an identity cancels every older visual generation. */
            exit_cancel_identity(&exits,id,exit_release); assert(!exits.count);
        }
        XDestroyWindow(owner,id); XSync(owner,False); win_del(id);
        assert(!exits.count && !exits.bytes);
#ifdef WITH_XCB_RES
        resources(&after_count,&after_bytes);
        assert(before_count==after_count && before_bytes==after_bytes);
#endif
    }
#ifdef WITH_XCB_RES
    printf("METRIC: production 256 cycles XRes resources=%llu/%llu pixmap_bytes=%llu/%llu\n",
        (unsigned long long)before_count,(unsigned long long)after_count,
        (unsigned long long)before_bytes,(unsigned long long)after_bytes);
    XDestroyWindow(dpy,anchor); xcb_disconnect(observer);
#endif
    id=create(owner);
    XRectangle rect={0,0,20,20};
    XShapeCombineRectangles(owner,id,ShapeBounding,0,0,&rect,1,ShapeSet,Unsorted);
    XSync(owner,False);
    Win *w=win_get(id); win_free_pix(w);
    dirty=stack_dirty=full_dirty=1; repaint(); assert(w->shaped);
    detach(id,now_ms()); assert(exits.count==1);
    XDestroyWindow(owner,id); XSync(owner,False); win_del(id);
    dirty=stack_dirty=full_dirty=1; repaint();
    assert(pixel(50,50)!=0 && pixel(100,100)==0);
    exits_clear();
    XVisualInfo visual;
    assert(XMatchVisualInfo(owner,DefaultScreen(owner),32,TrueColor,&visual));
    XSetWindowAttributes attrs={0};
    attrs.colormap=XCreateColormap(owner,DefaultRootWindow(owner),visual.visual,AllocNone);
    attrs.background_pixel=0x80402010; attrs.border_pixel=0;
    id=XCreateWindow(owner,DefaultRootWindow(owner),40,40,100,80,0,32,InputOutput,
        visual.visual,CWColormap|CWBackPixel|CWBorderPixel,&attrs);
    XMapWindow(owner,id); XSync(owner,False); assert(win_add(id));
    dirty=stack_dirty=full_dirty=1; repaint();
    detach(id,now_ms()); assert(exits.count==1);
    XDestroyWindow(owner,id); XSync(owner,False); win_del(id);
    dirty=stack_dirty=full_dirty=1; repaint();
    assert(pixel(50,50)==0x402010);
    exits_clear(); XFreeColormap(owner,attrs.colormap);
    puts("PASS: production detached shaped holes and premultiplied ARGB");
    /* Fixed source opacity and dim multiply exit alpha, never properties. */
    id=create(owner); w=win_get(id);
    w->opacity=OPAQUE/2; opt_dim=0.5; active_win=None;
    detach(id,now_ms()); assert(exits.count==1);
    XDestroyWindow(owner,id); XSync(owner,False); win_del(id);
    dirty=stack_dirty=full_dirty=1; repaint();
    unsigned long dimmed=pixel(50,50);
    /* Match live rendering: opacity=.5, then black overlay alpha=.25. */
    assert((dimmed&255)>=52 && (dimmed&255)<=60);
    exits_clear(); opt_dim=1;
    /* A newer opaque live window must stay above an older closing layer. */
    id=create(owner); Window upper=create(owner);
    XSetWindowBackground(owner,upper,0x004400); XClearWindow(owner,upper); XSync(owner,False);
    dirty=stack_dirty=full_dirty=1; repaint(); detach(id,now_ms());
    XDestroyWindow(owner,id); XSync(owner,False); win_del(id);
    dirty=stack_dirty=full_dirty=1; repaint(); assert(pixel(50,50)==0x004400);
    exits_clear(); XDestroyWindow(owner,upper); XSync(owner,False); win_del(upper);
    puts("PASS: production closing opacity/dim and live-window stacking");
    /* Admission uses the actual server bpp/stride; no ninth retained image. */
    Window burst[EXIT_COUNT+1];
    for (unsigned i=0;i<=EXIT_COUNT;i++) burst[i]=create(owner);
    for (unsigned i=0;i<=EXIT_COUNT;i++) {
        detach(burst[i],now_ms());
        XDestroyWindow(owner,burst[i]); XSync(owner,False); win_del(burst[i]);
    }
    assert(exits.count==EXIT_COUNT && exits.bytes<=EXIT_BYTES);
    exits_clear(); assert(!exits.bytes);
    id=create(owner);
    XResizeWindow(owner,id,4096,2048); XSetWindowBorderWidth(owner,id,0); XSync(owner,False);
    w=win_get(id); win_free_pix(w); dirty=stack_dirty=full_dirty=1; repaint();
    detach(id,now_ms()); assert(exits.count==1 && exits.bytes==EXIT_BYTES);
    XDestroyWindow(owner,id); XSync(owner,False); win_del(id); exits_clear();
    /* Error callbacks must unpublish only at a safe loop boundary. */
    id=create(owner); detach(id,now_ms()); assert(exits.count==1);
    for (unsigned i=0;i<EXIT_COUNT;i++) if(exits.layers[i].bytes) {
        XRenderFreePicture(dpy,exits.layers[i].picture);
        XRenderComposite(dpy,PictOpOver,exits.layers[i].picture,None,back_pict,
            0,0,0,0,0,0,1,1);
    }
    XSync(dpy,False); assert(exit_xerror);
    exits_maintain(now_ms()); assert(!exits.count && !exits.bytes);
    XDestroyWindow(owner,id); XSync(owner,False); win_del(id);
    puts("PASS: production count/32MiB admission and asynchronous retained-Picture error cleanup");
    id=create(owner); w=win_get(id);
    opt_reduce_motion=1; closing_detach(w,now_ms()); assert(!exits.count);
    opt_reduce_motion=0; opt_close_ms=0; closing_detach(w,now_ms()); assert(!exits.count);
    opt_close_ms=100; opt_fade=0; closing_detach(w,now_ms()); assert(!exits.count);
    opt_fade=1; win_free_pix(w); closing_detach(w,now_ms()); assert(!exits.count);
    XDestroyWindow(owner,id); XSync(owner,False); win_del(id);
    XCompositeUnredirectSubwindows(dpy,root,CompositeRedirectManual);
    XCloseDisplay(owner); XCloseDisplay(dpy);
    puts("PASS: 256 close/hide/remap-cancellation cycles and instant/missing-image fallback");
    return 0;
}
