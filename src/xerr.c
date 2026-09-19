#include "xerr.h"

#include <stdio.h>

/* Trapped X errors (T-H3A). Stores the first error per trap window,
 * prints request codes after XSync, then restores the ignore handler.
 * Logging happens outside the handler (async-safe). */
static int trap_depth;
static int trapped_err;
static int trapped_req, trapped_minor, trapped_code;
static Display *trap_dpy;
static int (*prev_handler)(Display *, XErrorEvent *);

static int xerror_trapped(Display *d, XErrorEvent *e) {
    (void)d;
    if (!trapped_err) {
        trapped_err = 1;
        trapped_req = (int)e->request_code;
        trapped_minor = (int)e->minor_code;
        trapped_code = (int)e->error_code;
    }
    return 0;
}

void trap_errors(Display *d) {
    if (trap_depth++ == 0) {
        trapped_err = 0;
        trapped_req = trapped_minor = trapped_code = 0;
        trap_dpy = d;
        prev_handler = XSetErrorHandler(xerror_trapped);
    }
}

void untrap_errors(Display *d) {
    char msg[128];
    if (trap_depth <= 0) return;
    if (--trap_depth > 0) return;
    XSync(d, False);
    if (trapped_err) {
        msg[0] = 0;
        XGetErrorText(d, trapped_code, msg, (int)sizeof(msg) - 1);
        msg[sizeof(msg) - 1] = 0;
        fprintf(stderr, "daniwm: trapped X error req=%d minor=%d code=%d (%s)\n",
            trapped_req, trapped_minor, trapped_code, msg);
    }
    if (prev_handler) XSetErrorHandler(prev_handler);
    prev_handler = NULL;
    trap_dpy = NULL;
}
