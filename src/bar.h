#pragma once
#include <stddef.h>

void bar_style(void);
void drawbar(void);
/* workspace names: label + variable-width cells (bar.c) */
const char *ws_name(int i);
void ws_label(int i, char *out, size_t n);
int ws_cell_w(int i);
int ws_block_w(void);
int ws_hit(int x); /* bar-relative x -> ws index, -1 = outside ws block */
