#pragma once

void tile(void);
void tile_mon(int m);
void monocle(void);
void monocle_mon(int m);
void arrange(void);
void arrange_later(void); /* mark dirty; flushed when the event queue drains */
void arrange_flush(void);
