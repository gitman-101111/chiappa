// chiappa-powerkey — power-button/cover → suspend daemon for the reMarkable
// Paper Pro Move. KoReader's Move input layer never surfaces KEY_POWER to its
// own handlers, so this owns the button: logind is set HandlePowerKey=ignore
// and a short press here becomes a logind Suspend() call. The folio cover
// (hall sensor, EV_SW/SW_LID) suspends on close; cover-open wakes from deep
// suspend natively (SPLD folio wake — IRQ 39, wakeup-reason 0x08).
//
// Event-driven state machine (no timed drains):
//   input fds — the bbnsm pwrkey evdev device + the hall-sensor (SW_LID) device
//   sd-bus fd — logind's PrepareForSleep signal (true = about to sleep,
//               false = resumed; the false edge fires even when the suspend
//               attempt fails, so the machine self-heals)
//
// The vendor bbnsm driver REPLAYS the wake press as an input event on resume
// ("never miss a press"), so a naive daemon re-suspends immediately on wake.
// States:
//   AWAKE      press → Suspend() → SUSPENDING
//   SUSPENDING input discarded (release chatter, presses while the sleep
//              screen draws); PrepareForSleep(false) → RESUME_DEBOUNCE
//   RESUME_DEBOUNCE input discarded for a short beat (the replayed wake-press
//              burst arrives right at thaw), then → AWAKE — unless the cover
//              is closed, in which case → suspend again. The folio wake
//              source fires on any cover edge, so closing the cover over a
//              sleeping device wakes it; the SW_LID close event lands inside
//              the debounce window and, being a switch state, never re-fires.
// A generous fallback timeout re-arms from SUSPENDING if no resume edge ever
// arrives (e.g. logind restarted mid-flight).
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <linux/input.h>
#include <sys/ioctl.h>
#include <systemd/sd-bus.h>

#define LONG_BITS (8 * sizeof(long))
#define NLONGS(n) (((n) + LONG_BITS - 1) / LONG_BITS)

// Replayed wake-press burst window after the resume edge (ms). The replay is
// synthesized at thaw, sub-second after PrepareForSleep(false) — this is not a
// human-timing race.
#define RESUME_DEBOUNCE_MS 1500
// Fallback: re-arm if no resume edge arrives after a suspend request (ms).
#define SUSPENDING_TIMEOUT_MS 90000

enum state { AWAKE, SUSPENDING, RESUME_DEBOUNCE };

static int test_bit(const unsigned long *arr, int bit) {
    return (arr[bit / LONG_BITS] >> (bit % LONG_BITS)) & 1UL;
}

// Find the trigger devices: the power key (prefer a device named
// "pwrkey"/"power", else the first advertising KEY_POWER) and the cover
// (first device advertising EV_SW + SW_LID).
static int open_pwrkey(int *lid_fd) {
    DIR *d = opendir("/dev/input");
    if (!d) return -1;
    struct dirent *e;
    int fd = -1, fallback = -1;
    char path[300], name[256];
    unsigned long keybit[NLONGS(KEY_MAX + 1)];
    unsigned long swbit[NLONGS(SW_MAX + 1)];
    *lid_fd = -1;
    while ((e = readdir(d))) {
        if (strncmp(e->d_name, "event", 5) != 0) continue;
        snprintf(path, sizeof path, "/dev/input/%s", e->d_name);
        int f = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (f < 0) continue;
        name[0] = 0;
        ioctl(f, EVIOCGNAME(sizeof name), name);
        memset(keybit, 0, sizeof keybit);
        ioctl(f, EVIOCGBIT(EV_KEY, sizeof keybit), keybit);
        memset(swbit, 0, sizeof swbit);
        ioctl(f, EVIOCGBIT(EV_SW, sizeof swbit), swbit);
        if (*lid_fd < 0 && test_bit(swbit, SW_LID)) {
            *lid_fd = f;
            continue;
        }
        int has_power = test_bit(keybit, KEY_POWER);
        if (has_power && fd < 0 && (strcasestr(name, "pwrkey") || strcasestr(name, "power"))) {
            fd = f;
            continue;
        }
        if (has_power && fallback < 0) {
            fallback = f;
            continue;
        }
        close(f);
    }
    closedir(d);
    if (fd < 0) fd = fallback;
    return fd;
}

// Live cover state (not events): SW_LID bit of the device's switch bitmap.
static int lid_closed(int lid_fd) {
    unsigned long swstate[NLONGS(SW_MAX + 1)];
    if (lid_fd < 0) return 0;
    memset(swstate, 0, sizeof swstate);
    if (ioctl(lid_fd, EVIOCGSW(sizeof swstate), swstate) < 0) return 0;
    return test_bit(swstate, SW_LID);
}

static long long now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_BOOTTIME, &ts); // advances across suspend
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

