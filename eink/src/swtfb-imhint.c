/* swtfb-imhint — signal an on-screen keyboard on text-input focus changes.
 *
 * Usage: swtfb-imhint <osk-pid>
 *
 * Binds the Wayland seat's input-method slot (zwp_input_method_v2) and does
 * nothing with it except observe: a text field gaining focus (activate)
 * sends SIGUSR2 (show) to <osk-pid>, losing focus (deactivate) sends SIGUSR1
 * (hide) — wvkbd's show/hide signals. This gives a keyboard that is not
 * input-method-aware (wvkbd) rise-on-focus behavior without any input-method
 * framework: the OSK still injects keys via the virtual-keyboard protocol;
 * this tool never commits text.
 *
 * Failure policy: on any error (protocol absent, slot already taken —
 * "unavailable" — or compositor gone) the OSK is shown (SIGUSR2) and the
 * tool exits, so the failure mode is a visible keyboard, never a missing
 * one. Pair with an OSK started hidden.
 *
 * Build: needs wayland-client plus scanner-generated glue for
 * input-method-unstable-v2.xml (ships in the wlroots source tree):
 *   wayland-scanner client-header input-method-unstable-v2.xml input-method-unstable-v2-client-protocol.h
 *   wayland-scanner private-code  input-method-unstable-v2.xml input-method-unstable-v2-protocol.c
 *   cc -O2 -o swtfb-imhint swtfb-imhint.c input-method-unstable-v2-protocol.c -lwayland-client
 */
#define _GNU_SOURCE
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <wayland-client.h>
#include "input-method-unstable-v2-client-protocol.h"

static pid_t g_osk;
static struct wl_seat *g_seat;
static struct zwp_input_method_manager_v2 *g_mgr;

static int g_active_pending, g_active;
static int g_unavailable;

static void die_visible(const char *why) {
    fprintf(stderr, "swtfb-imhint: %s — showing OSK and exiting\n", why);
    kill(g_osk, SIGUSR2);
    exit(1);
}

/* ── input method listener ──────────────────────────────────────────────── */
static void im_activate(void *d, struct zwp_input_method_v2 *im) {
    (void)d; (void)im;
    g_active_pending = 1;
}
static void im_deactivate(void *d, struct zwp_input_method_v2 *im) {
    (void)d; (void)im;
    g_active_pending = 0;
}
static void im_surrounding(void *d, struct zwp_input_method_v2 *im,
                           const char *t, uint32_t c, uint32_t a) {
    (void)d; (void)im; (void)t; (void)c; (void)a;
}
static void im_cause(void *d, struct zwp_input_method_v2 *im, uint32_t c) {
    (void)d; (void)im; (void)c;
}
static void im_content(void *d, struct zwp_input_method_v2 *im,
                       uint32_t hint, uint32_t purpose) {
    (void)d; (void)im; (void)hint; (void)purpose;
}
static void im_done(void *d, struct zwp_input_method_v2 *im) {
    (void)d; (void)im;
    if (g_active_pending != g_active) {
        g_active = g_active_pending;
        kill(g_osk, g_active ? SIGUSR2 : SIGUSR1);
    }
}
static void im_unavailable(void *d, struct zwp_input_method_v2 *im) {
    (void)d; (void)im;
    g_unavailable = 1;
}

static const struct zwp_input_method_v2_listener im_listener = {
    .activate = im_activate,
    .deactivate = im_deactivate,
    .surrounding_text = im_surrounding,
    .text_change_cause = im_cause,
    .content_type = im_content,
    .done = im_done,
    .unavailable = im_unavailable,
};

/* ── registry ───────────────────────────────────────────────────────────── */
static void reg_global(void *d, struct wl_registry *reg, uint32_t name,
                       const char *iface, uint32_t ver) {
    (void)d; (void)ver;
    if (!strcmp(iface, wl_seat_interface.name) && !g_seat)
        g_seat = wl_registry_bind(reg, name, &wl_seat_interface, 1);
    else if (!strcmp(iface, zwp_input_method_manager_v2_interface.name))
        g_mgr = wl_registry_bind(reg, name,
                                 &zwp_input_method_manager_v2_interface, 1);
}
static void reg_remove(void *d, struct wl_registry *r, uint32_t n) {
    (void)d; (void)r; (void)n;
}
static const struct wl_registry_listener reg_listener = {reg_global, reg_remove};

int main(int argc, char **argv) {
    if (argc != 2 || (g_osk = (pid_t)atoi(argv[1])) <= 0) {
        fprintf(stderr, "usage: %s <osk-pid>\n", argv[0]);
        return 2;
    }

    struct wl_display *dpy = wl_display_connect(NULL);
    if (!dpy) die_visible("no Wayland display");
    struct wl_registry *reg = wl_display_get_registry(dpy);
    wl_registry_add_listener(reg, &reg_listener, NULL);
    wl_display_roundtrip(dpy);
    if (!g_seat || !g_mgr)
        die_visible("compositor lacks wl_seat/zwp_input_method_manager_v2");

    struct zwp_input_method_v2 *im =
        zwp_input_method_manager_v2_get_input_method(g_mgr, g_seat);
    zwp_input_method_v2_add_listener(im, &im_listener, NULL);
    wl_display_roundtrip(dpy);
    if (g_unavailable)
        die_visible("input-method slot already taken");

    while (wl_display_dispatch(dpy) != -1) {
        if (g_unavailable)
            die_visible("input-method slot lost");
    }
    die_visible("compositor connection closed");
    return 1; /* unreachable */
}
