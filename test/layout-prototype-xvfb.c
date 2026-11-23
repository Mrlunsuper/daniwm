/* Mandatory Phase 2 experiment only. Separate WM, compositor, app and input
 * X connections, with deliberately controlled adverse processing order.
 * Does NOT run a replacement WM, alter production code, grab input/server,
 * or treat Xvfb as hardware/physical-input evidence. */
#define main comp_main
#include "../src/comp.c"
#undef main
#include "layout-prototype.h"
#include "layout-eligibility.h"
#include <X11/Xutil.h>
#include <X11/extensions/XInput2.h>
#include <assert.h>
#include <dlfcn.h>
#include <inttypes.h>
#ifdef WITH_XCB_RES
#include <xcb/xcb.h>
#include <xcb/res.h>
#endif

static Display *wm, *app, *input;
static Window wm_owner, app_windows[2];
static Atom capability, payload, message;
static ExitPool images;
static uint64_t generations[2] = {1,2};
static unsigned configure_events[2], client_presses[2], client_enters[2];
static unsigned raw_buttons, raw_motion;
static Window visible_cover;
static unsigned cover_presses;
static int xi_opcode;
static int (*fake_motion)(Display *, int, int, int, unsigned long);
static int (*fake_button)(Display *, unsigned int, Bool, unsigned long);
static void *xtst;
static int input_available;
static unsigned safe_captures, failed_captures;
static long long capture_us[128];
static unsigned capture_n;