// Drain everything currently readable; returns 1 on a suspend trigger
// (KEY_POWER press, or the cover closing: SW_LID = 1).
static int drain_input(int fd) {
    struct input_event ev;
    int trigger = 0;
    while (read(fd, &ev, sizeof ev) == (ssize_t)sizeof ev) {
        if (ev.type == EV_KEY && ev.code == KEY_POWER && ev.value == 1)
            trigger = 1;
        if (ev.type == EV_SW && ev.code == SW_LID && ev.value == 1)
            trigger = 1;
    }
    return trigger;
}

static void request_suspend(sd_bus *bus) {
    sd_bus_error err = SD_BUS_ERROR_NULL;
    int r = sd_bus_call_method(bus,
                               "org.freedesktop.login1", "/org/freedesktop/login1",
                               "org.freedesktop.login1.Manager", "Suspend",
                               &err, NULL, "b", 0 /* not interactive */);
    if (r < 0)
        fprintf(stderr, "chiappa-powerkey: Suspend() failed: %s\n",
                err.message ? err.message : strerror(-r));
    sd_bus_error_free(&err);
}

// PrepareForSleep(false) = resumed (or the sleep attempt concluded).
static int resumed_edge = 0;
static int on_prepare_for_sleep(sd_bus_message *m, void *userdata, sd_bus_error *ret_error) {
    (void)userdata; (void)ret_error;
    int start;
    if (sd_bus_message_read(m, "b", &start) >= 0 && !start)
        resumed_edge = 1;
    return 0;
}

int main(void) {
    int lid_fd = -1;
    int input_fd = open_pwrkey(&lid_fd);
    if (input_fd < 0) {
        fprintf(stderr, "chiappa-powerkey: no power-key input device found\n");
        return 1;
    }

    sd_bus *bus = NULL;
    if (sd_bus_default_system(&bus) < 0) {
        fprintf(stderr, "chiappa-powerkey: cannot connect to system bus\n");
        return 1;
    }
    if (sd_bus_match_signal(bus, NULL,
                            "org.freedesktop.login1", "/org/freedesktop/login1",
                            "org.freedesktop.login1.Manager", "PrepareForSleep",
                            on_prepare_for_sleep, NULL) < 0) {
        fprintf(stderr, "chiappa-powerkey: cannot subscribe to PrepareForSleep\n");
        return 1;
    }

    enum state st = AWAKE;
    long long state_since = now_ms();

    struct pollfd fds[3];
    int nfds = 0;
    int key_idx = nfds++;
    fds[key_idx] = (struct pollfd){input_fd, POLLIN, 0};
    int lid_idx = -1;
    if (lid_fd >= 0) {
        lid_idx = nfds++;
        fds[lid_idx] = (struct pollfd){lid_fd, POLLIN, 0};
    }
    int bus_idx = nfds++;
    fds[bus_idx] = (struct pollfd){sd_bus_get_fd(bus), POLLIN, 0};

    for (;;) {
        fds[bus_idx].fd = sd_bus_get_fd(bus);
        fds[bus_idx].events = sd_bus_get_events(bus) >= 0 ? (short)sd_bus_get_events(bus) : POLLIN;

        int timeout = -1;
        if (st == RESUME_DEBOUNCE)
            timeout = (int)(RESUME_DEBOUNCE_MS - (now_ms() - state_since));
        else if (st == SUSPENDING)
            timeout = (int)(SUSPENDING_TIMEOUT_MS - (now_ms() - state_since));
        if (timeout < 0 && st != AWAKE) timeout = 0;

        int r = poll(fds, nfds, timeout);
        if (r < 0 && errno != EINTR) {
            perror("chiappa-powerkey: poll");
            return 1;
        }

        // Bus traffic (delivers PrepareForSleep into resumed_edge)
        while (sd_bus_process(bus, NULL) > 0) {}

        int pressed = (fds[key_idx].revents & POLLIN) ? drain_input(input_fd) : 0;
        if (lid_idx >= 0 && (fds[lid_idx].revents & POLLIN))
            pressed |= drain_input(lid_fd);
        long long t = now_ms();

        switch (st) {
        case AWAKE:
            if (pressed) {
                request_suspend(bus);
                resumed_edge = 0;
                st = SUSPENDING;
                state_since = t;
            }
            break;
        case SUSPENDING:
            // Input discarded. Leave on the resume edge or the fallback timeout.
            if (resumed_edge) {
                resumed_edge = 0;
                st = RESUME_DEBOUNCE;
                state_since = t;
            } else if (t - state_since >= SUSPENDING_TIMEOUT_MS) {
                st = AWAKE;
            }
            break;
        case RESUME_DEBOUNCE:
            // Discard the replayed wake-press burst, then re-arm — or, if
            // this wake was the cover closing, go straight back to sleep.
            if (t - state_since >= RESUME_DEBOUNCE_MS) {
                if (lid_closed(lid_fd)) {
                    request_suspend(bus);
                    resumed_edge = 0;
                    st = SUSPENDING;
                } else {
                    st = AWAKE;
                }
                state_since = t;
            }
            break;
        }
    }
}
