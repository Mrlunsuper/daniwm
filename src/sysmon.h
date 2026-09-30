#pragma once
#include <stddef.h>

int sys_cpu(void);
int sys_mem(void);
int sys_bat(char *chg, size_t n);
const char *sys_vol(void);
void sys_vol_update(void);   /* 1s tick: start/timeout the async sample job */
int sys_vol_fd(void);        /* job pipe to select() on, -1 = idle */
void sys_vol_read(void);     /* job pipe readable: collect + parse */
void sys_vol_adjust(int delta); /* optimistic bar update after a wheel batch */
char **vol_set_cmd(char **am, char **wp, char **pa);
