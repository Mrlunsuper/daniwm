#pragma once
/* Feature macros for strict ISO C (-std=c11 -pedantic-errors):
 * expose POSIX.1-2008 declarations (getline, setenv, fork, popen,
 * strcasecmp, wordexp) that strict C hides. Must precede any libc header. */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#include <X11/Xlib.h>
#include <X11/Xft/Xft.h>

typedef struct { char *cls; char *title; int floating; int ws; } Rule;

typedef struct Client Client;
struct Client {
    Window win;
    int ws;       /* workspace index 0..NWS-1 */
    int floating;
    int fullscreen; /* EWMH fullscreen: fill area, no gaps/border */
    int hidden;     /* monocle: unmapped but managed (_NET_WM_STATE_HIDDEN) */
    int urgent;     /* demands attention: EWMH or WM_HINTS urgency */
    int mon;        /* monitor index */
    int fx, fy, fw, fh; /* saved floating geometry */
    float cfact; /* tiled height weight, 0.25..4.0, 1.0 = equal */
    Client *next;
};

typedef enum { L_TILE = 0, L_MONOCLE = 1 } Layout;

typedef struct { XftColor bg, ws_act, ws_acttx, ws_occ, ws_emp, mode, title, sys, urgent, sep, task_act, task_acttx, task_text; } BarColors;

typedef struct { int left, right, top, bottom; } StrutMargin;

typedef struct Dock Dock;
struct Dock {
    Window win;
    int has_strut;
    unsigned long strut[12];
    Dock *next;
};

typedef struct { Window win; int mode; int px, py, x, y, w, h, promoted, tiled0, mon0; float mfact0, cfact0; Window nb_up, nb_dn; float nb_up0, nb_dn0; Window swap_target; } Drag;

typedef struct TrayIcon TrayIcon;
struct TrayIcon {
    Window win;
    int mapped; /* _XEMBED_INFO MAPPED flag */
    TrayIcon *next;
};

typedef struct { KeySym keysym; unsigned int mod; void (*fn)(int); int arg; } Key;
