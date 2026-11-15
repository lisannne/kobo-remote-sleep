/*
 * remote-sleep (minimal): hold the "previous" button of a Bluetooth page-turn
 * remote for HOLD_MS and the Kobo goes to sleep. For Kobo Remote + Libra Colour.
 *
 * The remote's input device is grabbed so the reader does not see raw events;
 * every short click is passed on as a click when the button is released. That
 * way a long press does not also turn back a page.
 *
 * No settings, no log, nothing on the user-visible drive.
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/file.h>
#include <sys/inotify.h>
#include <sys/ioctl.h>
#include <linux/input.h>

#ifndef HOLD_MS
#define HOLD_MS 1000
#endif
#define QUIET_MS 3000 /* ignore the remote this long after triggering sleep */

static const int PREV_KEYS[] = { KEY_UP, KEY_LEFT, KEY_PAGEUP };

/* The kernel's event record as a 32-bit process sees it (16 bytes). */
struct kev {
    unsigned int   sec, usec;
    unsigned short type, code;
    int            value;
};

static int is_prev(int code)
{
    for (unsigned i = 0; i < sizeof PREV_KEYS / sizeof *PREV_KEYS; i++)
        if (PREV_KEYS[i] == code) return 1;
    return 0;
}

/* ---------------------------------------------------------- long-press logic */

enum action { ACT_NONE, ACT_CLICK, ACT_SLEEP };

struct machine {
    int       held;        /* previous key currently down, 0 if none */
    int       fired;       /* sleep already triggered for this press */
    long long press_time;  /* ms */
    long long quiet_until; /* ms */
};

static enum action fire(struct machine *m, long long now)
{
    m->fired = 1;
    m->quiet_until = now + QUIET_MS;
    return ACT_SLEEP;
}

/* One key event in; ACT_CLICK means "pass a click of this key on". */
static enum action machine_key(struct machine *m, int code, int value, long long now)
{
    if (now < m->quiet_until) {
        if (value == 0 && code == m->held) m->held = 0;
        return ACT_NONE;
    }
    if (value == 2) return ACT_NONE; /* autorepeat */

    if (!is_prev(code)) return value == 0 ? ACT_CLICK : ACT_NONE;

    if (value == 1) {
        m->held = code;
        m->fired = 0;
        m->press_time = now;
        return ACT_NONE;
    }
    if (m->held != code) return ACT_NONE;
    m->held = 0;
    if (m->fired) return ACT_NONE;
    return now - m->press_time >= HOLD_MS ? fire(m, now) : ACT_CLICK;
}

/* Called when the hold timer runs out. */
static enum action machine_tick(struct machine *m, long long now)
{
    if (m->held && !m->fired && now - m->press_time >= HOLD_MS) return fire(m, now);
    return ACT_NONE;
}

/* How long poll() may wait (ms), -1 = forever. */
static int machine_timeout(const struct machine *m, long long now)
{
    if (!m->held || m->fired) return -1;
    long long left = m->press_time + HOLD_MS - now;
    return left < 0 ? 0 : (int)left;
}

#ifndef TEST
/* ------------------------------------------------------------------ device */

_Static_assert(sizeof(struct kev) == 16 && sizeof(long) == 4, "build for 32-bit ARM");

static long long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static int has_key(int fd, int code)
{
    unsigned long bits[(KEY_MAX + 32) / 32];
    memset(bits, 0, sizeof bits);
    if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof bits), bits) < 0) return 0;
    return (bits[code / 32] >> (code % 32)) & 1;
}

/* remote = Bluetooth device with a "previous" key; otherwise the built-in power key */
static int find_device(int remote)
{
    DIR *d = opendir("/dev/input");
    if (!d) return -1;
    struct dirent *e;
    int found = -1;
    while (found < 0 && (e = readdir(d))) {
        if (strncmp(e->d_name, "event", 5)) continue;
        char path[300];
        snprintf(path, sizeof path, "/dev/input/%s", e->d_name);
        int fd = open(path, O_RDWR | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) continue;

        struct input_id id = { 0 };
        ioctl(fd, EVIOCGID, &id);
        int bt = id.bustype == BUS_BLUETOOTH, ok;
        if (remote) {
            ok = 0;
            for (unsigned i = 0; bt && i < sizeof PREV_KEYS / sizeof *PREV_KEYS; i++)
                ok |= has_key(fd, PREV_KEYS[i]);
        } else {
            ok = !bt && has_key(fd, KEY_POWER);
        }
        if (ok) found = fd; else close(fd);
    }
    closedir(d);
    return found;
}