static long long mono_us(void) {
    struct timespec t; assert(clock_gettime(CLOCK_MONOTONIC,&t)==0);
    return (long long)t.tv_sec*1000000+t.tv_nsec/1000;
}
static void pause_ms(long ms) {
    struct timespec t={ms/1000,(ms%1000)*1000000};
    while(nanosleep(&t,&t)<0) assert(errno==EINTR);
}
static LPBatch batch(uint32_t seq, int reverse) {
    LPBatch b={.wm=(uint32_t)wm_owner,.compositor=(uint32_t)cm_win,.epoch=7,
        .sequence=seq,.duration=160,.op=LP_PREPARE,.count=2};
    for(unsigned i=0;i<2;i++) {
        int x=(int)i*220+40, target=(int)(1-i)*220+40;
        b.clients[i]=(LPClient){(uint32_t)app_windows[i],generations[i],
            {reverse?target:x,60,reverse?180:160,reverse?100:120},
            {reverse?x:target,60,reverse?160:180,reverse?120:100}};
    }
    return b;
}
static void send_batch(Display *from, Window property_window, Window destination, const LPBatch *b) {
    unsigned long words[LP_WORDS]; size_t n=lp_encode(b,words);
    XChangeProperty(from,property_window,payload,XA_CARDINAL,32,PropModeReplace,
        (unsigned char *)words,(int)n);
    XEvent e={0}; e.xclient.type=ClientMessage; e.xclient.window=destination;
    e.xclient.message_type=message; e.xclient.format=32;
    e.xclient.data.l[0]=LP_VERSION; e.xclient.data.l[1]=(long)b->sequence;
    e.xclient.data.l[2]=(long)b->op; e.xclient.data.l[3]=(long)b->epoch;
    e.xclient.data.l[4]=(long)property_window;
    assert(XSendEvent(from,destination,False,NoEventMask,&e)); XFlush(from);
}
static int receive_batch(Display *connection, Window property_window, LPBatch *b) {
    /* A property may be overwritten before its notification is consumed.
     * Match its bounded content against the queued envelope; never silently
     * attach a newer batch to an older PREPARE/READY/COMMIT notification. */
    XSync(connection,False);
    XEvent event;
    Window destination=connection==wm ? wm_owner : cm_win;
    if(!XCheckTypedWindowEvent(connection,destination,ClientMessage,&event) ||
        event.xclient.message_type!=message || event.xclient.format!=32 ||
        event.xclient.data.l[0]!=LP_VERSION || (Window)event.xclient.data.l[4]!=property_window ||
        XGetSelectionOwner(connection,A_CM)!=cm_win) return 0;
    Atom type; int format; unsigned long n=0,extra=0; unsigned char *data=NULL;
    int ok=XGetWindowProperty(connection,property_window,payload,0,LP_WORDS,False,XA_CARDINAL,
        &type,&format,&n,&extra,&data)==Success && type==XA_CARDINAL && format==32 &&
        lp_decode(b,(unsigned long *)data,n,extra,(uint32_t)wm_owner,(uint32_t)cm_win,7);
    if(ok) ok=(uint32_t)event.xclient.data.l[1]==b->sequence &&
        (uint32_t)event.xclient.data.l[2]==b->op && (uint32_t)event.xclient.data.l[3]==b->epoch;
    if(data) XFree(data);
    return ok;
}
static int compatible(void) {
    Atom type; int format; unsigned long n,extra; unsigned char *data=NULL;
    if(XGetSelectionOwner(wm,A_CM)!=cm_win) return 0;
    int ok=XGetWindowProperty(wm,cm_win,capability,0,4,False,XA_CARDINAL,
        &type,&format,&n,&extra,&data)==Success && type==XA_CARDINAL && format==32 &&
        n==4 && !extra && data;
    if(ok) {
        unsigned long *v=(unsigned long *)data;
        ok=v[0]==LP_VERSION && v[1]==cm_win && v[2]==7 && v[3]==LP_CLIENTS;
    }
    if(data) XFree(data);
    return ok;
}
static void app_events(void) {
    XSync(app,False);
    while(XPending(app)) {
        XEvent e; XNextEvent(app,&e);
        if (e.type==ButtonPress && e.xbutton.window==visible_cover) cover_presses++;
        for(unsigned i=0;i<2;i++) if(e.xany.window==app_windows[i]) {
            if(e.type==ConfigureNotify) configure_events[i]++;
            if(e.type==ButtonPress) client_presses[i]++;
            if(e.type==EnterNotify && e.xcrossing.mode==NotifyNormal) client_enters[i]++;
        }
    }
}
static void raw_events(void) {
    XSync(dpy,False);
    while(XPending(dpy)) {
        XEvent e; XNextEvent(dpy,&e);
        if(e.type==GenericEvent && e.xcookie.extension==xi_opcode && XGetEventData(dpy,&e.xcookie)) {
            if(e.xcookie.evtype==XI_RawButtonPress) raw_buttons++;
            if(e.xcookie.evtype==XI_RawMotion) raw_motion++;
            XFreeEventData(dpy,&e.xcookie);
        }
    }
}
static void commit_real(const LPBatch *b) {
    for(unsigned i=0;i<b->count;i++) {
        LPRect r=b->clients[i].target;
        /* The only geometry request per client for a transaction. */
        XMoveResizeWindow(wm,b->clients[i].id,r.x,r.y,(unsigned)r.w,(unsigned)r.h);
    }
    XFlush(wm); /* no READY wait or synchronous server call in this path */
}
static void set_old(const LPBatch *b) {
    for(unsigned i=0;i<2;i++) {
        LPRect r=b->clients[i].old;
        XMoveResizeWindow(wm,b->clients[i].id,r.x,r.y,(unsigned)r.w,(unsigned)r.h);
    }
    XSync(wm,False); app_events();
    memset(configure_events,0,sizeof configure_events);
    memset(client_presses,0,sizeof client_presses); memset(client_enters,0,sizeof client_enters);
}
static void release_owned(unsigned long picture,unsigned long pixmap) {
    /* Same staged X-error convention as production, no invalid handle retained. */
    trap_errors(); exit_release(picture,pixmap); assert(!untrap_errors());
}
static int capture_one(const LPClient *c,unsigned index,long long captured) {
    if(index>=2 || c->id!=app_windows[index] || c->generation!=generations[index]) return 0;
    XWindowAttributes a;
    trap_errors(); int valid=XGetWindowAttributes(dpy,c->id,&a); int error=untrap_errors();
    if(error || !valid || a.map_state!=IsViewable || a.override_redirect || a.border_width ||
        a.depth!=24 || a.x!=c->old.x || a.y!=c->old.y ||
        a.width!=c->old.w || a.height!=c->old.h) return 0;
    /* This gate experiment renders only unshaped opaque depth-24 clients;
     * production shape/ARGB/border interpolation is deliberately deferred. */
    Bool bounding,clip; int bx,by,cx,cy; unsigned bw,bh,cw,ch;
    trap_errors();
    valid=XShapeQueryExtents(dpy,c->id,&bounding,&bx,&by,&bw,&bh,&clip,&cx,&cy,&cw,&ch);
    error=untrap_errors(); if(error || !valid || bounding) return 0;
    size_t bytes;
    if(!exit_size((unsigned)a.width,(unsigned)a.height,32,32,&bytes) ||
        images.count>=EXIT_COUNT || bytes>EXIT_BYTES-images.bytes) return 0;
    XRenderPictFormat *sf=XRenderFindVisualFormat(dpy,a.visual);
    XRenderPictFormat *df=XRenderFindStandardFormat(dpy,PictStandardARGB32);
    if(!sf || !df) return 0;
    Pixmap named=None,copy=None; Picture source=None,target=None; int ok=0;
    trap_errors(); Pixmap candidate=XCompositeNameWindowPixmap(dpy,c->id); error=untrap_errors();
    if(error || !candidate) return 0;
    named=candidate;
    Window rr; int px,py; unsigned pw,ph,pb,pd;
    trap_errors(); valid=XGetGeometry(dpy,named,&rr,&px,&py,&pw,&ph,&pb,&pd); error=untrap_errors();
    if(error || !valid || pw!=(unsigned)a.width || ph!=(unsigned)a.height || pd!=(unsigned)a.depth) goto done;
    trap_errors(); candidate=XCreatePixmap(dpy,root,(unsigned)a.width,(unsigned)a.height,32);
    error=untrap_errors(); if(error || !candidate) goto done;
    copy=candidate;
    trap_errors(); Picture pc=XRenderCreatePicture(dpy,named,sf,0,NULL); error=untrap_errors();
    if(error || !pc) goto done;
    source=pc;
    trap_errors(); pc=XRenderCreatePicture(dpy,copy,df,0,NULL); error=untrap_errors();
    if(error || !pc) goto done;
    target=pc;
    trap_errors();
    XTransform identity={{{65536,0,0},{0,65536,0},{0,0,65536}}};
    XRenderPictureAttributes pa={0}; pa.clip_mask=None;
    XRenderSetPictureTransform(dpy,source,&identity); XRenderSetPictureFilter(dpy,source,FilterNearest,NULL,0);
    XRenderChangePicture(dpy,source,CPClipMask,&pa);
    XRenderComposite(dpy,PictOpSrc,source,None,target,0,0,0,0,0,0,(unsigned)a.width,(unsigned)a.height);
    error=untrap_errors(); if(error) goto done;
    /* Reject an observed concurrent geometry change rather than attach a
     * final-size image to an old-size transaction. No server grab closes
     * the remaining observation gap; that is part of the prototype gate. */
    trap_errors(); valid=XGetWindowAttributes(dpy,c->id,&a); error=untrap_errors();
    if(error || !valid || a.map_state!=IsViewable || a.x!=c->old.x || a.y!=c->old.y ||
        a.width!=c->old.w || a.height!=c->old.h) goto done;
    ExitLayer s={.identity=c->id,.generation=c->generation,.pixmap=copy,.picture=target,
        .bytes=bytes,.retained=captured,.width=a.width,.height=a.height};
    animation_start(&s.animation,c->id,c->generation,captured,LP_TTL_MS);
    if(!exit_admit(&images,&s)) goto done;
    copy=None; target=None; ok=1;
done:
    trap_errors();
    if(source) XRenderFreePicture(dpy,source);
    if(named) XFreePixmap(dpy,named);
    if(target) XRenderFreePicture(dpy,target);
    if(copy) XFreePixmap(dpy,copy);
    assert(!untrap_errors());
    return ok;
}
static int capture_batch(const LPBatch *b,long long prepared) {
    exit_clear(&images,release_owned);
    long long start=mono_us();
    for(unsigned i=0;i<b->count;i++) if(!capture_one(&b->clients[i],i,prepared)) {
        failed_captures++; exit_clear(&images,release_owned); return 0;
    }
    assert(images.count==b->count && images.bytes<=EXIT_BYTES);
    safe_captures++;
    if(capture_n<128) capture_us[capture_n++]=mono_us()-start;
    return 1;
}
static void paint_batch(const LPBatch *b,int64_t start,int64_t sample) {
    XRenderColor black={0,0,0,65535};
    XRenderFillRectangle(dpy,PictOpSrc,back_pict,&black,0,0,640,480);
    for(unsigned i=0;i<b->count;i++) {
        const LPClient *c=&b->clients[i]; const ExitLayer *s=NULL;
        for(unsigned j=0;j<EXIT_COUNT;j++) if(images.layers[j].bytes &&
            images.layers[j].identity==c->id && images.layers[j].generation==c->generation) s=&images.layers[j];
        assert(s);
        LPRect r=lp_sample(*c,start,b->duration,sample);
        XTransform transform={{{XDoubleToFixed((double)s->width/r.w),0,0},
            {0,XDoubleToFixed((double)s->height/r.h),0},{0,0,65536}}};
        XRenderPictureAttributes pa={0}; pa.clip_mask=None;
        XRenderChangePicture(dpy,s->picture,CPClipMask,&pa);
        XRenderSetPictureTransform(dpy,s->picture,&transform);
        XRenderSetPictureFilter(dpy,s->picture,FilterBilinear,NULL,0);
        XRenderComposite(dpy,PictOpOver,s->picture,None,back_pict,0,0,0,0,
            r.x,r.y,(unsigned)r.w,(unsigned)r.h);
        XTransform identity={{{65536,0,0},{0,65536,0},{0,0,65536}}};
        XRenderSetPictureTransform(dpy,s->picture,&identity);
        XRenderSetPictureFilter(dpy,s->picture,FilterNearest,NULL,0);
        XRenderChangePicture(dpy,s->picture,CPClipMask,&pa);
    }
    /* Full repaint; no opaque occlusion shortcut or per-frame X geometry. */
    XCopyArea(dpy,back_buf,root,present_gc,0,0,640,480,0,0); XFlush(dpy);
}
static unsigned long pixel(void) {
    XImage *im=XGetImage(dpy,root,80,100,1,1,AllPlanes,ZPixmap); assert(im);
    unsigned long value=XGetPixel(im,0,0); XDestroyImage(im); return value;
}
static int load_input(void) {
    xtst=dlopen("libXtst.so.6",RTLD_NOW|RTLD_LOCAL);
    if(!xtst) return 0;
    void *motion=dlsym(xtst,"XTestFakeMotionEvent"), *button=dlsym(xtst,"XTestFakeButtonEvent");
    if(!motion || !button || sizeof motion!=sizeof fake_motion || sizeof button!=sizeof fake_button) return 0;
    memcpy(&fake_motion,&motion,sizeof motion); memcpy(&fake_button,&button,sizeof button);
    int event,error,opcode; if(!XQueryExtension(input,"XTEST",&opcode,&event,&error)) return 0;
    int maj=2,min=0,err;
    if(!XQueryExtension(dpy,"XInputExtension",&xi_opcode,&event,&err) || XIQueryVersion(dpy,&maj,&min)!=Success) return 0;
    unsigned char mask[XIMaskLen(XI_RawMotion)]={0};
    XISetMask(mask,XI_RawMotion); XISetMask(mask,XI_RawButtonPress);
    XIEventMask em={.deviceid=XIAllMasterDevices,.mask_len=(int)sizeof mask,.mask=mask};
    assert(XISelectEvents(dpy,root,&em,1)==Success); XSync(dpy,False); return 1;
}
static void input_experiment(void) {
    if(!input_available) { puts("BLOCKED: click/hover prototype needs runtime libXtst/XTEST and XI2"); return; }
    LPBatch b=batch(1000,0); set_old(&b);
    /* Stationary pointer: geometry crossing can focus a final-position client
     * without physical/raw motion. Keep the old picture visible on purpose. */
    assert(fake_motion(input,screen,80,100,CurrentTime)); XSync(input,False);
    app_events(); raw_events(); raw_motion=raw_buttons=0;
    memset(client_enters,0,sizeof client_enters);
    long long start=now_ms(); assert(capture_batch(&b,start));
    commit_real(&b); XSync(wm,False); paint_batch(&b,start,start); XSync(dpy,False);
    assert(pixel()==0xaa0000);
    app_events();
    unsigned stationary_enters=client_enters[1];
    assert(stationary_enters>0);
    /* Delay only compositor processing, NEVER application delivery. The
     * server already routes the press using final real geometry. */
    long long injected=mono_us();
    assert(fake_button(input,Button1,True,CurrentTime));
    assert(fake_button(input,Button1,False,CurrentTime)); XSync(input,False);
    app_events(); long long delivered=mono_us();
    assert(client_presses[0]==0 && client_presses[1]==1);
    assert(pixel()==0xaa0000); /* red A visual, green B received click */
    pause_ms(25); raw_events(); assert(raw_buttons==1 && raw_motion==0);
    /* Raw observation is not an input barrier: cancellation fixes only later
     * pixels. It cannot undo the click/Enter already delivered to the app. */
    paint_batch(&b,start,start+b.duration); XSync(dpy,False);
    assert(pixel()==0x00aa00);
    long long snapped=mono_us();
    printf("METRIC: stationary_hover_crossings=%u raw_motion_before_cancel=%u wrong_visual_target_clicks=1/1 "
        "input_inject_to_app_us=%lld input_inject_to_snap_us=%lld\n",stationary_enters,raw_motion,
        delivered-injected,snapped-injected);
    /* Even when draining a raw motion notification before the following
     * button notification, the server is not required to wait for repaint. */
    memset(client_presses,0,sizeof client_presses); paint_batch(&b,start,start); XSync(dpy,False);
    assert(fake_motion(input,screen,81,101,CurrentTime));
    assert(fake_button(input,Button1,True,CurrentTime));
    assert(fake_button(input,Button1,False,CurrentTime)); XSync(input,False);
    app_events(); assert(client_presses[1]==1 && pixel()==0xaa0000);
    raw_events(); assert(raw_motion>0);
    puts("FINDING: raw click/motion cancellation cannot guarantee snap-before-delivery without changing input semantics");
    exit_clear(&images,release_owned);
}
/* Re-check proposed conservative admission conditions. These are adverse
 * schedules, not production policy, and NEVER delay the app/server's input. */
