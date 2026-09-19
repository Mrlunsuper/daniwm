#pragma once
#include "types.h"

void ewmh_init(void);
void ewmh_set_wm_state(Client *c, long state);
int  ewmh_hasstate(Window w, Atom state);
void ewmh_update_state(Client *c);
void set_urgent(Client *c, int urg);
int  ws_has_urgent(int n);
int  ewmh_isfloating_type(Window w);
void ewmh_client_list(void);
void ewmh_active(void);
void ewmh_set_wm_desktop(Client *c);
void ewmh_desktops(void);
int  ewmh_read_desktop(Window w);
void setfullscreen(Client *c, int fs);
Dock *find_dock(Window w);
int  ewmh_isdock(Window w);
void update_struts(void);
void manage_dock(Window w);
void unmanage_dock(Window w);
void update_dock_strut(Window w);
