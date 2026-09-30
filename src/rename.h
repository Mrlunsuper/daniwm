#pragma once

void rename_init(void);      /* self-pipe + SIGUSR1 handler, call once at startup */
int  rename_fd(void);        /* read end for select(), -1 if unavailable */
void rename_poll(void);      /* drain pipe + apply a finished prompt result */
void rename_tick(void);      /* 1s watchdog: clear pending after a dead child */
void k_wsrename(int unused); /* action: prompt to rename the current workspace */
