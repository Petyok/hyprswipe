/*
 * mousegest -- hold LMB+RMB and drag horizontally to swipe Hyprland workspaces,
 * 1:1 with the touchpad's `gesture = 3, horizontal, workspace`.
 *
 * It reads the gaming mouse's evdev stream, passes everything through unchanged,
 * and while LMB+RMB are held it swallows the buttons/motion and instead drives a
 * synthetic 3-finger swipe on a virtual touchpad (see vtouchpad.c) so libinput +
 * Hyprland do the actual, pixel-proportional workspace movement.
 *
 * Two modes:
 *   (default) interception plugin:  reads input_event from stdin, writes to
 *       stdout. Drop into an Interception Tools pipe:
 *           intercept -g $DEVNODE | mousegest | uinput -d $DEVNODE
 *   --grab /dev/input/eventN:  grabs the device itself and re-emits a clone via
 *       uinput (standalone, for testing without udevmon).
 *
 * Click swallowing: a button press is held back for a short window (--window ms).
 * If the other button arrives within the window -> it's a gesture chord, both
 * presses are discarded. Otherwise the press is flushed (a normal click/hold),
 * delayed by at most the window. This is the only way to retract a press that
 * turns out to be the start of a chord.
 */
#define _GNU_SOURCE
#include <libevdev/libevdev.h>
#include <libevdev/libevdev-uinput.h>
#include <linux/input.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/epoll.h>
#include <sys/timerfd.h>

#include "vtouchpad.h"

/* ----- config (overridable via CLI) ----- */
static double  g_sens      = 1.0;   /* mouse REL unit -> touchpad ABS units */
static long    g_window_ms = 35;    /* click-swallow / chord-detect window  */
static int     g_modkey    = KEY_LEFTMETA; /* held + chord = move-window mode */
static double  g_move_step = 500;  /* mouse units per workspace when moving */
static int     g_move_invert = 0;   /* flip move-window direction           */
static long    g_move_min_ms = 120; /* min gap between workspace jumps (debounce) */

static volatile sig_atomic_t g_mod_down = 0;   /* mainMod key currently held */

/* ----- runtime state ----- */
enum mode { NORMAL, PENDING, GESTURE, DRAIN, WINDOW_MOVE };

struct state {
    enum mode mode;
    int phys_left, phys_right;        /* physical button state (truth)        */
    struct vtouchpad tp;

    /* PENDING: events held back since a lone button press */
    struct input_event buf[64];
    int nbuf;
    int timerfd;                      /* one-shot timer arming the PENDING window */

    double move_acc;                  /* WINDOW_MOVE: accumulated horizontal  */
    long   last_move_ms;              /* WINDOW_MOVE: time of last jump        */

    /* output sink */
    struct libevdev_uinput *mirror;   /* non-NULL in --grab mode; else stdout */
};

static volatile sig_atomic_t g_stop = 0;
static void on_signal(int s) { (void)s; g_stop = 1; }

static long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

/* one-shot arm / disarm of the PENDING chord-detect window */
static void timer_arm(struct state *st, long ms)
{
    struct itimerspec its = {0};
    its.it_value.tv_sec  = ms / 1000;
    its.it_value.tv_nsec = (ms % 1000) * 1000000L;
    timerfd_settime(st->timerfd, 0, &its, NULL);
}

static void timer_disarm(struct state *st)
{
    struct itimerspec its = {0};   /* zero it_value -> disarm */
    timerfd_settime(st->timerfd, 0, &its, NULL);
}

/* ---- output a single event downstream (passthrough) ---- */
static void emit(struct state *st, const struct input_event *ev)
{
    if (st->mirror) {
        libevdev_uinput_write_event(st->mirror, ev->type, ev->code, ev->value);
    } else {
        if (fwrite(ev, sizeof(*ev), 1, stdout) != 1) { g_stop = 1; return; }
        fflush(stdout);
    }
}

/* flush all buffered (pending) events downstream, in order */
static void flush_pending(struct state *st)
{
    for (int i = 0; i < st->nbuf; i++)
        emit(st, &st->buf[i]);
    st->nbuf = 0;
}

static void buf_push(struct state *st, const struct input_event *ev)
{
    if (st->nbuf < (int)(sizeof(st->buf) / sizeof(st->buf[0])))
        st->buf[st->nbuf++] = *ev;
    else { /* overflow: flush what we have and forward live */
        flush_pending(st);
        emit(st, ev);
    }
}

