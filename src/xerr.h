#pragma once
#include <X11/Xlib.h>

/* Trapped X errors with logging (T-H3A). Default handler stays
 * xerror_ignore; wrap risky sequences:
 *   trap_errors(dpy); ... risky X calls ...; untrap_errors(dpy);
 * untrap syncs, logs request codes via XGetErrorText, and continues.
 * Nest-safe. No exits. */
void trap_errors(Display *d);
void untrap_errors(Display *d);
