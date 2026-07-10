/* einkbridge-fb-shim — LD_PRELOAD /dev/fb0 → einkbridge shim.
 *
 * Makes an mxcfb-fbdev app (KOReader's default reMarkable backend, and any
 * classic reMarkable-1/i.MX app) render through einkbridge without a real
 * framebuffer device. It intercepts three things:
 *
 *   open("/dev/fb0")            → an fd onto /dev/shm/swtfb (einkbridge's shared
 *                                 RGB565 buffer), so the app's mmap of the "fb"
 *                                 lands directly in the bridge buffer — zero-copy.
 *   ioctl(FBIOGET_[VF]SCREENINFO) → report 954x1696, 16bpp RGB565, line 954*2.
 *   ioctl(MXCFB_SEND_UPDATE)    → translate the mxcfb update rect + waveform to
 *                                 einkbridge's 24-byte swtfb_update and send it on
 *                                 a persistent connection to /tmp/swtfb.ipc.
 *
 * Everything else falls through to the real libc. No einkbridge changes needed;
 * this speaks the exact rm2fb protocol in einkbridge.cpp.
 *
 * Build:  cc -O2 -shared -fPIC -o einkbridge-fb-shim.so einkbridge-fb-shim.c -ldl -lpthread
 * Use:    LD_PRELOAD=/path/einkbridge-fb-shim.so <mxcfb app>
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

/* Panel geometry: REQUIRED SWTFB_WIDTH/SWTFB_HEIGHT env vars (same
 * convention as einkbridge). No compiled-in default — a preloaded process
 * with them unset exits loudly rather than faking a wrong-sized fb. */
static int g_fb_w, g_fb_h;
__attribute__((constructor)) static void fb_dims_init(void) {
    const char *w = getenv("SWTFB_WIDTH"), *h = getenv("SWTFB_HEIGHT");
    g_fb_w = w ? atoi(w) : 0;
    g_fb_h = h ? atoi(h) : 0;
    if (g_fb_w <= 0 || g_fb_h <= 0) {
        fprintf(stderr,
                "einkbridge-fb-shim: SWTFB_WIDTH/SWTFB_HEIGHT unset/invalid — "
                "set them to the panel size in pixels (e.g. 954 and 1696 for "
                "the reMarkable Paper Pro Move)\n");
        _exit(2);
    }
}
#define FB_W g_fb_w
#define FB_H g_fb_h
#define FB_PATH "/dev/fb0"
#define SHM_PATH "/dev/shm/swtfb"
#define SOCK_PATH "/tmp/swtfb.ipc"

/* mxcfb (i.MX / reMarkable) — enough of the ABI to read an update request. */
typedef struct { uint32_t top, left, width, height; } mxcfb_rect;
typedef struct {
    mxcfb_rect update_region;
    uint32_t waveform_mode;
    uint32_t update_mode;
    uint32_t update_marker;
    int temp;
    unsigned int flags;
    int dither_mode;
    int quant_bit;
    /* alt_buffer_data tail intentionally omitted — we only read the head */
} mxcfb_update_data;
#define MXCFB_SEND_UPDATE _IOW('F', 0x2E, mxcfb_update_data)
#define MXCFB_WAIT_FOR_UPDATE_COMPLETE _IOWR('F', 0x2F, uint32_t)

/* einkbridge wire protocol (matches einkbridge.cpp) */
typedef struct { uint32_t top, left, width, height; } swtfb_rect;
typedef struct { swtfb_rect region; uint32_t waveform; uint32_t flags; } swtfb_update;

/* mxcfb waveform_mode ids (reMarkable/i.MX): 1=DU, 6=A2 are fast 2-level. */
#define WAVEFORM_MODE_DU 1
#define WAVEFORM_MODE_A2 6

static int (*real_ioctl)(int, unsigned long, ...);
static int (*real_open)(const char *, int, ...);
static int (*real_open64)(const char *, int, ...);
static int (*real_openat)(int, const char *, int, ...);

static int g_fbfd = -1; /* the fd we handed out for /dev/fb0 (a real /dev/shm/swtfb) */
static int g_sock = -1;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

__attribute__((constructor)) static void init_reals(void) {
    real_ioctl = dlsym(RTLD_NEXT, "ioctl");
    real_open = dlsym(RTLD_NEXT, "open");
    real_open64 = dlsym(RTLD_NEXT, "open64");
    real_openat = dlsym(RTLD_NEXT, "openat");
}

static int ensure_sock_locked(void) {
    if (g_sock >= 0) return g_sock;
    int s = socket(AF_UNIX, SOCK_STREAM, 0);
    if (s < 0) return -1;
    struct sockaddr_un a = {.sun_family = AF_UNIX};
    strncpy(a.sun_path, SOCK_PATH, sizeof(a.sun_path) - 1);
    if (connect(s, (struct sockaddr *)&a, sizeof(a)) < 0) {
        close(s);
        return -1;
    }
    g_sock = s;
    return s;
}

static int is_fb(const char *p) { return p && strcmp(p, FB_PATH) == 0; }

/* Redirect a /dev/fb0 open to the einkbridge shared buffer. */
static int fb_open(const char *real_path_ignored) {
    (void)real_path_ignored;
    int fd = real_open(SHM_PATH, O_RDWR, 0);
    if (fd >= 0) {
        pthread_mutex_lock(&g_lock);
        g_fbfd = fd;
        pthread_mutex_unlock(&g_lock);
    }
    return fd;
}

