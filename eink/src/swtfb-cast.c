/* swtfb-cast — mirror a Wayland compositor's output onto the einkbridge panel.
 *
 * A wlr-screencopy client: captures the compositor's output into a wl_shm
 * buffer, converts XRGB8888 → RGB565 into the einkbridge shared framebuffer
 * (/dev/shm/swtfb) and sends an rm2fb update for the DAMAGED region only —
 * e-ink refreshes just what changed. Damage-driven via copy_with_damage
 * (screencopy v2+): between frames the process sleeps; an idle compositor
 * costs nothing.
 *
 * Intended pairing: a headless-backend kiosk compositor (WLR_BACKENDS=
 * headless,libinput) whose single output is set to the panel geometry. If the
 * captured output size differs from the panel, frames are nearest-neighbour
 * scaled full-screen instead (correct but blurrier — match sizes for best
 * results).
 *
 * Env (REQUIRED, same convention as einkbridge):
 *   SWTFB_WIDTH / SWTFB_HEIGHT   panel size in pixels
 * Env (optional):
 *   SWTFB_CAST_WAVEFORM   rm2fb waveform id for updates (default 2 = GC16)
 *   SWTFB_CAST_WAVEFORM_FAST
 *                         waveform for SMALL damage rects — key flashes,
 *                         cursors, typed characters (default 1 = DU, fast
 *                         2-level; set equal to SWTFB_CAST_WAVEFORM to
 *                         disable the distinction). "Small" = damage area
 *                         under 1/16 of the panel.
 *   WAYLAND_DISPLAY       which compositor to mirror (standard Wayland)
 *
 * Build: needs wayland-client plus scanner-generated screencopy glue:
 *   wayland-scanner client-header wlr-screencopy-unstable-v1.xml wlr-screencopy-unstable-v1-client-protocol.h
 *   wayland-scanner private-code  wlr-screencopy-unstable-v1.xml wlr-screencopy-unstable-v1-protocol.c
 *   cc -O2 -o swtfb-cast swtfb-cast.c wlr-screencopy-unstable-v1-protocol.c -lwayland-client
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <wayland-client.h>
#include "wlr-screencopy-unstable-v1-client-protocol.h"

#define SHM_PATH "/dev/shm/swtfb"
#define IPC_PATH "/tmp/swtfb.ipc"

typedef struct { uint32_t top, left, width, height; } rect_t;
typedef struct { rect_t region; uint32_t waveform; uint32_t flags; } update_t;

static int env_dim(const char *name) {
    const char *v = getenv(name);
    int n = v ? atoi(v) : 0;
    if (n <= 0) {
        fprintf(stderr,
                "swtfb-cast: %s is unset/invalid — set SWTFB_WIDTH and "
                "SWTFB_HEIGHT to the panel size in pixels (e.g. 954 and 1696 "
                "for the reMarkable Paper Pro Move)\n", name);
        exit(2);
    }
    return n;
}

/* ── globals wired up by registry / frame listeners ─────────────────────── */
static struct wl_shm *g_wlshm;
static struct wl_output *g_output;
static struct zwlr_screencopy_manager_v1 *g_mgr;

static int g_panel_w, g_panel_h;
static uint16_t *g_fb;          /* mmap of /dev/shm/swtfb (RGB565) */
static int g_ipc = -1;
static uint32_t g_waveform = 2;      /* GC16 */
static uint32_t g_waveform_fast = 1; /* DU — small-damage updates */

/* current capture buffer */
static struct wl_buffer *g_buf;
static void *g_bufdata;
static size_t g_bufsize;
static int32_t g_cap_w, g_cap_h, g_cap_stride;
static uint32_t g_cap_fmt;

/* per-frame state */
static int g_have_buffer, g_ready, g_failed;
static rect_t g_damage;
static int g_have_damage;

static void ipc_send(rect_t r) {
    /* Vendor-style feel: small updates (key flashes, cursors) refresh with
     * the fast waveform; large ones (page loads, scrolls) at full quality. */
    uint32_t wf =
        ((uint64_t)r.width * r.height <
         (uint64_t)g_panel_w * g_panel_h / 16)
            ? g_waveform_fast
            : g_waveform;
    update_t u = { r, wf, 0 };
    if (write(g_ipc, &u, sizeof u) != (ssize_t)sizeof u) {
        /* bridge restarted: reconnect once */
        close(g_ipc);
        g_ipc = socket(AF_UNIX, SOCK_STREAM, 0);
        struct sockaddr_un a = { .sun_family = AF_UNIX };
        strncpy(a.sun_path, IPC_PATH, sizeof(a.sun_path) - 1);
        if (connect(g_ipc, (struct sockaddr *)&a, sizeof a) == 0)
            (void)!write(g_ipc, &u, sizeof u);
    }
}

