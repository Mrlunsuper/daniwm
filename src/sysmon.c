#include "sysmon.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "state.h"

/* ---- sysmon: cpu/mem/bat/vol, no extra deps ---- */
int sys_cpu(void) { /* 0..100, -1 = unknown */
    FILE *f = fopen("/proc/stat", "r");
    if (!f) return -1;
    unsigned long long u, n, s, id, io, ir, so, st;
    if (fscanf(f, "cpu %llu %llu %llu %llu %llu %llu %llu %llu",
        &u, &n, &s, &id, &io, &ir, &so, &st) != 8) { fclose(f); return -1; }
    fclose(f);
    unsigned long long idle = id + io;
    unsigned long long total = u + n + s + id + io + ir + so + st;
    /* guard against tick anomalies / counter reset: unsigned wrap would
     * otherwise produce a bogus 0% or 100% spike */
    if (total < cpu_prev_total || idle < cpu_prev_idle) {
        cpu_prev_total = total; cpu_prev_idle = idle;
        return -1;
    }
    unsigned long long dt = total - cpu_prev_total, di = idle - cpu_prev_idle;
    cpu_prev_total = total; cpu_prev_idle = idle;
    if (dt == 0 || dt < di) return -1;
    return (int)(100 * (dt - di) / dt);
}
int sys_mem(void) { /* used % 0..100, -1 = unknown */
    FILE *f = fopen("/proc/meminfo", "r");
    if (!f) return -1;
    long total = 0, avail = 0;
    char k[64]; long v;
    while (fscanf(f, "%63s %ld", k, &v) == 2) {
        if (!strcmp(k, "MemTotal:")) total = v;
        else if (!strcmp(k, "MemAvailable:")) avail = v;
        if (total && avail) break;
        /* skip rest of line (kB unit) */
        int ch; while ((ch = fgetc(f)) != '\n' && ch != EOF) {}
    }
    fclose(f);
    if (total <= 0) return -1;
    return (int)(100 * (total - avail) / total);
}
int sys_bat(char *chg, size_t n) { /* capacity %, chg="+"/"-"/" " */
    const char *bats[] = { "BAT0", "BAT1", NULL };
    for (int i = 0; bats[i]; i++) {
        char pcap[128], pstat[128];
        snprintf(pcap, sizeof(pcap), "/sys/class/power_supply/%s/capacity", bats[i]);
        snprintf(pstat, sizeof(pstat), "/sys/class/power_supply/%s/status", bats[i]);
        FILE *f = fopen(pcap, "r");
        if (!f) continue;
        int cap = -1;
        if (fscanf(f, "%d", &cap) != 1) { fclose(f); continue; }
        fclose(f);
        char st[32] = "";
        f = fopen(pstat, "r");
        if (f) { if (!fgets(st, sizeof(st), f)) st[0] = 0; fclose(f); }
        if (chg && n) snprintf(chg, n, "%s", strstr(st, "Charg") ? "+" : (strstr(st, "Discharg") ? "-" : ""));
        return cap;
    }
    return -1;
}
/* Volume: cached string, never blocks. Sampling runs as an async child
 * job (pipe+fork, audit-0930 #6): sys_vol_update() starts it from the 1s
 * tick, the main loop select()s on sys_vol_fd() and calls sys_vol_read()
 * to collect output; a job older than VOL_JOB_TIMEOUT is SIGKILLed so a
 * hung backend can never stall the event loop. drawbar() reads the cache.
 * Backend: amixer → wpctl (PipeWire) → pactl (Pulse), first success sticks
 * (vol_backend) so later ticks probe only one tool; total failure backs
 * off 30s. vol_set_cmd() picks the matching setter for keys/bar clicks. */
static int vol_backend = 0; /* 0=unknown, 1=amixer, 2=wpctl, 3=pactl */
const char *sys_vol(void) { /* "40%" / "MUTE" / "" */
    return vol_cache[0] ? vol_cache : "";
}
static int vol_parse_amixer(const char *buf) {
    /* scan "amixer get Master" output in C instead of a grep pipeline */
    char pct[16] = "", st[16] = "";
    for (const char *lb = strchr(buf, '['); lb; lb = strchr(lb + 1, '[')) {
        /* percent: first "[NN%]" token */
        const char *pc = strchr(lb, '%');
        if (pc && pc == lb + 1 + strspn(lb + 1, "0123456789")) {
            size_t nd = (size_t)(pc - (lb + 1));
            if (nd > 0 && nd < sizeof(pct) - 1 && !pct[0]) {
                memcpy(pct, lb + 1, nd);
                pct[nd] = '%'; pct[nd + 1] = 0;
            }
        }
        /* mute state: "[on]" / "[off]" token */
        if (!strncmp(lb, "[on]", 4)) snprintf(st, sizeof(st), "on");
        else if (!strncmp(lb, "[off]", 5)) snprintf(st, sizeof(st), "off");
    }
    if (!pct[0]) return 0;
    if (!strcmp(st, "off")) snprintf(vol_cache, sizeof(vol_cache), "MUTE");
    else snprintf(vol_cache, sizeof(vol_cache), "%.10s", pct);
    return 1;
}
static int vol_parse_wpctl(const char *buf) {
    /* "Volume: 0.75" or "Volume: 0.75 [MUTED]" */
    float v = -1;
    if (sscanf(buf, "Volume: %f", &v) != 1 || v < 0) return 0;
    if (strstr(buf, "MUTED")) snprintf(vol_cache, sizeof(vol_cache), "MUTE");
    else snprintf(vol_cache, sizeof(vol_cache), "%d%%", (int)(v * 100 + 0.5f));
    return 1;
}
static int vol_parse_pactl(const char *buf) {
    /* get-sink-volume output, then get-sink-mute's "Mute: yes|no" */
    const char *pc = strchr(buf, '%');
    if (!pc) return 0;
    const char *q = pc;
    while (q > buf && isdigit((unsigned char)q[-1])) q--;
    if (q == pc) return 0;
    const char *m = strstr(buf, "Mute:");
    if (m && strstr(m, "yes")) snprintf(vol_cache, sizeof(vol_cache), "MUTE");
    else snprintf(vol_cache, sizeof(vol_cache), "%d%%", atoi(q));
    return 1;
}
static const char *const vol_cmds[4] = { NULL,
    "exec amixer get Master 2>/dev/null",
    "exec wpctl get-volume @DEFAULT_AUDIO_SINK@ 2>/dev/null",
    "pactl get-sink-volume @DEFAULT_SINK@ 2>/dev/null; "
    "pactl get-sink-mute @DEFAULT_SINK@ 2>/dev/null",
};
enum { VOL_JOB_TIMEOUT = 2 };
static int vol_fd = -1;
static pid_t vol_pid = -1;
static time_t vol_started;
static int vol_order[3], vol_try; /* candidate backends, index in progress */
static char vol_buf[2048];
static size_t vol_len;

