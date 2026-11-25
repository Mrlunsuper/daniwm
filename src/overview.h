#pragma once
#include <X11/Xlib.h>
int overview_is_open(void);
int overview_committing(void);
long overview_next_ms(void);
void overview_tick(void);
int overview_selection(void);
void overview_select(int ws);
void overview_show(int unused);
void overview_hide(int unused);
void overview_toggle(int unused);
void expose_toggle(int unused);
int overview_event(XEvent *event);
void overview_capture(Window win);
void overview_drop(Window win);
void overview_top(void);
