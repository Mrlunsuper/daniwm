#pragma once
#include <X11/Xlib.h>

/* XEmbed system tray (freedesktop spec): WM owns _NET_SYSTEM_TRAY_S<n>,
 * icons send SYSTEM_TRAY_REQUEST_DOCK, we reparent them into the bar. */

void tray_init(void);
void tray_enable(int on);
int  tray_active_now(void);

int  tray_has(Window w);
int  tray_is_icon_window(Window w); /* _XEMBED_INFO present (pre-dock fallback) */
int  tray_handle_opcode(XClientMessageEvent *e); /* 1 = consumed */
void tray_add(Window icon);
void tray_remove(Window w);
void tray_handle_map(Window w);
void tray_handle_unmap(Window w);
void tray_handle_property(Window w, Atom a);
void tray_handle_resize(Window w);
void tray_handle_selection_clear(Atom sel);

int  tray_width_px(void);
int  tray_box(int *x0, int *x1); /* v2: icon-group bounds, 0 = no icons */
void tray_poll(void); /* v2: re-acquire ownerless selection (1s tick) */
void tray_layout_icons(void);