static inline uint16_t px565(uint32_t x) {
    /* XRGB8888 little-endian word: 0xXXRRGGBB */
    return (uint16_t)((((x >> 16) & 0xF8) << 8) |
                      (((x >> 8) & 0xFC) << 3) |
                      ((x & 0xFF) >> 3));
}

/* Convert the damaged rect 1:1 (capture size == panel size). */
static void blit_rect(rect_t r) {
    const uint8_t *src = g_bufdata;
    for (uint32_t y = r.top; y < r.top + r.height; y++) {
        const uint32_t *row = (const uint32_t *)(src + (size_t)y * g_cap_stride);
        uint16_t *dst = g_fb + (size_t)y * g_panel_w;
        for (uint32_t x = r.left; x < r.left + r.width; x++)
            dst[x] = px565(row[x]);
    }
}

/* Nearest-neighbour full-frame scale (capture size != panel size). */
static void blit_scaled(void) {
    const uint8_t *src = g_bufdata;
    for (int y = 0; y < g_panel_h; y++) {
        const uint32_t *row =
            (const uint32_t *)(src + (size_t)(y * g_cap_h / g_panel_h) * g_cap_stride);
        uint16_t *dst = g_fb + (size_t)y * g_panel_w;
        for (int x = 0; x < g_panel_w; x++)
            dst[x] = px565(row[x * g_cap_w / g_panel_w]);
    }
}

