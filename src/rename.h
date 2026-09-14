#pragma once

void rename_init(void);      /* self-pipe + SIGUSR1 handler, call once at startup */
int  rename_fd(void);        /* read end for select(), -1 if unavailable */
void rename_poll(void);      /* drain pipe + apply a finished prompt result */
void k_wsrename(int unused); /* action: prompt to rename the current workspace */