int sys_vol_fd(void) { return vol_fd; }

static void vol_job_close(int killit) {
    if (killit && vol_pid > 0) kill(-vol_pid, SIGKILL); /* whole group */
    if (vol_fd >= 0) close(vol_fd);
    vol_fd = -1;
    vol_pid = -1; /* SIGCHLD = SIG_IGN: the kernel reaps it */
}
/* start the job for vol_order[vol_try]; 0 = could not spawn */
static int vol_job_start(void) {
    int pfd[2];
    if (pipe(pfd) != 0) return 0;
    pid_t pid = fork();
    if (pid == -1) { close(pfd[0]); close(pfd[1]); return 0; }
    if (pid == 0) {
        if (dpy) close(ConnectionNumber(dpy));
        setpgid(0, 0); /* timeout kill reaches grandchildren too */
        signal(SIGCHLD, SIG_DFL);
        close(pfd[0]);
        dup2(pfd[1], STDOUT_FILENO);
        if (pfd[1] != STDOUT_FILENO) close(pfd[1]);
        execl("/bin/sh", "sh", "-c", vol_cmds[vol_order[vol_try]], (char *)NULL);
        _exit(127);
    }
    setpgid(pid, pid); /* also here: no race with an early timeout kill */
    close(pfd[1]);
    fcntl(pfd[0], F_SETFD, FD_CLOEXEC);
    fcntl(pfd[0], F_SETFL, O_NONBLOCK);
    vol_fd = pfd[0];
    vol_pid = pid;
    vol_started = time(NULL);
    vol_len = 0;
    return 1;
}
/* current candidate failed: try the next one, or back off 30s */
static void vol_job_next(void) {
    while (++vol_try < 3)
        if (vol_job_start()) return;
    vol_cache[0] = 0; vol_backend = 0; vol_ts = time(NULL) + 30;
}
void sys_vol_read(void) {
    if (vol_fd < 0) return;
    for (;;) {
        char tmp[512];
        ssize_t r = read(vol_fd, tmp, sizeof(tmp));
        if (r > 0) {
            size_t room = sizeof(vol_buf) - 1 - vol_len;
            size_t k = (size_t)r < room ? (size_t)r : room; /* drop overflow */
            memcpy(vol_buf + vol_len, tmp, k);
            vol_len += k;
            continue;
        }
        if (r < 0 && errno == EINTR) continue;
        if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return;
        break; /* EOF or hard error: job done */
    }
    vol_job_close(0);
    vol_buf[vol_len] = 0;
    int b = vol_order[vol_try], ok;
    ok = b == 1 ? vol_parse_amixer(vol_buf)
       : b == 2 ? vol_parse_wpctl(vol_buf)
       : vol_parse_pactl(vol_buf);
    if (ok) { vol_backend = b; vol_ts = time(NULL) + 2; }
    else vol_job_next();
}
/* Optimistic display: shift the cached % by delta so the bar tracks the
 * wheel in real time. Never blocks, never spawns; the next 1s tick
 * (sys_vol_update) re-samples the true value and reconciles. */
void sys_vol_adjust(int delta) {
    if (!delta || !vol_cache[0] || !strncmp(vol_cache, "MUTE", 4)) return;
    char *end = NULL;
    long cur = strtol(vol_cache, &end, 10);
    if (end == vol_cache || *end != '%') return;
    long nv = cur + delta;
    if (nv < 0) nv = 0;
    if (nv > 100) nv = 100;
    snprintf(vol_cache, sizeof(vol_cache), "%ld%%", nv);
}
void sys_vol_update(void) {
    time_t now = time(NULL);
    if (vol_fd >= 0) {
        if (now - vol_started < VOL_JOB_TIMEOUT) return;
        vol_job_close(1); /* hung backend */
        vol_job_next();
        return;
    }
    if (now < vol_ts) return;
    /* preferred backend first, then the rest */
    int n = 0;
    if (vol_backend) vol_order[n++] = vol_backend;
    for (int b = 1; b <= 3; b++) if (b != vol_backend) vol_order[n++] = b;
    vol_try = 0;
    if (!vol_job_start()) vol_job_next();
}
char **vol_set_cmd(char **am, char **wp, char **pa) {
    return vol_backend == 2 ? wp : vol_backend == 3 ? pa : am;
}
