/* Phase 0: instrument the real compositor loop only in this test binary.
 * Two deadline wakeups bound measurement; they are reported separately.
 * No debug mode, synthetic Present events, WM, session services or live DISPLAY. */
#define _POSIX_C_SOURCE 200809L
#include <X11/Xlib.h>
#include <sys/select.h>
static int measured_select(int, fd_set *, fd_set *, fd_set *, struct timeval *);
static int measured_flush(Display *);
#define select measured_select
#define XFlush measured_flush
#define main comp_main
/* glibc may upgrade this macro when _DEFAULT_SOURCE is enabled. */
#undef _POSIX_C_SOURCE
#include "../src/comp.c"
#undef main
#undef XFlush
#undef select
#include <assert.h>

static long long started, settled_at, end_at;
static unsigned deadline_wakes, event_wakes, timer_wakes, idle_event_wakes, idle_timer_wakes;
static long idle_paints;
static int settled;
static int cancel_mode, cancelled;
static int close_mode, close_action;
static Window close_window;
static unsigned peak_exits;
static int remap_mode, remapped;
static uint64_t first_generation;
static int burst_mode;
static Window burst_windows[32];
static unsigned burst_index;
static long long cancel_at;
static Display *settings_display;
static Window mock_wm;
static unsigned long mock_values[5];
static struct timespec idle_cpu;
static long long frames[512];
static unsigned frame_count;

static long long microseconds(void) {
    struct timespec ts;
    assert(clock_gettime(CLOCK_MONOTONIC, &ts) == 0);
    return (long long)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

static int measured_flush(Display *display) {
    if (back_pict && !quit_req) {
        assert(frame_count < sizeof(frames) / sizeof(frames[0]));
        frames[frame_count++] = microseconds();
    }
    return XFlush(display);
}

static void sample_boundary(void) {
    long long now = now_ms();
    if (exits.count > peak_exits) peak_exits = exits.count;
    if (burst_mode && burst_index < 32 && now >= started + 220 + burst_index * 3) {
        XDestroyWindow(settings_display, burst_windows[burst_index++]);
        XSync(settings_display, False);
    }
    if (close_mode && !burst_mode && !close_action && now >= started + 220) {
        Win *w = win_get(close_window); assert(w);
        first_generation = w->generation;
        if (remap_mode) XUnmapWindow(settings_display, close_window);
        else XDestroyWindow(settings_display, close_window);
        XSync(settings_display, False);
        close_action = 1;
    }
    if (remap_mode && close_action && !remapped && now >= started + 350) {
        assert(exits.count == 1);
        XMapWindow(settings_display, close_window); XSync(settings_display, False);
        remapped = 1;
    }
    if (cancel_mode && !cancelled && now >= cancel_at) {
        mock_values[3] = 1;
        XChangeProperty(settings_display, DefaultRootWindow(settings_display),
            XInternAtom(settings_display, ANIMATION_SETTINGS_ATOM, False), XA_CARDINAL,
            32, PropModeReplace, (unsigned char *)mock_values, 5);
        XSync(settings_display, False);
        cancelled = 1;
    }
    if (!settled && now >= settled_at) {
        settled = 1;
        idle_paints = n_paint;
        assert(clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &idle_cpu) == 0);
    }
    if (now >= end_at) quit_req = 1;
}

static int measured_select(int nfds, fd_set *readfds, fd_set *writefds,
                           fd_set *exceptfds, struct timeval *timeout) {
    sample_boundary();
    if (quit_req) return 0;
    long long due = settled ? end_at : settled_at;
    if (cancel_mode && !cancelled && cancel_at < due) due = cancel_at;
    if (burst_mode && burst_index < 32 && started + 220 + burst_index * 3 < due)
        due = started + 220 + burst_index * 3;
    if (close_mode && !burst_mode && !close_action && started + 220 < due) due = started + 220;
    if (remap_mode && close_action && !remapped && started + 350 < due) due = started + 350;
    long long remaining = (due - now_ms()) * 1000;
    if (remaining < 0) remaining = 0;
    long long normal = timeout ? (long long)timeout->tv_sec * 1000000 + timeout->tv_usec : -1;
    int bounded = normal < 0 || remaining < normal;
    struct timeval limit = { (time_t)(remaining / 1000000), (suseconds_t)(remaining % 1000000) };
    int ret = select(nfds, readfds, writefds, exceptfds, bounded ? &limit : timeout);
    if (ret > 0) { event_wakes++; if (settled) idle_event_wakes++; }
    else if (ret == 0 && bounded) deadline_wakes++;
    else if (ret == 0) { timer_wakes++; if (settled) idle_timer_wakes++; }
    sample_boundary();
    return ret;
}

