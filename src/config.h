#pragma once

void load_config(const char *path);
void config_defaults(void);
void finalize_nws(void);
void ws_set_name(int idx, const char *val); /* NULL/empty clears -> number */
