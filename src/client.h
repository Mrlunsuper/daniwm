#pragma once
#include "types.h"

Client *find(Window w);
int     ws_occupied(int n);
Client *first_in_ws(int n);
int     count_tiled(void);
void    attach(Client *c);
void    detach(Client *c);
void    manage(Window w);
void    unmanage(Window w);
void    focus(Client *c);
void    focus_step(int dir);
void    view(int n);
void    send_to(int n);
void    move_to(Client *c, int n);
void    kill_sel(void);
void    kill_client(Client *c);
void    spawn(char **argv);
void    toggle_floating_sel(void);
void    zoom(int n);
void    ws_toggle(int n);
void    keep_docks_on_top(void);
void    quit(void);
