#pragma once
#include <stddef.h>

int sys_cpu(void);
int sys_mem(void);
int sys_bat(char *chg, size_t n);
const char *sys_vol(void);
void sys_vol_update(void);
char **vol_set_cmd(char **am, char **wp, char **pa);