static int write_key(int fd, int code, int value)
{
    struct kev ev[2] = { { 0, 0, EV_KEY, code, value }, { 0, 0, EV_SYN, SYN_REPORT, 0 } };
    return write(fd, ev, sizeof ev) == (ssize_t)sizeof ev ? 0 : -1;
}

/* A short press of the Kobo's own power button is what makes the reader sleep. */
static void do_sleep(void)
{
    int fd = find_device(0);
    if (fd < 0) return;
    write_key(fd, KEY_POWER, 1);
    usleep(120 * 1000);
    write_key(fd, KEY_POWER, 0);
    close(fd);
}

/*
 * Pass a click on to the reader: release the grab for a moment, inject
 * press + release into the remote's own device, grab again. The injected
 * events also land in our own queue (before write() returns), so drop them.
 */
static void forward_click(int fd, int code, struct machine *m)
{
    ioctl(fd, EVIOCGRAB, (void *)0);
    int ok = write_key(fd, code, 1) == 0 && write_key(fd, code, 0) == 0;
    ioctl(fd, EVIOCGRAB, (void *)1);
    if (!ok) return;

    int echo_down = 1, echo_up = 1;
    struct kev ev;
    while (read(fd, &ev, sizeof ev) == (ssize_t)sizeof ev) {
        if (ev.type != EV_KEY) continue;
        if (ev.code == code && ev.value == 1 && echo_down) { echo_down = 0; continue; }
        if (ev.code == code && ev.value == 0 && echo_up)   { echo_up = 0;   continue; }
        machine_key(m, ev.code, ev.value, now_ms()); /* real event in between: track only */
    }
}

int main(int argc, char **argv)
{
    if (!(argc > 1 && !strcmp(argv[1], "-f"))) { /* go to the background */
        if (fork() > 0) _exit(0);
        setsid();
        if (fork() > 0) _exit(0);
        int n = open("/dev/null", O_RDWR);
        if (n >= 0) { dup2(n, 0); dup2(n, 1); dup2(n, 2); if (n > 2) close(n); }
    }

    int lock = open("/tmp/remote-sleep.lock", O_RDWR | O_CREAT | O_CLOEXEC, 0644);
    if (lock >= 0 && flock(lock, LOCK_EX | LOCK_NB) < 0) return 0; /* already running */

    /* the remote's device comes and goes with the Bluetooth connection */
    int ino = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (ino >= 0 && inotify_add_watch(ino, "/dev/input", IN_CREATE | IN_ATTRIB) < 0) {
        close(ino);
        ino = -1;
    }

    int dev = -1, rescan = 1;
    struct machine m = { 0 };

    for (;;) {
        if (dev < 0 && rescan) {
            rescan = 0;
            dev = find_device(1);
            if (dev >= 0) {
                memset(&m, 0, sizeof m);
                ioctl(dev, EVIOCGRAB, (void *)1);
            }
        }

        struct pollfd p[2] = { { ino, POLLIN, 0 }, { dev, POLLIN, 0 } };
        int timeout = dev >= 0 ? machine_timeout(&m, now_ms()) : (ino >= 0 ? -1 : 5000);
        int r = poll(p, 2, timeout);
        if (r < 0) { if (errno != EINTR) sleep(1); continue; }

        if (ino < 0 && dev < 0) rescan = 1;
        if (p[0].revents) {
            char buf[1024];
            while (read(ino, buf, sizeof buf) > 0) {}
            if (dev < 0) { usleep(700 * 1000); rescan = 1; } /* let the new device settle */
        }

        if (dev >= 0 && p[1].revents) {
            struct kev ev;
            ssize_t n;
            while ((n = read(dev, &ev, sizeof ev)) == (ssize_t)sizeof ev) {
                if (ev.type != EV_KEY) continue;
                enum action a = machine_key(&m, ev.code, ev.value, now_ms());
                if (a == ACT_CLICK) forward_click(dev, ev.code, &m);
                else if (a == ACT_SLEEP) do_sleep();
            }
            if (n == 0 || (n < 0 && errno != EAGAIN && errno != EINTR) || (p[1].revents & (POLLHUP | POLLERR | POLLNVAL))) {
                close(dev);
                dev = -1;
                rescan = 1;
                continue;
            }
        }

        if (dev >= 0 && machine_tick(&m, now_ms()) == ACT_SLEEP) do_sleep();
    }
}
#endif