static int is_chord_button(int code)
{
    return code == BTN_LEFT || code == BTN_RIGHT;
}

/* update physical button truth from a key event */
static void track_button(struct state *st, const struct input_event *ev)
{
    if (ev->type != EV_KEY) return;
    if (ev->code == BTN_LEFT)  st->phys_left  = (ev->value != 0);
    if (ev->code == BTN_RIGHT) st->phys_right = (ev->value != 0);
}

static void enter_gesture(struct state *st)
{
    st->mode = GESTURE;
    st->nbuf = 0;                 /* discard the held-back press(es) */
    vtouchpad_begin(&st->tp);
}

/* fire-and-forget: move the active window one workspace over (view follows) */
static void dispatch_move(int dir)
{
    const char *arg = dir > 0 ? "+1" : "-1";
    pid_t pid = fork();
    if (pid == 0) {
        /* child: silence output, exec hyprctl */
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) { dup2(devnull, 1); dup2(devnull, 2); }
        execlp("hyprctl", "hyprctl", "dispatch", "movetoworkspace", arg, (char *)NULL);
        _exit(127);
    }
    /* parent: SIGCHLD is ignored -> auto-reaped, no wait */
}

static void enter_window_move(struct state *st)
{
    st->mode = WINDOW_MOVE;
    st->nbuf = 0;                 /* discard the held-back press(es) */
    st->move_acc = 0;
    st->last_move_ms = 0;
}

static void end_gesture(struct state *st)
{
    vtouchpad_end(&st->tp);
    st->mode = DRAIN;             /* swallow remaining button(s) until released */
}

/* core per-event handler */
static void handle(struct state *st, const struct input_event *ev)
{
    int both_before = st->phys_left && st->phys_right;
    track_button(st, ev);
    int both_now = st->phys_left && st->phys_right;

    switch (st->mode) {
    case NORMAL:
        if (ev->type == EV_KEY && is_chord_button(ev->code) && ev->value == 1) {
            /* a lone button press: hold it back briefly */
            st->mode = PENDING;
            st->nbuf = 0;
            buf_push(st, ev);
            timer_arm(st, g_window_ms);
        } else {
            emit(st, ev);
        }
        break;

    case PENDING:
        if (!both_before && both_now) {
            /* second button arrived in time -> chord. mainMod selects mode. */
            timer_disarm(st);
            if (g_mod_down)
                enter_window_move(st);
            else
                enter_gesture(st);
        } else if (ev->type == EV_KEY && is_chord_button(ev->code) &&
                   ev->value == 0) {
            /* the pending button released within the window -> real quick click */
            timer_disarm(st);
            buf_push(st, ev);
            flush_pending(st);
            st->mode = NORMAL;
        } else {
            buf_push(st, ev);     /* keep order until the window resolves */
        }
        break;

    case GESTURE:
        if (ev->type == EV_REL && ev->code == REL_X) {
            vtouchpad_move(&st->tp, ev->value * g_sens);
        }
        /* swallow everything else (motion Y, buttons, wheel) */
        if (!both_now)
            end_gesture(st);
        break;

    case WINDOW_MOVE:
        if (ev->type == EV_REL && ev->code == REL_X) {
            st->move_acc += g_move_invert ? -ev->value : ev->value;
            /* one deliberate jump per full step, rate-limited; reset the
             * accumulator so a single drag can't machine-gun many switches */
            if (st->move_acc >= g_move_step || st->move_acc <= -g_move_step) {
                long t = now_ms();
                if (t - st->last_move_ms >= g_move_min_ms) {
                    dispatch_move(st->move_acc > 0 ? +1 : -1);
                    st->last_move_ms = t;
                }
                st->move_acc = 0;
            }
        }
        /* swallow everything else */
        if (!both_now)
            st->mode = DRAIN;
        break;

    case DRAIN:
        /* swallow until all chord buttons are up */
        if (!st->phys_left && !st->phys_right)
            st->mode = NORMAL;
        break;
    }
}

/* the PENDING window elapsed with no chord -> it was a real press/hold */
static void on_timer(struct state *st)
{
    uint64_t expirations;
    if (read(st->timerfd, &expirations, sizeof(expirations)) < 0) { /* ignore */ }
    if (st->mode == PENDING) {
        flush_pending(st);
        st->mode = NORMAL;
    }
}