int main(int argc, char **argv) {
    if (argc != 4 || argv[1][0] != ':' ||
        (strcmp(argv[2], "fade") && strcmp(argv[2], "instant") &&
         strcmp(argv[2], "zero") && strcmp(argv[2], "reduced") && strcmp(argv[2], "cancel") &&
         strcmp(argv[2], "close") && strcmp(argv[2], "close-cancel") && strcmp(argv[2], "remap") && strcmp(argv[2], "burst")) ||
        (strcmp(argv[3], "copy") && strcmp(argv[3], "present"))) {
        fprintf(stderr, "usage: comp-animation-loop :PRIVATE fade|instant|zero|reduced|cancel copy|present\n");
        return 2;
    }
    unsetenv("DANI_COMP_DEBUG");
    unsetenv("DANI_COMP_BARONLY");
    if (!strcmp(argv[3], "copy")) setenv("DANI_COMP_NO_PRESENT", "1", 1);
    else unsetenv("DANI_COMP_NO_PRESENT");
    Display *owner = XOpenDisplay(argv[1]);
    assert(owner);
    Window window = XCreateSimpleWindow(owner, DefaultRootWindow(owner),
        40, 40, 200, 120, 0, 0, 0x336699);
    XMapWindow(owner, window);
    XSync(owner, False);
    cancel_mode = !strcmp(argv[2], "cancel") || !strcmp(argv[2], "close-cancel");
    remap_mode = !strcmp(argv[2], "remap");
    burst_mode = !strcmp(argv[2], "burst");
    close_mode = !strcmp(argv[2], "close") || !strcmp(argv[2], "close-cancel") || remap_mode || burst_mode;
    if (burst_mode) {
        burst_windows[0] = window;
        for (unsigned i = 1; i < 32; i++) {
            burst_windows[i] = XCreateSimpleWindow(owner, DefaultRootWindow(owner),
                40, 40, 200, 120, 0, 0, 0x336699);
            XMapWindow(owner, burst_windows[i]);
        }
        XSync(owner, False);
    }
    close_window = window;
    settings_display = owner;
    if (cancel_mode) {
        Atom check = XInternAtom(owner, "_NET_SUPPORTING_WM_CHECK", False);
        mock_wm = XCreateSimpleWindow(owner, DefaultRootWindow(owner), 0, 0, 1, 1, 0, 0, 0);
        XChangeProperty(owner, mock_wm, check, XA_WINDOW, 32, PropModeReplace, (unsigned char *)&mock_wm, 1);
        XChangeProperty(owner, DefaultRootWindow(owner), check, XA_WINDOW, 32, PropModeReplace, (unsigned char *)&mock_wm, 1);
        mock_values[0] = 1; mock_values[1] = mock_wm; mock_values[2] = 1; mock_values[4] = 1000;
        XChangeProperty(owner, DefaultRootWindow(owner), XInternAtom(owner, ANIMATION_SETTINGS_ATOM, False),
            XA_CARDINAL, 32, PropModeReplace, (unsigned char *)mock_values, 5);
        if (close_mode) {
            unsigned long closing[] = {1, mock_wm, 1000};
            XChangeProperty(owner, DefaultRootWindow(owner), XInternAtom(owner, ANIMATION_CLOSE_ATOM, False),
                XA_CARDINAL, 32, PropModeReplace, (unsigned char *)closing, 3);
        }
        XSync(owner, False);
    }
    char *args[] = { "comp-loop-test", "-d", argv[1], "--no-shadow", "--dim", "1",
        !strcmp(argv[2], "instant") ? "--no-fade" : "--fade", "--fade-ms",
        !strcmp(argv[2], "zero") ? "0" : "160",
        cancel_mode ? "--wm-settings" : !strcmp(argv[2], "reduced") ? "--reduce-motion" : "-v",
        "--close-ms", "1000", NULL };
    if (close_mode) args[8] = "0";
    started = now_ms(); cancel_at = started + (close_mode ? 400 : 80);
    settled_at = started + (close_mode ? 1600 : 800); end_at = settled_at + 1500;
    int result = comp_main(12, args);
    assert(result == 0 && settled);
    struct timespec cpu;
    assert(clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &cpu) == 0);
    double cpu_ms = (cpu.tv_sec - idle_cpu.tv_sec) * 1000.0 +
        (cpu.tv_nsec - idle_cpu.tv_nsec) / 1000000.0;
    assert(n_paint == idle_paints && !fading);
    assert(!idle_timer_wakes && !idle_event_wakes);
    if (cancel_mode) assert(cancelled && opt_reduce_motion && (close_mode || n_paint < 50));
    if (close_mode && !burst_mode) { assert(close_action && peak_exits == 1 && !exits.count && !exits.bytes); }
    if (burst_mode) assert(burst_index == 32 && peak_exits > 0 && peak_exits <= EXIT_COUNT &&
        !exits.count && !exits.bytes);
    if (remap_mode) {
        Win *w = win_get(close_window);
        assert(remapped && w && w->mapped && w->generation > first_generation);
    }
    printf("METRIC: peak_detached=%u final_detached=%u bytes=%zu\n", peak_exits, exits.count, exits.bytes);
    assert(frame_count > 0);
    long long intervals[511];
    unsigned n = frame_count - 1;
    for (unsigned i = 0; i < n; i++) intervals[i] = frames[i + 1] - frames[i];
    for (unsigned i = 0; i < n; i++) for (unsigned j = i + 1; j < n; j++)
        if (intervals[j] < intervals[i]) {
            long long temp = intervals[i]; intervals[i] = intervals[j]; intervals[j] = temp;
        }
    printf("METRIC: real-loop mode=%s requested_backend=%s present_final=%d paints=%ld "
           "event_wakes=%u timer_wakes=%u measurement_deadline_wakes=%u "
           "settled_ms=1500 settled_paints=%ld settled_events=%u settled_timers=%u cpu_ms=%.3f\n",
           argv[2], argv[3], present_ok, n_paint, event_wakes, timer_wakes, deadline_wakes,
           n_paint - idle_paints, idle_event_wakes, idle_timer_wakes, cpu_ms);
    if (n) printf("METRIC: paint-submit intervals n=%u min_us=%lld p50_us=%lld p95_us=%lld max_us=%lld "
                  "(includes startup/shape rechecks; not scanout latency)\n",
                  n, intervals[0], intervals[(n - 1) / 2], intervals[(n - 1) * 95 / 100], intervals[n - 1]);
    if (!strcmp(argv[3], "present") && !present_ok)
        puts("SKIP: real Present pacing unavailable or fell back; reported run is not hardware-vsync evidence");
    puts("PASS: real compositor loop settles without animation-driven paint");
    XCloseDisplay(owner);
    return 0;
}
