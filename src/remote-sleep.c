/*
 * remote-sleep: put a Kobo to sleep by long-pressing the "previous" button
 * of a Bluetooth page-turn remote (written for Kobo Remote + Libra Colour).
 *
 * How it works
 *  - Waits for the remote's input device to show up in /dev/input (it comes
 *    and goes as Bluetooth connects/disconnects).
 *  - Watches its key events. If the "previous" key stays down for hold_ms,
 *    a short power-button press is injected into the Kobo's own power-key
 *    device, which is exactly what makes Nickel go to sleep.
 *  - In grab mode (default) the remote is grabbed so the reading app does
 *    not see the raw events; short clicks are passed on as a click when the
 *    button is released. That way a long press does not also turn the page.
 *
 * No files are kept open on /mnt/onboard, so USB mounting keeps working.
 */
#define _GNU_SOURCE
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/file.h>
#include <sys/inotify.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <linux/input.h>

/*
 * The kernel's event record as a 32-bit process sees it (16 bytes). Not using
 * struct input_event: with a 64-bit time_t libc it would be 24 bytes on ARM.
 */
struct kev {
    unsigned int   sec, usec;
    unsigned short type, code;
    int            value;
};
#ifndef TEST
_Static_assert(sizeof(struct kev) == 16 && sizeof(long) == 4, "build for 32-bit ARM");
#endif

#define MAX_PREV 8
#define QUIET_AFTER_SLEEP_MS 3000

/* ---------------------------------------------------------------- config */

struct config {
    int  hold_ms;
    int  repeat_gap_ms; /* 0 = off */
    int  prev[MAX_PREV];
    int  nprev;
    int  grab;
    int  debug;
    char device_name[64];
    char sleep_cmd[200];
};

static void config_defaults(struct config *c)
{
    memset(c, 0, sizeof *c);
    c->hold_ms = 1000;
    c->prev[0] = KEY_UP;      /* 103 */
    c->prev[1] = KEY_LEFT;    /* 105 */
    c->prev[2] = KEY_PAGEUP;  /* 104 */
    c->nprev = 3;
    c->grab = 1;
    c->debug = 0;
}

static char *trim(char *s)
{
    while (isspace((unsigned char)*s)) s++;
    char *e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1])) *--e = 0;
    return s;
}

static void config_parse_line(struct config *c, char *line)
{
    char *hash = strchr(line, '#');
    if (hash) *hash = 0;
    char *eq = strchr(line, '=');
    if (!eq) return;
    *eq = 0;
    char *k = trim(line), *v = trim(eq + 1);

    if (!strcmp(k, "hold_ms")) {
        int n = atoi(v);
        if (n >= 300 && n <= 10000) c->hold_ms = n;
    } else if (!strcmp(k, "repeat_gap_ms")) {
        int n = atoi(v);
        if (n >= 0 && n <= 2000) c->repeat_gap_ms = n;
    } else if (!strcmp(k, "prev_key")) {
        int n = 0;
        for (char *t = strtok(v, ", "); t && n < MAX_PREV; t = strtok(NULL, ", ")) {
            int code = atoi(t);
            if (code > 0 && code <= KEY_MAX) c->prev[n++] = code;
        }
        if (n) c->nprev = n;
    } else if (!strcmp(k, "grab")) {
        c->grab = atoi(v) != 0;
    } else if (!strcmp(k, "debug")) {
        c->debug = atoi(v) != 0;
    } else if (!strcmp(k, "device_name")) {
        snprintf(c->device_name, sizeof c->device_name, "%s", v);
    } else if (!strcmp(k, "sleep_cmd")) {
        snprintf(c->sleep_cmd, sizeof c->sleep_cmd, "%s", v);
    }
}

static int is_prev(const struct config *c, int code)
{
    for (int i = 0; i < c->nprev; i++)
        if (c->prev[i] == code) return 1;
    return 0;
}

/* --------------------------------------------------------- state machine */