/* epoll tags */
enum { TAG_TIMER = 1, TAG_MOUSE, TAG_STDIN, TAG_KB_BASE = 100 };

/* ============ plugin mode: stdin -> stdout ============ */
static int run_plugin(struct state *st)
{
    setvbuf(stdout, NULL, _IONBF, 0);

    int ep = epoll_create1(EPOLL_CLOEXEC);
    struct epoll_event e = { .events = EPOLLIN };
    e.data.u32 = TAG_STDIN; epoll_ctl(ep, EPOLL_CTL_ADD, STDIN_FILENO, &e);
    e.data.u32 = TAG_TIMER; epoll_ctl(ep, EPOLL_CTL_ADD, st->timerfd, &e);

    struct epoll_event evs[8];
    while (!g_stop) {
        int n = epoll_wait(ep, evs, 8, -1);
        if (n < 0) { if (errno == EINTR) continue; break; }
        for (int i = 0; i < n; i++) {
            if (evs[i].data.u32 == TAG_TIMER) { on_timer(st); continue; }
            /* TAG_STDIN */
            struct input_event ev;
            ssize_t r = read(STDIN_FILENO, &ev, sizeof(ev));
            if (r == 0) { g_stop = 1; break; }            /* EOF */
            if (r < 0) { if (errno == EINTR) continue; g_stop = 1; break; }
            if (r == (ssize_t)sizeof(ev)) handle(st, &ev);
        }
    }
    close(ep);
    return 0;
}

/* Try to open+grab one evdev node. Returns 0 on success, filling the out params. */
static int try_grab_node(const char *path, int *out_fd, struct libevdev **out_dev)
{
    int fd = open(path, O_RDONLY | O_NONBLOCK);
    if (fd < 0) return -errno;
    struct libevdev *dev = NULL;
    if (libevdev_new_from_fd(fd, &dev) < 0) { close(fd); return -EIO; }

    /* must be a pointer, not the keyboard sibling */
    if (!libevdev_has_event_code(dev, EV_REL, REL_X) ||
        !libevdev_has_event_code(dev, EV_KEY, BTN_LEFT)) {
        libevdev_free(dev); close(fd); return -ENODEV;
    }
    /* Combo receivers expose one node with both a pointer and a full keyboard.
     * Grabbing that would swallow typing, so skip anything that can type. */
    if (libevdev_has_event_code(dev, EV_KEY, KEY_A)) {
        libevdev_free(dev); close(fd); return -ENODEV;
    }
    if (libevdev_grab(dev, LIBEVDEV_GRAB) < 0) {
        libevdev_free(dev); close(fd); return -EBUSY;
    }
    *out_fd = fd; *out_dev = dev;
    return 0;
}

/* Scan /sys/class/input and grab the first grabbable pointer. With `match`,
 * only nodes whose name contains that substring are considered; with NULL, any
 * pointer will do (a node already grabbed by interception is skipped). */
static int open_by_match(const char *match, int *out_fd, struct libevdev **out_dev)
{
    for (int i = 0; i < 256; i++) {
        char namep[128], devp[64], nbuf[256];
        snprintf(namep, sizeof(namep), "/sys/class/input/event%d/device/name", i);
        FILE *f = fopen(namep, "r");
        if (!f) continue;
        if (!fgets(nbuf, sizeof(nbuf), f)) { fclose(f); continue; }
        fclose(f);
        nbuf[strcspn(nbuf, "\n")] = 0;
        if (match && !strstr(nbuf, match)) continue;

        snprintf(devp, sizeof(devp), "/dev/input/event%d", i);
        int rc = try_grab_node(devp, out_fd, out_dev);
        if (rc == 0) {
            fprintf(stderr, "mousegest: grabbed %s (%s)\n", devp, nbuf);
            return 0;
        }
        fprintf(stderr, "mousegest: skip %s (%s): %s\n", devp, nbuf,
                rc == -EBUSY ? "already grabbed" : strerror(-rc));
    }
    return -ENODEV;
}

