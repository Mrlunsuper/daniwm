#pragma once
#include <X11/Xlib.h>
int overview_is_open(void);
int overview_committing(void);
int overview_selection(void);
void overview_select(int ws);
void overview_show(int unused);
void overview_hide(int unused);
void overview_toggle(int unused);
int overview_event(XEvent *event);
void overview_capture(Window win);
void overview_drop(Window win);
void overview_top(void);