int open(const char *path, int flags, ...) {
    if (!real_open) init_reals();
    mode_t mode = 0;
    if (flags & O_CREAT) {
        va_list ap;
        va_start(ap, flags);
        mode = va_arg(ap, mode_t);
        va_end(ap);
    }
    if (is_fb(path)) return fb_open(path);
    return real_open(path, flags, mode);
}

int open64(const char *path, int flags, ...) {
    if (!real_open64) init_reals();
    mode_t mode = 0;
    if (flags & O_CREAT) {
        va_list ap;
        va_start(ap, flags);
        mode = va_arg(ap, mode_t);
        va_end(ap);
    }
    if (is_fb(path)) return fb_open(path);
    return real_open64(path, flags, mode);
}

int openat(int dirfd, const char *path, int flags, ...) {
    if (!real_openat) init_reals();
    mode_t mode = 0;
    if (flags & O_CREAT) {
        va_list ap;
        va_start(ap, flags);
        mode = va_arg(ap, mode_t);
        va_end(ap);
    }
    if (is_fb(path)) return fb_open(path);
    return real_openat(dirfd, path, flags, mode);
}

int ioctl(int fd, unsigned long req, ...) {
    if (!real_ioctl) init_reals();
    va_list ap;
    va_start(ap, req);
    void *arg = va_arg(ap, void *);
    va_end(ap);

    int is_fb_fd;
    pthread_mutex_lock(&g_lock);
    is_fb_fd = (g_fbfd >= 0 && fd == g_fbfd);
    pthread_mutex_unlock(&g_lock);
    if (!is_fb_fd) return real_ioctl(fd, req, arg);

    switch (req) {
    case FBIOGET_VSCREENINFO: {
        struct fb_var_screeninfo *v = arg;
        memset(v, 0, sizeof(*v));
        v->xres = FB_W;
        v->yres = FB_H;
        v->xres_virtual = FB_W;
        v->yres_virtual = FB_H; /* single buffer — mmap size == shm size */
        v->bits_per_pixel = 16;
        v->grayscale = 0;
        v->red = (struct fb_bitfield){11, 5, 0};
        v->green = (struct fb_bitfield){5, 6, 0};
        v->blue = (struct fb_bitfield){0, 5, 0};
        return 0;
    }
    case FBIOGET_FSCREENINFO: {
        struct fb_fix_screeninfo *f = arg;
        memset(f, 0, sizeof(*f));
        strncpy(f->id, "swtfb", sizeof(f->id) - 1);
        f->smem_len = FB_W * FB_H * 2;
        f->line_length = FB_W * 2;
        f->type = FB_TYPE_PACKED_PIXELS;
        f->visual = FB_VISUAL_TRUECOLOR;
        return 0;
    }
    case FBIOPUT_VSCREENINFO:
        return 0; /* accept mode sets (we are fixed 954x1696 RGB565) */
    default:
        /* mxcfb ioctls: match by type 'F' + NUMBER, IGNORING the encoded struct
           size. reMarkable's mxcfb_update_data is 72 B (it carries alt_buffer_data),
           so its MXCFB_SEND_UPDATE (0x4048462E) differs from a 44-B struct's
           (0x402C462E) — an exact-value case silently misses it. But update_region
           + waveform_mode sit at the same head offsets regardless of the tail, so
           dispatch on the ioctl number (0x2E) and read the head. */
        if (((req >> 8) & 0xFF) == 'F') {
            if ((req & 0xFF) == 0x2E) { /* MXCFB_SEND_UPDATE, any struct size */
                mxcfb_update_data *u = arg;
                swtfb_update s;
                s.region.top = u->update_region.top;
                s.region.left = u->update_region.left;
                s.region.width = u->update_region.width;
                s.region.height = u->update_region.height;
                s.waveform = (u->waveform_mode == WAVEFORM_MODE_DU ||
                              u->waveform_mode == WAVEFORM_MODE_A2)
                                 ? 1  /* DU — fast 2-level */
                                 : 2; /* GC16 — grayscale/color */
                s.flags = 0;
                pthread_mutex_lock(&g_lock);
                /* MSG_NOSIGNAL: a dead peer must surface as EPIPE, not a
                   SIGPIPE that kills the client. */
                int sk = ensure_sock_locked();
                if (sk >= 0) {
                    ssize_t n = send(sk, &s, sizeof(s), MSG_NOSIGNAL);
                    if (n != (ssize_t)sizeof(s)) {
                        /* Bridge went away (restart/EPIPE): drop the dead
                           connection and retry once, so a long-lived client
                           reattaches instead of painting into the void. */
                        close(g_sock);
                        g_sock = -1;
                        sk = ensure_sock_locked();
                        if (sk >= 0) {
                            n = send(sk, &s, sizeof(s), MSG_NOSIGNAL);
                            (void)n;
                        }
                    }
                }
                pthread_mutex_unlock(&g_lock);
            }
            /* SEND_UPDATE done, or WAIT_FOR_UPDATE_COMPLETE (0x2F) / other mxcfb 'F'
               ioctls (temp, auto-update, etc.): report success as a no-op. */
            return 0;
        }
        return real_ioctl(fd, req, arg);
    }
}