/* Observe (no grab) every keyboard exposing the mainMod key. Returns count. */
static int open_modkey_keyboards(int *fds, int max)
{
    int n = 0;
    for (int i = 0; i < 256 && n < max; i++) {
        char devp[64];
        snprintf(devp, sizeof(devp), "/dev/input/event%d", i);
        int fd = open(devp, O_RDONLY | O_NONBLOCK);
        if (fd < 0) continue;
        struct libevdev *d = NULL;
        if (libevdev_new_from_fd(fd, &d) < 0) { close(fd); continue; }
        int has = libevdev_has_event_code(d, EV_KEY, g_modkey) &&
                  !libevdev_has_event_code(d, EV_REL, REL_X); /* skip the mouse */
        libevdev_free(d);
        if (has) { fds[n++] = fd; }
        else     { close(fd); }
    }
    return n;
}

/* read a keyboard fd and track the mainMod key state */
static void kb_drain(int fd)
{
    struct input_event ev;
    ssize_t r;
    while ((r = read(fd, &ev, sizeof(ev))) == (ssize_t)sizeof(ev)) {
        if (ev.type == EV_KEY && ev.code == g_modkey)
            g_mod_down = (ev.value != 0);
    }
}

/* ============ grab mode: own the device, re-emit via uinput ============ */
/* Acquire the mouse: retry until grabbable or g_stop. Returns 0 on success.
 * The grabbed node (interception's virtual output) can vanish on USB
 * re-enumeration / autosuspend / interception restart, so this is allowed to
 * block-with-backoff rather than give up. */
static int acquire_mouse(const char *path, const char *match,
                         int *out_fd, struct libevdev **out_dev)
{
    int backoff_ms = 200;
    while (!g_stop) {
        int rc;
        if (match) rc = open_by_match(match, out_fd, out_dev);
        else       rc = try_grab_node(path, out_fd, out_dev);
        if (rc == 0) return 0;

        struct timespec ts = { backoff_ms / 1000, (backoff_ms % 1000) * 1000000L };
        nanosleep(&ts, NULL);
        if (backoff_ms < 2000) backoff_ms *= 2;   /* cap at ~2s */
    }
    return -1;
}

/* Run one connected session: epoll the mouse + keyboards + timer until the
 * device is lost (returns 1) or we are asked to stop (returns 0). */
static int session_loop(struct state *st, int fd, struct libevdev *dev)
{
    int kbfds[16];
    int nkb = open_modkey_keyboards(kbfds, 16);

    int ep = epoll_create1(EPOLL_CLOEXEC);
    struct epoll_event e = { .events = EPOLLIN };
    e.data.u32 = TAG_MOUSE; epoll_ctl(ep, EPOLL_CTL_ADD, fd, &e);
    e.data.u32 = TAG_TIMER; epoll_ctl(ep, EPOLL_CTL_ADD, st->timerfd, &e);
    for (int i = 0; i < nkb; i++) {
        e.data.u32 = TAG_KB_BASE + i;
        epoll_ctl(ep, EPOLL_CTL_ADD, kbfds[i], &e);
    }

    int lost = 0;
    struct epoll_event evs[16];
    while (!g_stop && !lost) {
        int n = epoll_wait(ep, evs, 16, -1);
        if (n < 0) { if (errno == EINTR) continue; break; }
        for (int i = 0; i < n; i++) {
            uint32_t tag = evs[i].data.u32;
            if (tag == TAG_TIMER) {
                on_timer(st);
            } else if (tag == TAG_MOUSE) {
                if (evs[i].events & (EPOLLHUP | EPOLLERR)) { lost = 1; break; }
                struct input_event ev;
                int r = libevdev_next_event(dev, LIBEVDEV_READ_FLAG_NORMAL, &ev);
                while (r >= 0) {
                    if (r == LIBEVDEV_READ_STATUS_SUCCESS)
                        handle(st, &ev);
                    /* (SYNC drops ignored: gesture self-heals on next events) */
                    r = libevdev_next_event(dev, LIBEVDEV_READ_FLAG_NORMAL, &ev);
                }
                if (r == -ENODEV) { lost = 1; break; }   /* device removed */
            } else {  /* keyboard */
                kb_drain(kbfds[tag - TAG_KB_BASE]);
            }
        }
    }
    close(ep);
    for (int i = 0; i < nkb; i++) close(kbfds[i]);
    return lost;
}

