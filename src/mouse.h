#pragma once
#include "types.h"

void grabbuttons(Client *c);
void drag_start(Client *c, int mode, int px, int py);
void drag_motion(int px, int py);
void drag_end(int px, int py);
