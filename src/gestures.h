#pragma once
#include <X11/Xlib.h>

void gestures_init(int major, int minor);
void gestures_reset(const char *reason);
void gestures_configure(void);
/* Cookie data is acquired/freed by the caller. */
void gestures_event(XGenericEventCookie *cookie);
