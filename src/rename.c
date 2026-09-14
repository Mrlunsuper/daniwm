#include "rename.h"

#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "bar.h"
#include "config.h"
#include "ewmh.h"
#include "state.h"

/* Runtime workspace rename (action `ws_rename`).
 * Flow: fork a child that pipes the current name into `rename_cmd`
 * (rofi -dmenu / dmenu); the child saves stdout to a cache file and
 * wakes the parent with SIGUSR1. The parent never blocks: a self-pipe
 * integrated into the main select() loop carries the wakeup.
 * Cancel (non-zero prompt exit) deletes the file -> old name kept.
 * Empty input clears the name back to the ws number. */
static int rpipe[2] = { -1, -1 };
static int pending_idx = -1;
static char pending_file[256] = "";

static void on_sigusr1(int sig) {
    (void)sig;
    if (rpipe[1] >= 0) {
        char b = 0;
        /* async-signal-safe, atomic (<= PIPE_BUF); drop when full */
        (void)!write(rpipe[1], &b, 1);
    }
}

void rename_init(void) {
    if (pipe(rpipe) != 0) { rpipe[0] = rpipe[1] = -1; return; }
    fcntl(rpipe[0], F_SETFL, O_NONBLOCK);
    fcntl(rpipe[0], F_SETFD, FD_CLOEXEC);
    fcntl(rpipe[1], F_SETFD, FD_CLOEXEC);
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_sigusr1;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART; /* blocking X calls resume; the pipe byte wakes select */
    sigaction(SIGUSR1, &sa, NULL);
}

int rename_fd(void) {
    return rpipe[0];
}

static void rename_path(char *out, size_t n) {
    const char *xdg = getenv("XDG_CACHE_HOME");
    const char *home = getenv("HOME");
    if (xdg && *xdg) snprintf(out, n, "%s/daniwm-ws-rename", xdg);
    else if (home && *home) snprintf(out, n, "%s/.cache/daniwm-ws-rename", home);
    else snprintf(out, n, "/tmp/daniwm-ws-rename-%d", (int)getuid());
}

void k_wsrename(int unused) {
    (void)unused;
    if (pending_idx >= 0) {
        fprintf(stderr, "daniwm: rename already in progress\n");
        return;
    }
    if (!RENAME_CMD || !*RENAME_CMD) {
        fprintf(stderr, "daniwm: rename_cmd is empty\n");
        return;
    }
    char file[256];
    rename_path(file, sizeof(file));
    pending_idx = curws;
    snprintf(pending_file, sizeof(pending_file), "%s", file);
    pid_t pid = fork();
    if (pid == -1) { perror("daniwm: fork rename"); pending_idx = -1; return; }
    if (pid == 0) {
        /* child: detached, no X, no pipe; prompt -> file, then wake parent */
        if (rpipe[0] >= 0) close(rpipe[0]);
        if (rpipe[1] >= 0) close(rpipe[1]);
        if (dpy) close(ConnectionNumber(dpy));
        setsid();
        signal(SIGUSR1, SIG_DFL);
        signal(SIGCHLD, SIG_DFL);
        const char *cur = (pending_idx >= 0 && pending_idx < MAXWS && ws_names[pending_idx])
            ? ws_names[pending_idx] : "";
        char num[16];
        snprintf(num, sizeof(num), "%d", pending_idx + 1);
        setenv("DANIWM_WS_CUR", cur, 1);
        setenv("DANIWM_WS_NUM", num, 1);
        setenv("DANIWM_RENAME_CMD", RENAME_CMD, 1);
        setenv("DANIWM_RENAME_FILE", file, 1);
        execl("/bin/sh", "sh", "-c",
            "mkdir -p \"$(dirname \"$DANIWM_RENAME_FILE\")\" 2>/dev/null; "
            "if printf '%s\\n' \"$DANIWM_WS_CUR\" | eval \"$DANIWM_RENAME_CMD\""
            " > \"$DANIWM_RENAME_FILE\" 2>/dev/null; then :; "
            "else rm -f \"$DANIWM_RENAME_FILE\"; fi; "
            "kill -USR1 $PPID 2>/dev/null",
            NULL);
        _exit(1);
    }
    /* parent: SIGCHLD is SIG_IGN (auto-reap); result arrives via SIGUSR1 */
}

/* trim leading/trailing whitespace in place */
static char *trim_in(char *s) {
    while (*s && (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r')) s++;
    char *e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\n' || e[-1] == '\r')) *--e = 0;
    return s;
}

void rename_poll(void) {
    if (rpipe[0] < 0) return;
    char b;
    while (read(rpipe[0], &b, 1) == 1) { }
    if (pending_idx < 0) return;
    int idx = pending_idx;
    pending_idx = -1;
    FILE *f = fopen(pending_file, "r");
    if (!f) return; /* prompt cancelled: keep the old name */
    char buf[128];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    unlink(pending_file);
    buf[n] = 0;
    buf[strcspn(buf, "\r\n")] = 0; /* first line only */
    char *t = trim_in(buf);
    if (idx < 0 || idx >= NWS) return;
    if (!*t) ws_set_name(idx, NULL); /* blank = clear back to number */
    else ws_set_name(idx, t);
    ewmh_desktops();
    drawbar();
}