/* ── screencopy frame listener ──────────────────────────────────────────── */
static void on_buffer(void *d, struct zwlr_screencopy_frame_v1 *f,
                      uint32_t fmt, uint32_t w, uint32_t h, uint32_t stride) {
    (void)d; (void)f;
    if (!g_buf || (int32_t)w != g_cap_w || (int32_t)h != g_cap_h ||
        (int32_t)stride != g_cap_stride || fmt != g_cap_fmt) {
        if (g_buf) { wl_buffer_destroy(g_buf); munmap(g_bufdata, g_bufsize); }
        g_cap_w = w; g_cap_h = h; g_cap_stride = stride; g_cap_fmt = fmt;
        g_bufsize = (size_t)stride * h;
        int fd = memfd_create("swtfb-cast", MFD_CLOEXEC);
        if (fd < 0 || ftruncate(fd, g_bufsize) < 0) { perror("memfd"); exit(1); }
        g_bufdata = mmap(NULL, g_bufsize, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        struct wl_shm_pool *pool = wl_shm_create_pool(g_wlshm, fd, g_bufsize);
        g_buf = wl_shm_pool_create_buffer(pool, 0, w, h, stride, fmt);
        wl_shm_pool_destroy(pool);
        close(fd);
    }
    g_have_buffer = 1;
}

static void on_flags(void *d, struct zwlr_screencopy_frame_v1 *f, uint32_t fl) {
    (void)d; (void)f; (void)fl;
}

static void on_ready(void *d, struct zwlr_screencopy_frame_v1 *f,
                     uint32_t s_hi, uint32_t s_lo, uint32_t ns) {
    (void)d; (void)f; (void)s_hi; (void)s_lo; (void)ns;
    g_ready = 1;
}

static void on_failed(void *d, struct zwlr_screencopy_frame_v1 *f) {
    (void)d; (void)f;
    g_failed = 1;
}

static void on_damage(void *d, struct zwlr_screencopy_frame_v1 *f,
                      uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
    (void)d; (void)f;
    if (!g_have_damage) {
        g_damage = (rect_t){ y, x, w, h };
        g_have_damage = 1;
    } else { /* union */
        uint32_t x2 = g_damage.left + g_damage.width, y2 = g_damage.top + g_damage.height;
        if (x < g_damage.left) g_damage.left = x;
        if (y < g_damage.top) g_damage.top = y;
        if (x + w > x2) x2 = x + w;
        if (y + h > y2) y2 = y + h;
        g_damage.width = x2 - g_damage.left;
        g_damage.height = y2 - g_damage.top;
    }
}

static void on_linux_dmabuf(void *d, struct zwlr_screencopy_frame_v1 *f,
                            uint32_t fmt, uint32_t w, uint32_t h) {
    (void)d; (void)f; (void)fmt; (void)w; (void)h; /* shm only */
}

static void on_buffer_done(void *d, struct zwlr_screencopy_frame_v1 *f) {
    (void)d; (void)f;
}

static const struct zwlr_screencopy_frame_v1_listener frame_listener = {
    .buffer = on_buffer,
    .flags = on_flags,
    .ready = on_ready,
    .failed = on_failed,
    .damage = on_damage,
    .linux_dmabuf = on_linux_dmabuf,
    .buffer_done = on_buffer_done,
};

/* ── registry ───────────────────────────────────────────────────────────── */
static void reg_global(void *d, struct wl_registry *reg, uint32_t name,
                       const char *iface, uint32_t ver) {
    (void)d;
    if (!strcmp(iface, wl_shm_interface.name))
        g_wlshm = wl_registry_bind(reg, name, &wl_shm_interface, 1);
    else if (!strcmp(iface, wl_output_interface.name) && !g_output)
        g_output = wl_registry_bind(reg, name, &wl_output_interface, 1);
    else if (!strcmp(iface, zwlr_screencopy_manager_v1_interface.name))
        g_mgr = wl_registry_bind(reg, name, &zwlr_screencopy_manager_v1_interface,
                                 ver >= 2 ? 2 : 1);
}
static void reg_remove(void *d, struct wl_registry *r, uint32_t n) {
    (void)d; (void)r; (void)n;
}
static const struct wl_registry_listener reg_listener = { reg_global, reg_remove };

int main(void) {
    g_panel_w = env_dim("SWTFB_WIDTH");
    g_panel_h = env_dim("SWTFB_HEIGHT");
    const char *wf = getenv("SWTFB_CAST_WAVEFORM");
    if (wf && atoi(wf) > 0) g_waveform = (uint32_t)atoi(wf);
    const char *wff = getenv("SWTFB_CAST_WAVEFORM_FAST");
    if (wff && atoi(wff) > 0) g_waveform_fast = (uint32_t)atoi(wff);

    int fbfd = open(SHM_PATH, O_RDWR);
    if (fbfd < 0) { perror("swtfb-cast: open " SHM_PATH " (is einkbridge running?)"); return 1; }
    g_fb = mmap(NULL, (size_t)g_panel_w * g_panel_h * 2, PROT_WRITE, MAP_SHARED, fbfd, 0);
    if (g_fb == MAP_FAILED) { perror("swtfb-cast: mmap"); return 1; }
    close(fbfd);

    g_ipc = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un a = { .sun_family = AF_UNIX };
    strncpy(a.sun_path, IPC_PATH, sizeof(a.sun_path) - 1);
    if (connect(g_ipc, (struct sockaddr *)&a, sizeof a) < 0) {
        perror("swtfb-cast: connect " IPC_PATH);
        return 1;
    }

    struct wl_display *dpy = wl_display_connect(NULL);
    if (!dpy) { fprintf(stderr, "swtfb-cast: no Wayland display (WAYLAND_DISPLAY unset?)\n"); return 1; }
    struct wl_registry *reg = wl_display_get_registry(dpy);
    wl_registry_add_listener(reg, &reg_listener, NULL);
    wl_display_roundtrip(dpy);
    if (!g_wlshm || !g_output || !g_mgr) {
        fprintf(stderr, "swtfb-cast: compositor lacks wl_shm/wl_output/zwlr_screencopy_manager_v1\n");
        return 1;
    }
    int have_damage_proto =
        zwlr_screencopy_manager_v1_get_version(g_mgr) >= 2;

    int first = 1;
    for (;;) {
        struct zwlr_screencopy_frame_v1 *frame =
            zwlr_screencopy_manager_v1_capture_output(g_mgr, 0, g_output);
        zwlr_screencopy_frame_v1_add_listener(frame, &frame_listener, NULL);
        g_have_buffer = g_ready = g_failed = g_have_damage = 0;

        /* wait for buffer metadata, then request the copy. copy_with_damage
         * BLOCKS (server-side) until the output actually changes — that is
         * the sleep between frames. */
        while (!g_have_buffer && !g_failed && wl_display_dispatch(dpy) != -1) {}
        if (!g_failed) {
            if (have_damage_proto && !first)
                zwlr_screencopy_frame_v1_copy_with_damage(frame, g_buf);
            else
                zwlr_screencopy_frame_v1_copy(frame, g_buf);
            while (!g_ready && !g_failed && wl_display_dispatch(dpy) != -1) {}
        }

        if (g_ready) {
            if (g_cap_w == g_panel_w && g_cap_h == g_panel_h) {
                rect_t r = (g_have_damage && !first)
                    ? g_damage
                    : (rect_t){0, 0, (uint32_t)g_panel_w, (uint32_t)g_panel_h};
                /* clamp */
                if (r.left + r.width > (uint32_t)g_panel_w) r.width = g_panel_w - r.left;
                if (r.top + r.height > (uint32_t)g_panel_h) r.height = g_panel_h - r.top;
                if (r.width && r.height) { blit_rect(r); ipc_send(r); }
            } else {
                blit_scaled();
                ipc_send((rect_t){0, 0, (uint32_t)g_panel_w, (uint32_t)g_panel_h});
            }
            first = 0;
        } else {
            usleep(200 * 1000); /* failed capture: back off */
        }
        zwlr_screencopy_frame_v1_destroy(frame);
        /* coalesce bursts: e-ink cannot use >10 fps anyway */
        usleep(80 * 1000);
    }
}