enum action { ACT_NONE = 0, ACT_CLICK, ACT_SLEEP };

struct machine {
    int       held;         /* prev key currently down, 0 if none */
    int       fired;        /* sleep already triggered for this hold */
    int       count;        /* presses in the current run */
    long long press_time;   /* ms, start of the current press */
    long long run_start;    /* ms, start of the first press of the run */
    long long last_release; /* ms, 0 = none */
    long long quiet_until;  /* swallow everything until then */
};

/*
 * A long press normally shows up as one press that stays down. Some remotes
 * instead send a stream of short clicks while the button is held; with
 * repeat_gap_ms set, clicks that follow each other that closely count as one
 * run, and a run of 3+ clicks lasting hold_ms is treated as a long press.
 */
static int machine_due(const struct machine *m, const struct config *c, long long now)
{
    if (now - m->press_time >= c->hold_ms) return 1;
    return c->repeat_gap_ms > 0 && m->count >= 3 && now - m->run_start >= c->hold_ms;
}

static enum action machine_fire(struct machine *m, long long now)
{
    m->fired = 1;
    m->last_release = 0;
    m->quiet_until = now + QUIET_AFTER_SLEEP_MS;
    return ACT_SLEEP;
}

/* Feed one key event. Returns what to do; *click_code is set for ACT_CLICK. */
static enum action machine_key(struct machine *m, const struct config *c,
                               int code, int value, long long now, int *click_code)
{
    if (now < m->quiet_until) {
        if (value == 0 && code == m->held) m->held = 0;
        return ACT_NONE;
    }
    if (value == 2) return ACT_NONE; /* autorepeat */

    if (is_prev(c, code)) {
        if (value == 1) {
            int cont = c->repeat_gap_ms > 0 && m->last_release &&
                       now - m->last_release <= c->repeat_gap_ms;
            if (cont) {
                m->count++;
            } else {
                m->count = 1;
                m->run_start = now;
            }
            m->held = code;
            m->fired = 0;
            m->press_time = now;
            return machine_due(m, c, now) ? machine_fire(m, now) : ACT_NONE;
        }
        /* release */
        if (m->held != code) return ACT_NONE;
        m->held = 0;
        if (m->fired) return ACT_NONE;
        if (machine_due(m, c, now)) return machine_fire(m, now); /* timer was late */
        m->last_release = now;
        *click_code = code;
        return ACT_CLICK;
    }

    m->last_release = 0; /* another button ends a run */
    if (value == 0) {
        *click_code = code;
        return ACT_CLICK;
    }
    return ACT_NONE;
}

/* How long poll() may wait (ms), -1 = forever. */
static int machine_timeout(const struct machine *m, const struct config *c, long long now)
{
    if (!m->held || m->fired) return -1;
    long long due = m->press_time + c->hold_ms;
    if (c->repeat_gap_ms > 0 && m->count >= 3 && m->run_start + c->hold_ms < due)
        due = m->run_start + c->hold_ms;
    return due <= now ? 0 : (int)(due - now);
}

static enum action machine_tick(struct machine *m, const struct config *c, long long now)
{
    if (m->held && !m->fired && now >= m->quiet_until && machine_due(m, c, now))
        return machine_fire(m, now);
    return ACT_NONE;
}

#ifndef TEST
/* ------------------------------------------------------------- runtime */

#define DATA_DIR    "/mnt/onboard/.adds/remote-sleep"
#define CONFIG_PATH DATA_DIR "/config.txt"
#define LOG_PATH    DATA_DIR "/log.txt"
#define LOG_MAX     (200 * 1024)
#define LOCK_PATH   "/tmp/remote-sleep.lock"
#define INPUT_DIR   "/dev/input"

static struct config cfg;
static int foreground;

