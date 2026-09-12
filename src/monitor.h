#pragma once

void initmons(void);
int  mon_at(int x, int y);
int  mon_by_pointer(void);
void getarea(int m, int *ax, int *ay, int *aw, int *ah);
void on_monitors_changed(void);
void screen_extents(long *x0, long *y0, long *x1, long *y1);