static int run_grab(struct state *st, const char *path, const char *match)
{
    /* Reconnect loop: survive the grabbed node vanishing and reappearing. */
    while (!g_stop) {
        int fd = -1;
        struct libevdev *dev = NULL;

        if (acquire_mouse(path, match, &fd, &dev) != 0)
            break;                                  /* g_stop while acquiring */

        int rc = libevdev_uinput_create_from_device(dev,
                LIBEVDEV_UINPUT_OPEN_MANAGED, &st->mirror);
        if (rc != 0) {
            fprintf(stderr, "mirror create: %d (retrying)\n", rc);
            libevdev_grab(dev, LIBEVDEV_UNGRAB);
            libevdev_free(dev); close(fd);
            struct timespec ts = { 0, 300 * 1000000L };
            nanosleep(&ts, NULL);
            continue;
        }

        fprintf(stderr, "mousegest: connected, gesture active\n");
        int lost = session_loop(st, fd, dev);

        /* tear down this session's mouse resources */
        if (st->mode != NORMAL) {                   /* drop any half-done gesture */
            if (st->tp.active) vtouchpad_end(&st->tp);
            st->mode = NORMAL;
        }
        libevdev_uinput_destroy(st->mirror); st->mirror = NULL;
        libevdev_grab(dev, LIBEVDEV_UNGRAB);
        libevdev_free(dev);
        close(fd);

        if (lost && !g_stop)
            fprintf(stderr, "mousegest: device lost, reconnecting...\n");
    }
    return 0;
}

static void usage(const char *p)
{
    fprintf(stderr,
        "usage: %s [opts] [--grab /dev/input/eventN | --match NAME]\n"
        "  --sens F        swipe gain (mouse unit -> touchpad unit)\n"
        "  --window MS     chord-detect / click-swallow window (default 35)\n"
        "  --match NAME    auto-pick first grabbable mouse whose name contains NAME\n"
        "  --mod-key CODE  mainMod evdev keycode for move-window mode (default %d)\n"
        "  --move-step F   mouse units (counts) per workspace in move-window mode (default 1500)\n"
        "  --move-min MS   min gap between workspace jumps, debounce (default 120)\n"
        "  --move-invert   flip move-window direction\n"
        "  (no --grab/--match: interception plugin mode, stdin->stdout)\n",
        p, KEY_LEFTMETA);
}

int main(int argc, char **argv)
{
    const char *grab_path = NULL;
    const char *match = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--sens") && i + 1 < argc)        g_sens = atof(argv[++i]);
        else if (!strcmp(argv[i], "--window") && i + 1 < argc) g_window_ms = atol(argv[++i]);
        else if (!strcmp(argv[i], "--grab") && i + 1 < argc)   grab_path = argv[++i];
        else if (!strcmp(argv[i], "--match") && i + 1 < argc)  match = argv[++i];
        else if (!strcmp(argv[i], "--mod-key") && i + 1 < argc) g_modkey = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--move-step") && i + 1 < argc) g_move_step = atof(argv[++i]);
        else if (!strcmp(argv[i], "--move-min") && i + 1 < argc) g_move_min_ms = atol(argv[++i]);
        else if (!strcmp(argv[i], "--move-invert"))            g_move_invert = 1;
        else { usage(argv[0]); return 2; }
    }

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGHUP, on_signal);
    signal(SIGPIPE, on_signal);
    signal(SIGCHLD, SIG_IGN);   /* auto-reap hyprctl dispatch children */

    struct state st;
    memset(&st, 0, sizeof(st));
    st.mode = NORMAL;
    st.timerfd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    if (st.timerfd < 0) { perror("timerfd_create"); return 1; }

    int rc = vtouchpad_create(&st.tp);
    if (rc != 0) {
        fprintf(stderr, "vtouchpad_create failed (%d): check /dev/uinput access\n", rc);
        return 1;
    }
    /* give libinput/Hyprland a moment to enumerate the touchpad */
    struct timespec ts = { 0, 300 * 1000000L };
    nanosleep(&ts, NULL);

    int ret;
    /* Interception Tools pipes us stdin/stdout, so a non-tty stdin means plugin
     * mode. Run interactively (or from exec-once) and we pick a mouse ourselves;
     * --match narrows the scan, --grab skips it entirely. */
    if (grab_path || match || isatty(STDIN_FILENO))
        ret = run_grab(&st, grab_path, match);
    else
        ret = run_plugin(&st);

    if (st.tp.active) vtouchpad_end(&st.tp);
    vtouchpad_destroy(&st.tp);
    close(st.timerfd);
    return ret;
}
