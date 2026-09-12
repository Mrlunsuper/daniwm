#pragma once
#include "state.h"

typedef struct { const char *name; void (*fn)(int); } KeyAction;

extern const KeyAction actions[];
extern const unsigned nactions;

void grabkeys(void);
void add_default_keys(void);
void keys_reset(void);
void push_key_fn(KeySym ks, unsigned int mod, void (*fn)(int), int arg);
void k_reload(int);
