#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/Xutil.h>
#include <X11/extensions/Xcomposite.h>
#include <X11/extensions/Xdamage.h>
#include <X11/extensions/Xrender.h>
#include <X11/extensions/Xfixes.h>
#include <strings.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
static Display *d;
static Atom g(Window w, const char *n){ return XInternAtom(d,n,True); }
static void prop_str(Window w, const char *name){
    Atom a=g(w,name); if(a==None) return;
    Atom rt; int rf; unsigned long n=0,ex=0; unsigned char *p=NULL;
    if(XGetWindowProperty(d,w,a,0,64,False,AnyPropertyType,&rt,&rf,&n,&ex,&p)==Success && p){
        if(rt==XA_ATOM){ printf("   %s=", name);
            Atom *at=(Atom*)p; for(unsigned long i=0;i<n;i++){ char *s=XGetAtomName(d,at[i]); printf("%s ",s?s:"?"); if(s)XFree(s);} printf("\n"); }
        else if(rf==8){ printf("   %s=%s\n", name, (char*)p); }
        else if(rf==32){ printf("   %s=", name); unsigned long *v=(unsigned long*)p; for(unsigned long i=0;i<n;i++) printf("0x%lx ",v[i]); printf("\n"); }
        XFree(p);
    }
}
int main(void){
    d=XOpenDisplay(NULL); if(!d){printf("no display\n");return 1;}
    int scr=DefaultScreen(d); Window root=RootWindow(d,scr);
    printf("server=%dx%d vendor=%s\n", DisplayWidth(d,scr), DisplayHeight(d,scr), ServerVendor(d));
    int ev,er;
    printf("Composite=%d Damage=%d Render=%d XFixes=%d GLX=%d\n",
        XCompositeQueryExtension(d,&ev,&er),
        XDamageQueryExtension(d,&ev,&er),
        XRenderQueryExtension(d,&ev,&er),
        XFixesQueryExtension(d,&ev,&er),
        0);
    char cm[32]; snprintf(cm,sizeof cm,"_NET_WM_CM_S%d",scr);
    Window owner=XGetSelectionOwner(d, XInternAtom(d,cm,False));
    printf("compositor owner of %s = 0x%lx %s\n", cm, owner, owner==None?"(NONE -> no compositor)":"");
    Window r,p,*kids=NULL; unsigned nk=0;
    if(XQueryTree(d,root,&r,&p,&kids,&nk)){
        printf("top-level windows: %u\n", nk);
        for(unsigned i=0;i<nk;i++){
            XWindowAttributes a; if(!XGetWindowAttributes(d,kids[i],&a)) continue;
            char *nm=NULL; XFetchName(d,kids[i],&nm);
            XClassHint ch={0}; XGetClassHint(d,kids[i],&ch);
            printf(" - 0x%lx %dx%d+%d+%d depth=%d border=%d map=%d ovr=%d visual=0x%lx class=%s/%s name=%s\n",
                kids[i], a.width,a.height,a.x,a.y,a.depth,a.border_width,a.map_state,a.override_redirect,
                a.visual?a.visual->visualid:0, ch.res_class?ch.res_class:"-", ch.res_name?ch.res_name:"-", nm?nm:"-");
            if(ch.res_class && (strcasestr(ch.res_class,"brave")||strcasestr(ch.res_class,"chrom"))) {
                prop_str(kids[i],"_NET_WM_WINDOW_TYPE");
                prop_str(kids[i],"_NET_WM_STATE");
                prop_str(kids[i],"_NET_WM_WINDOW_OPACITY");
                prop_str(kids[i],"_NET_WM_BYPASS_COMPOSITOR");
                prop_str(kids[i],"_NET_WM_PID");
            }
            if(nm) XFree(nm); if(ch.res_class)XFree(ch.res_class); if(ch.res_name)XFree(ch.res_name);
        }
        if(kids)XFree(kids);
    }
    return 0;
}