static const char default_config[] =
    "# remote-sleep settings. Edit, then restart the Kobo (or reconnect the remote).\n"
    "\n"
    "# How long the previous button must be held before the Kobo sleeps (milliseconds).\n"
    "hold_ms=1000\n"
    "\n"
    "# Key code(s) of the remote's previous button. If a long press does nothing,\n"
    "# set debug=1, press the button, and look for 'key <number>' in log.txt.\n"
    "prev_key=103,105,104\n"
    "\n"
    "# Only if log.txt shows the remote sending a stream of quick down/up pairs\n"
    "# while you hold the button: set this to a bit more than the pause between\n"
    "# them (e.g. 250). 0 = off.\n"
    "repeat_gap_ms=0\n"
    "\n"
    "# 1 = hide the remote from the reader while this tool handles it, and pass\n"
    "#     short clicks on when the button is released (long press turns no page).\n"
    "# 0 = only listen. Safest, but a long press also turns back a page first.\n"
    "grab=1\n"
    "\n"
    "# Leave empty to use any Bluetooth input device, or put (part of) the\n"
    "# remote's name here if you also use other Bluetooth devices.\n"
    "device_name=\n"
    "\n"
    "# 1 = write what happens to log.txt in this folder. Turn off again afterwards.\n"
    "debug=0\n";

static long long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static int onboard_mounted(void)
{
    struct stat st;
    return stat("/mnt/onboard/.kobo", &st) == 0 && S_ISDIR(st.st_mode);
}

static void logmsg(const char *fmt, ...)
{
    va_list ap;
    char buf[300];
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);

    if (foreground) fprintf(stderr, "%s\n", buf);
    if (!cfg.debug || !onboard_mounted()) return;

    struct stat st;
    int flags = O_WRONLY | O_CREAT | O_APPEND;
    if (stat(LOG_PATH, &st) == 0 && st.st_size > LOG_MAX) flags |= O_TRUNC;
    int fd = open(LOG_PATH, flags, 0644);
    if (fd < 0) return;
    time_t t = time(NULL);
    struct tm tm;
    localtime_r(&t, &tm);
    char line[360];
    int n = snprintf(line, sizeof line, "%02d:%02d:%02d %s\n",
                     tm.tm_hour, tm.tm_min, tm.tm_sec, buf);
    if (write(fd, line, n) < 0) { /* nothing useful to do */ }
    close(fd);
}

static void load_config(void)
{
    if (!onboard_mounted()) return; /* keep what we have (USB connected / early boot) */

    FILE *f = fopen(CONFIG_PATH, "r");
    if (!f) {
        mkdir("/mnt/onboard/.adds", 0755);
        mkdir(DATA_DIR, 0755);
        f = fopen(CONFIG_PATH, "w");
        if (f) { fputs(default_config, f); fclose(f); }
        return;
    }
    struct config c;
    config_defaults(&c);
    char line[256];
    while (fgets(line, sizeof line, f)) config_parse_line(&c, line);
    fclose(f);
    cfg = c;
}

#define BITS_PER_LONG (8 * sizeof(unsigned long))
#define NLONGS(n) (((n) + BITS_PER_LONG - 1) / BITS_PER_LONG)
static int test_bit(const unsigned long *a, int bit)
{
    return (a[bit / BITS_PER_LONG] >> (bit % BITS_PER_LONG)) & 1;
}

static int has_key(int fd, int code)
{
    unsigned long bits[NLONGS(KEY_MAX + 1)];
    memset(bits, 0, sizeof bits);
    if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof bits), bits) < 0) return 0;
    return test_bit(bits, code);
}

static int contains_nocase(const char *hay, const char *needle)
{
    size_t n = strlen(needle);
    for (; *hay; hay++)
        if (!strncasecmp(hay, needle, n)) return 1;
    return 0;
}

enum want { WANT_REMOTE, WANT_POWER };

/* Scan /dev/input for the remote or for the built-in power key. Returns fd or -1. */
static int find_device_pass(enum want want, int verbose, int strict);