static void eligibility_experiment(void) {
    if (!input_available) {
        puts("BLOCKED: eligibility input tests require the preceding XTEST/XI2 setup");
        return;
    }
    unsigned outside_mismatches=0, cover_revocations=0, instant_controls=0;
    LEObservation observation={.compositor=1,.enabled=1};
    for(unsigned cycle=0;cycle<8;cycle++) {
        LPBatch b=batch(2000+cycle,0); set_old(&b);
        /* PREPARE-time pointer is far outside both swept client rectangles. */
        assert(fake_motion(input,screen,600,420,CurrentTime)); XSync(input,False);
        app_events(); raw_events();
        Window rr,child; int rx,ry,wx,wy; unsigned mask;
        assert(XQueryPointer(dpy,root,&rr,&child,&rx,&ry,&wx,&wy,&mask));
        assert(rx==600 && ry==420);
        observation.pointer_outside=1; observation.input_transparent_visual=1;
        assert(le_decide(&b,observation)==LE_UNPROVEN_INPUT_GEOMETRY);
        long long start=now_ms(); assert(capture_batch(&b,start));
        commit_real(&b); XSync(wm,False); paint_batch(&b,start,start); XSync(dpy,False);
        /* Pictures drawn to the root have no input window at all: already
         * the strongest input-transparent visual, without any shield. */
        memset(client_presses,0,sizeof client_presses);
        raw_motion=raw_buttons=0;
        assert(fake_motion(input,screen,80,100,CurrentTime));
        assert(fake_button(input,Button1,True,CurrentTime));
        assert(fake_button(input,Button1,False,CurrentTime)); XSync(input,False);
        app_events();
        assert(client_presses[0]==0 && client_presses[1]==1 && pixel()==0xaa0000);
        outside_mismatches++;
        raw_events(); assert(raw_motion>0 && raw_buttons==1);
        paint_batch(&b,start,start+b.duration); XSync(dpy,False);
        assert(pixel()==0x00aa00);
        /* Final-rectangle instant control, same routing, no transformed stale
         * image. This validates fallback, not completion of layout effects. */
        memset(client_presses,0,sizeof client_presses);
        assert(fake_button(input,Button1,True,CurrentTime));
        assert(fake_button(input,Button1,False,CurrentTime)); XSync(input,False);
        app_events(); assert(client_presses[1]==1 && pixel()==0x00aa00); instant_controls++;
        app_events(); assert(configure_events[0]==1 && configure_events[1]==1);
        exit_clear(&images,release_owned);
    }
    /* An ordinary VISIBLE covering application, never an animation-created
     * input shield or overview. Its owner may withdraw at any instant. */
    visible_cover=XCreateSimpleWindow(app,root,20,40,450,170,0,0,0x0000aa);
    XSelectInput(app,visible_cover,ButtonPressMask|StructureNotifyMask);
    for(unsigned cycle=0;cycle<8;cycle++) {
        LPBatch b=batch(3000+cycle,0); set_old(&b);
        XMapRaised(app,visible_cover); XSync(app,False);
        assert(fake_motion(input,screen,80,100,CurrentTime)); XSync(input,False);
        app_events(); raw_events(); raw_motion=raw_buttons=0; cover_presses=0;
        observation.covered=1;
        assert(le_decide(&b,observation)==LE_UNPROVEN_INPUT_GEOMETRY);
        long long start=now_ms(); assert(capture_batch(&b,start));
        commit_real(&b); XSync(wm,False); paint_batch(&b,start,start);
        /* Normal compositor output with the visible cover above the batch. */
        XSetForeground(dpy,present_gc,0x0000aa);
        XFillRectangle(dpy,root,present_gc,20,40,450,170); XSync(dpy,False);
        assert(pixel()==0x0000aa);
        Window rr,child; int rx,ry,wx,wy; unsigned mask;
        assert(XQueryPointer(dpy,root,&rr,&child,&rx,&ry,&wx,&wy,&mask) && child==visible_cover);
        memset(client_presses,0,sizeof client_presses);
        assert(fake_button(input,Button1,True,CurrentTime));
        assert(fake_button(input,Button1,False,CurrentTime)); XSync(input,False);
        app_events(); assert(cover_presses==1 && !client_presses[0] && !client_presses[1]);
        /* Withdrawal is made by the cover's owner, not the WM and not held
         * until a compositor acknowledgement. Events are deliberately left
         * unprocessed on the compositor connection until after delivery. */
        XUnmapWindow(app,visible_cover); XSync(app,False);
        assert(fake_button(input,Button1,True,CurrentTime));
        assert(fake_button(input,Button1,False,CurrentTime)); XSync(input,False);
        app_events();
        assert(client_presses[1]==1 && pixel()==0x0000aa);
        cover_revocations++;
        raw_events();
        paint_batch(&b,start,start+b.duration); XSync(dpy,False);
        assert(pixel()==0x00aa00);
        exit_clear(&images,release_owned);
    }
    XDestroyWindow(app,visible_cover); XSync(app,False); visible_cover=None;
    /* Sampled empty ShapeInput is also revocable by the app. Restore the
     * DEFAULT shape in the experiment, without an input grab/shield. */
    LPBatch b=batch(4000,0); set_old(&b);
    for(unsigned i=0;i<2;i++) XShapeCombineRectangles(app,app_windows[i],ShapeInput,0,0,
        NULL,0,ShapeSet,Unsorted);
    XSync(app,False);
    for(unsigned i=0;i<2;i++) {
        int count,order; XRectangle *shape=XShapeGetRectangles(dpy,app_windows[i],ShapeInput,&count,&order);
        assert(count==0); if(shape) XFree(shape);
    }
    observation.input_shape_empty=1;
    assert(le_decide(&b,observation)==LE_UNPROVEN_INPUT_GEOMETRY);
    long long start=now_ms(); assert(capture_batch(&b,start));
    commit_real(&b); XSync(wm,False); paint_batch(&b,start,start); XSync(dpy,False);
    for(unsigned i=0;i<2;i++) XShapeCombineMask(app,app_windows[i],ShapeInput,0,0,None,ShapeSet);
    XSync(app,False); app_events(); memset(client_presses,0,sizeof client_presses);
    assert(fake_motion(input,screen,80,100,CurrentTime));
    assert(fake_button(input,Button1,True,CurrentTime));
    assert(fake_button(input,Button1,False,CurrentTime)); XSync(input,False);
    app_events(); assert(client_presses[1]==1 && pixel()==0xaa0000);
    raw_events(); paint_batch(&b,start,start+b.duration); XSync(dpy,False);
    exit_clear(&images,release_owned);
    printf("METRIC: eligibility pointer_outside_then_entry_mismatches=%u/8 "
        "transparent_picture_mismatches=%u/8 visible_cover_admission_revoked=%u/8 "
        "revoked_empty_input_shape_mismatches=1/1 instant_final_controls=%u/8 retained_final=%u/%zu\n",
        outside_mismatches,outside_mismatches,cover_revocations,instant_controls,images.count,images.bytes);
    puts("FINDING: sampled pointer/occlusion/input-shape conditions are not lifetime input guarantees; movement remains fail-closed");
}
#ifdef WITH_XCB_RES
static xcb_connection_t *resource_connection;
static void resource_sample(uint64_t *count,uint64_t *bytes) {
    XSync(dpy,False);
    xcb_res_query_client_resources_reply_t *r=xcb_res_query_client_resources_reply(resource_connection,
        xcb_res_query_client_resources(resource_connection,(uint32_t)cm_win),NULL); assert(r);
    *count=0; xcb_res_type_iterator_t it=xcb_res_query_client_resources_types_iterator(r);
    for(;it.rem;xcb_res_type_next(&it)) *count+=it.data->count;
    free(r);
    xcb_res_query_client_pixmap_bytes_reply_t *p=xcb_res_query_client_pixmap_bytes_reply(resource_connection,
        xcb_res_query_client_pixmap_bytes(resource_connection,(uint32_t)cm_win),NULL); assert(p);
    *bytes=((uint64_t)p->bytes_overflow<<32)|p->bytes; free(p);
}
#endif
int main(int argc,char **argv) {
    if(argc!=2 || argv[1][0]!=':') return 2;
    dpy=XOpenDisplay(argv[1]); wm=XOpenDisplay(argv[1]); app=XOpenDisplay(argv[1]); input=XOpenDisplay(argv[1]);
    assert(dpy && wm && app && input);
    screen=DefaultScreen(dpy); root=RootWindow(dpy,screen);
    int event,error; assert(XCompositeQueryExtension(dpy,&event,&error));
    assert(XRenderQueryExtension(dpy,&event,&render_errbase));
    assert(XShapeQueryExtension(dpy,&event,&error));
    int formats_n, argb_format=0;
    XPixmapFormatValues *formats=XListPixmapFormats(dpy,&formats_n); assert(formats);
    for(int i=0;i<formats_n;i++) if(formats[i].depth==32 && formats[i].bits_per_pixel==32 &&
        formats[i].scanline_pad==32) argb_format=1;
    XFree(formats); assert(argb_format); /* actual retained-pixmap stride */
    XSetErrorHandler(xerror_ignore);
    trap_errors(); XCompositeRedirectSubwindows(dpy,root,CompositeRedirectManual); assert(!untrap_errors());
    A_CM=XInternAtom(dpy,"_NET_WM_CM_S0",False);
    assert(XGetSelectionOwner(dpy,A_CM)==None);
    cm_win=XCreateSimpleWindow(dpy,root,0,0,1,1,0,0,0);
    wm_owner=XCreateSimpleWindow(wm,root,0,0,1,1,0,0,0);
    capability=XInternAtom(dpy,LP_CAP_ATOM,False); payload=XInternAtom(dpy,LP_DATA_ATOM,False);
    message=XInternAtom(dpy,LP_EVENT_ATOM,False);
    unsigned long caps[]={LP_VERSION,cm_win,7,LP_CLIENTS};
    XChangeProperty(dpy,cm_win,capability,XA_CARDINAL,32,PropModeReplace,(unsigned char *)caps,4);
    XSetSelectionOwner(dpy,A_CM,cm_win,CurrentTime); XSync(dpy,False); assert(compatible());
    caps[2]=8; XChangeProperty(dpy,cm_win,capability,XA_CARDINAL,32,PropModeReplace,(unsigned char *)caps,4);
    XSync(dpy,False); assert(!compatible()); caps[2]=7;
    XChangeProperty(dpy,cm_win,capability,XA_CARDINAL,32,PropModeReplace,(unsigned char *)caps,4); XSync(dpy,False);
    for(unsigned i=0;i<2;i++) {
        app_windows[i]=XCreateSimpleWindow(app,root,(int)i*220+40,60,160,120,0,0,i?0x00aa00:0xaa0000);
        XSelectInput(app,app_windows[i],StructureNotifyMask|ButtonPressMask|EnterWindowMask|PointerMotionMask);
        XMapWindow(app,app_windows[i]);
    }
    XSync(app,False);
    ensure_targets(640,480,DefaultVisual(dpy,screen),DefaultDepth(dpy,screen));
    present_gc=XCreateGC(dpy,root,0,NULL); assert(back_pict && present_gc);
    /* XISelectEvents can allocate a server-side selection resource. Include
     * this fixed observer overhead in baseline, not in snapshot leak deltas. */
    input_available=load_input();
#ifdef WITH_XCB_RES
    resource_connection=xcb_connect(argv[1],NULL); assert(!xcb_connection_has_error(resource_connection));
    uint64_t before_count,before_bytes,after_count,after_bytes; resource_sample(&before_count,&before_bytes);
#endif
    unsigned early=0,late=0,expired=0;
    long long max_submit_us=0;
    for(unsigned cycle=0;cycle<64;cycle++) {
        LPBatch b=batch(cycle+1,cycle%2), wire; LPState state={0}; set_old(&b);
        long long now=now_ms(); assert(lp_begin(&state,&b,now));
        send_batch(wm,wm_owner,cm_win,&b); XSync(wm,False);
        /* Two valid schedules on independent connections. No claim that
         * sending PREPARE forces the first ordering in real processes. */
        if(cycle%2) { commit_real(&b); XSync(wm,False); }
        assert(receive_batch(dpy,wm_owner,&wire));
        assert(wire.sequence==b.sequence);
        int captured=capture_batch(&wire,now), ready=0;
        if(cycle%2) { assert(!captured); late++; }
        else {
            assert(captured); early++;
            wire.op=LP_READY; send_batch(dpy,cm_win,wm_owner,&wire); XSync(dpy,False);
            assert(receive_batch(wm,cm_win,&wire));
            ready=lp_ready(&state,&wire,now_ms());
            if(!ready) expired++;
            long long submit_start=mono_us();
            commit_real(&b);
            long long submit_us=mono_us()-submit_start;
            if(submit_us>max_submit_us) max_submit_us=submit_us;
        }
        b.op=LP_COMMIT; send_batch(wm,wm_owner,cm_win,&b); XSync(wm,False);
        assert(receive_batch(dpy,wm_owner,&wire));
        int active=lp_commit(&state,&wire,now_ms());
        if(ready && !active) expired++;
        assert(!active || (captured && ready));
        if(active) {
            long long animation_start_ms=now_ms();
            for(;;) {
                long long sampled=now_ms();
                paint_batch(&b,animation_start_ms,sampled);
                if(sampled-animation_start_ms>=b.duration) break;
                pause_ms(FRAME_INTERVAL_MS);
            }
        }
        app_events(); assert(configure_events[0]==1 && configure_events[1]==1);
        exit_clear(&images,release_owned); assert(!images.count && !images.bytes);
#ifdef WITH_XCB_RES
        resource_sample(&after_count,&after_bytes);
        assert(after_count==before_count && after_bytes==before_bytes);
#endif
    }
    printf("METRIC: independent_connection_schedules old_capture_before_commit=%u "
        "late_prepare_skipped=%u deadline_expired=%u transactions=64 geometry_events_per_client_per_transaction=1 "
        "max_geometry_enqueue_us=%lld\n",early,late,expired,max_submit_us);
#ifdef WITH_XCB_RES
    printf("METRIC: 64 prototype cycles XRes resources=%" PRIu64 "/%" PRIu64 " pixmap_bytes=%" PRIu64 "/%" PRIu64 "\n",
        before_count,after_count,before_bytes,after_bytes);
#endif
    /* Late READY cannot hold or revive geometry; timeout and owner loss. */
    LPBatch b=batch(100,0), response=b; LPState s={0}; set_old(&b);
    long long now=now_ms(); assert(lp_begin(&s,&b,now)); assert(capture_batch(&b,now));
    commit_real(&b); XSync(wm,False); response.op=LP_READY;
    assert(!lp_ready(&s,&response,now+LP_PREPARE_MS)); lp_cancel(&s); exit_clear(&images,release_owned);
    XWindowAttributes a; assert(XGetWindowAttributes(app,app_windows[0],&a));
    assert(a.x==b.clients[0].target.x);
    XSetSelectionOwner(wm,A_CM,wm_owner,CurrentTime); XSync(wm,False); assert(!compatible());
    lp_cancel(&s); assert(s.phase==LP_IDLE); XSetSelectionOwner(dpy,A_CM,cm_win,CurrentTime); XSync(dpy,False);
    /* Malformed/stale real property payloads are bounded and rejected. */
    unsigned long invalid[LP_WORDS+1]={0}; lp_encode(&b,invalid); invalid[5]=LP_CLIENTS+1;
    send_batch(wm,wm_owner,cm_win,&b);
    XChangeProperty(wm,wm_owner,payload,XA_CARDINAL,32,PropModeReplace,(unsigned char *)invalid,LP_WORDS+1);
    XSync(wm,False); assert(!receive_batch(dpy,wm_owner,&response));
    b.sequence=101; send_batch(wm,wm_owner,cm_win,&b);
    b.sequence=102; send_batch(wm,wm_owner,cm_win,&b);
    XSync(wm,False);
    assert(!receive_batch(dpy,wm_owner,&response)); /* overwritten old envelope */
    assert(receive_batch(dpy,wm_owner,&response) && response.sequence==102);
    puts("PASS: owner/capability mismatch, malformed property and stale-overwritten notification rejected");
    /* Generation change between PREPARE and capture cannot bind reused IDs. */
    b=batch(103,0); set_old(&b); generations[0]++;
    assert(!capture_batch(&b,now_ms()) && !images.count);
    generations[0]--;
    /* A retained reference must expire even when no presentation is possible. */
    assert(capture_batch(&b,now_ms()));
    long long retained=0;
    for(unsigned i=0;i<EXIT_COUNT;i++) if(images.layers[i].bytes) retained=images.layers[i].retained;
    exit_expire(&images,retained+LP_TTL_MS,release_owned);
    assert(!images.count && !images.bytes);
    input_experiment();
    eligibility_experiment();
#ifdef WITH_XCB_RES
    resource_sample(&after_count,&after_bytes);
    assert(after_count==before_count && after_bytes==before_bytes);
    printf("METRIC: eligibility final XRes resources=%" PRIu64 "/%" PRIu64 " pixmap_bytes=%" PRIu64 "/%" PRIu64 "\n",
        before_count,after_count,before_bytes,after_bytes);
#endif
    for(unsigned i=0;i<capture_n;i++) for(unsigned j=i+1;j<capture_n;j++) if(capture_us[j]<capture_us[i]) {
        long long tmp=capture_us[i]; capture_us[i]=capture_us[j]; capture_us[j]=tmp;
    }
    if(capture_n) printf("METRIC: copied_two_client_capture_us n=%u p50=%lld p95=%lld max=%lld safe=%u skipped=%u\n",
        capture_n,capture_us[(capture_n-1)/2],capture_us[(capture_n-1)*95/100],capture_us[capture_n-1],safe_captures,failed_captures);
    exit_clear(&images,release_owned);
#ifdef WITH_XCB_RES
    xcb_disconnect(resource_connection);
#endif
    for(unsigned i=0;i<2;i++) XDestroyWindow(app,app_windows[i]);
    XSync(app,False);
    if(present_gc) XFreeGC(dpy,present_gc);
    if(back_pict) XRenderFreePicture(dpy,back_pict);
    if(back_buf) XFreePixmap(dpy,back_buf);
    if(bg_pix) XFreePixmap(dpy,bg_pix);
    XCompositeUnredirectSubwindows(dpy,root,CompositeRedirectManual);
    XDestroyWindow(wm,wm_owner); XDestroyWindow(dpy,cm_win);
    XCloseDisplay(input); XCloseDisplay(app); XCloseDisplay(wm); XCloseDisplay(dpy);
    if(xtst) dlclose(xtst);
    puts("PASS: prototype assertions completed; Phase 2 integration NOT approved by this test");
    return 0;
}
