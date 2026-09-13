#include "sysmon.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

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
/* Volume: cached string, never blocks. The blocking sample runs only
 * from the 1s tick via sys_vol_update(); drawbar() just reads the cache.
 * First second after startup shows no volume until the first tick fills it.
 * Backend: amixer → wpctl (PipeWire) → pactl (Pulse), first success sticks
 * (vol_backend) so later ticks probe only one tool; total failure backs
 * off 30s. vol_set_cmd() picks the matching setter for keys/bar clicks. */
static int vol_backend = 0; /* 0=unknown, 1=amixer, 2=wpctl, 3=pactl */
const char *sys_vol(void) { /* "40%" / "MUTE" / "" */
    return vol_cache[0] ? vol_cache : "";
}
static int vol_try_amixer(void) {
    /* single amixer invocation: scan "amixer get Master" output in C
     * instead of a 4-process pipeline (grep | head per 2s tick) */
    FILE *p = popen("amixer get Master 2>/dev/null", "r");
    char pct[16] = "", st[16] = "";
    char line[256];
    if (!p) return 0;
    while (fgets(line, sizeof(line), p)) {
        /* percent: first "[NN%]" token on the line */
        char *lb = strchr(line, '[');
        while (lb) {
            char *pc = strchr(lb, '%');
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
            lb = strchr(lb + 1, '[');
        }
    }
    pclose(p);
    if (!pct[0]) return 0;
    char *e = strchr(pct, '%');
    if (e) e[1] = 0;
    if ((st[0] == 'o' && !strstr(st, "on")) || strstr(st, "off"))
        snprintf(vol_cache, sizeof(vol_cache), "MUTE");
    else snprintf(vol_cache, sizeof(vol_cache), "%.10s", pct);
    return 1;
}
static int vol_try_wpctl(void) {
    /* "Volume: 0.75" or "Volume: 0.75 [MUTED]" */
    FILE *p = popen("wpctl get-volume @DEFAULT_AUDIO_SINK@ 2>/dev/null", "r");
    char line[64];
    if (!p) return 0;
    if (!fgets(line, sizeof(line), p)) { pclose(p); return 0; }
    pclose(p);
    float v = -1;
    if (sscanf(line, "Volume: %f", &v) != 1 || v < 0) return 0;
    if (strstr(line, "MUTED")) snprintf(vol_cache, sizeof(vol_cache), "MUTE");
    else snprintf(vol_cache, sizeof(vol_cache), "%d%%", (int)(v * 100 + 0.5f));
    return 1;
}
static int vol_try_pactl(void) {
    FILE *p = popen("pactl get-sink-volume @DEFAULT_SINK@ 2>/dev/null", "r");
    char line[256];
    int pct = -1;
    if (!p) return 0;
    while (fgets(line, sizeof(line), p)) {
        char *pc = strchr(line, '%');
        if (pc) { char *q = pc; while (q > line && isdigit((unsigned char)q[-1])) q--; pct = atoi(q); break; }
    }
    pclose(p);
    if (pct < 0) return 0;
    p = popen("pactl get-sink-mute @DEFAULT_SINK@ 2>/dev/null", "r");
    if (p) {
        if (fgets(line, sizeof(line), p) && strstr(line, "yes")) pct = -2;
        pclose(p);
    }
    if (pct == -2) snprintf(vol_cache, sizeof(vol_cache), "MUTE");
    else snprintf(vol_cache, sizeof(vol_cache), "%d%%", pct);
    return 1;
}
void sys_vol_update(void) {
    time_t now = time(NULL);
    int ok = 0;
    if (now < vol_ts) return;
    /* preferred backend first, then the rest */
    if (vol_backend == 2) ok = vol_try_wpctl() ? 2 : 0;
    else if (vol_backend == 3) ok = vol_try_pactl() ? 3 : 0;
    else if (vol_backend == 1) ok = vol_try_amixer() ? 1 : 0;
    if (!ok) {
        if (vol_backend != 1 && vol_try_amixer()) ok = 1;
        else if (vol_backend != 2 && vol_try_wpctl()) ok = 2;
        else if (vol_backend != 3 && vol_try_pactl()) ok = 3;
    }
    if (ok) { vol_backend = ok; vol_ts = now + 2; }
    else { vol_cache[0] = 0; vol_backend = 0; vol_ts = now + 30; }
}
char **vol_set_cmd(char **am, char **wp, char **pa) {
    return vol_backend == 2 ? wp : vol_backend == 3 ? pa : am;
}