static int find_device(enum want want, int verbose)
{
    int fd = find_device_pass(want, verbose, 1);
    if (fd < 0 && want == WANT_REMOTE && !cfg.device_name[0])
        fd = find_device_pass(want, 0, 0); /* any Bluetooth device with keys */
    return fd;
}

static int find_device_pass(enum want want, int verbose, int strict)
{
    DIR *d = opendir(INPUT_DIR);
    if (!d) return -1;
    struct dirent *e;
    int found = -1;
    while (found < 0 && (e = readdir(d))) {
        if (strncmp(e->d_name, "event", 5)) continue;
        char path[300];
        snprintf(path, sizeof path, INPUT_DIR "/%s", e->d_name);
        int fd = open(path, O_RDWR | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) continue;

        struct input_id id;
        char name[80] = "?";
        memset(&id, 0, sizeof id);
        ioctl(fd, EVIOCGID, &id);
        ioctl(fd, EVIOCGNAME(sizeof name - 1), name);
        int bt = id.bustype == BUS_BLUETOOTH;
        if (verbose)
            logmsg("device %s name='%s' bus=%d%s", e->d_name, name, id.bustype,
                   bt ? " (bluetooth)" : "");

        int ok;
        if (want == WANT_POWER) {
            ok = !bt && has_key(fd, KEY_POWER);
        } else {
            int any_prev = !strict;
            unsigned long evbits = 0;
            ioctl(fd, EVIOCGBIT(0, sizeof evbits), &evbits);
            for (int i = 0; i < cfg.nprev; i++) any_prev |= has_key(fd, cfg.prev[i]);
            any_prev = any_prev && (evbits & (1UL << EV_KEY));
            ok = cfg.device_name[0] ? contains_nocase(name, cfg.device_name)
                                    : (bt && any_prev);
        }
        if (ok) {
            found = fd;
            if (want == WANT_REMOTE) logmsg("using %s ('%s') as remote", e->d_name, name);
        } else {
            close(fd);
        }
    }
    closedir(d);
    return found;
}

static int write_key(int fd, int code, int value)
{
    struct kev ev[2];
    memset(ev, 0, sizeof ev);
    ev[0].type = EV_KEY; ev[0].code = code; ev[0].value = value;
    ev[1].type = EV_SYN; ev[1].code = SYN_REPORT;
    return write(fd, ev, sizeof ev) == (ssize_t)sizeof ev ? 0 : -1;
}

static void do_sleep(void)
{
    if (cfg.sleep_cmd[0]) {
        logmsg("long press -> running sleep_cmd");
        if (system(cfg.sleep_cmd) != 0) logmsg("sleep_cmd failed");
        return;
    }
    int fd = find_device(WANT_POWER, 0);
    if (fd < 0) {
        logmsg("long press, but no power-key device found");
        return;
    }
    logmsg("long press -> sleep");
    write_key(fd, KEY_POWER, 1);
    usleep(120 * 1000);
    write_key(fd, KEY_POWER, 0);
    close(fd);
}

/*
 * Pass a click on to everyone else: briefly release the grab, inject
 * press + release into the remote's own device, grab again. The injected
 * events also come back to us; they are sitting in our queue by the time
 * write() returns, so drain and drop them right here.
 */
static void forward_click(int fd, int code, struct machine *m, long long now)
{
    ioctl(fd, EVIOCGRAB, (void *)0);
    int ok = write_key(fd, code, 1) == 0 && write_key(fd, code, 0) == 0;
    ioctl(fd, EVIOCGRAB, (void *)1);
    if (!ok) { logmsg("could not forward key %d: %s", code, strerror(errno)); return; }

    int echo_down = 1, echo_up = 1;
    struct kev ev;
    while (read(fd, &ev, sizeof ev) == (ssize_t)sizeof ev) {
        if (ev.type != EV_KEY) continue;
        if (ev.code == code && ev.value == 1 && echo_down) { echo_down = 0; continue; }
        if (ev.code == code && ev.value == 0 && echo_up)   { echo_up = 0;   continue; }
        /* a real event that slipped in between: only track state, never recurse */
        int dummy;
        machine_key(m, &cfg, ev.code, ev.value, now, &dummy);
    }
}

static void daemonize(void)
{
    if (fork() > 0) _exit(0);
    setsid();
    if (fork() > 0) _exit(0);
    if (chdir("/") < 0) { /* ignore */ }
    int n = open("/dev/null", O_RDWR);
    if (n >= 0) { dup2(n, 0); dup2(n, 1); dup2(n, 2); if (n > 2) close(n); }
}

int main(int argc, char **argv)
{
    for (int i = 1; i < argc; i++)
        if (!strcmp(argv[i], "-f")) foreground = 1;

    config_defaults(&cfg);
    if (!foreground) daemonize();

    int lock = open(LOCK_PATH, O_RDWR | O_CREAT | O_CLOEXEC, 0644);
    if (lock >= 0 && flock(lock, LOCK_EX | LOCK_NB) < 0) return 0; /* already running */

    int ino = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (ino >= 0 && inotify_add_watch(ino, INPUT_DIR, IN_CREATE | IN_ATTRIB) < 0) {
        close(ino);
        ino = -1; /* fall back to checking every few seconds */
    }

    int dev = -1;
    int rescan = 1;
    struct machine m;
    memset(&m, 0, sizeof m);

    for (;;) {
        if (dev < 0 && rescan) {
            rescan = 0;
            load_config();
            dev = find_device(WANT_REMOTE, cfg.debug);
            if (dev >= 0) {
                memset(&m, 0, sizeof m);
                if (cfg.grab && ioctl(dev, EVIOCGRAB, (void *)1) < 0) {
                    logmsg("grab failed (%s), listening only", strerror(errno));
                    cfg.grab = 0;
                }
            }
        }

        /* poll() skips entries with a negative fd, so both slots can always be passed */
        struct pollfd p[2] = { { ino, POLLIN, 0 }, { dev, POLLIN, 0 } };
        int timeout = dev >= 0 ? machine_timeout(&m, &cfg, now_ms())
                               : (ino >= 0 ? -1 : 5000);
        int r = poll(p, 2, timeout);
        if (r < 0 && errno != EINTR) { sleep(1); continue; }

        if (ino < 0 && dev < 0) rescan = 1; /* no inotify: plain polling */
        if (r > 0 && p[0].revents) {
            char buf[1024];
            while (read(ino, buf, sizeof buf) > 0) {}
            if (dev < 0) { usleep(700 * 1000); rescan = 1; } /* let the node settle */
        }

        if (dev >= 0 && r > 0 && p[1].revents) {
            struct kev ev;
            ssize_t n;
            while ((n = read(dev, &ev, sizeof ev)) == (ssize_t)sizeof ev) {
                if (cfg.debug && ev.type == EV_KEY && ev.value != 2)
                    logmsg("key %d %s", ev.code, ev.value ? "down" : "up");
                else if (cfg.debug && ev.type != EV_KEY && ev.type != EV_SYN)
                    logmsg("other event type=%d code=%d value=0x%x", ev.type, ev.code, ev.value);
                if (ev.type != EV_KEY) continue;
                int code = 0;
                enum action a = machine_key(&m, &cfg, ev.code, ev.value, now_ms(), &code);
                if (a == ACT_CLICK && cfg.grab) forward_click(dev, code, &m, now_ms());
                else if (a == ACT_SLEEP) do_sleep();
            }
            if (n == 0 || (n < 0 && errno != EAGAIN && errno != EINTR) ||
                (p[1].revents & (POLLHUP | POLLERR | POLLNVAL))) {
                logmsg("remote disconnected");
                close(dev);
                dev = -1;
                rescan = 1;
                continue;
            }
        }

        if (dev >= 0 && machine_tick(&m, &cfg, now_ms()) == ACT_SLEEP) do_sleep();
    }
}
#endif /* !TEST */
