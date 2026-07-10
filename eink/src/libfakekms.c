/*
 * libfakekms.c — LD_PRELOAD shim for DRM lease injection
 *
 * Makes any Wayland compositor or X server use the VKMS DRM lease provided
 * by vkms_bridge instead of trying to acquire global DRM master on card1
 * (which vkms_bridge holds permanently for writeback captures).
 *
 * What it intercepts:
 *   open/open64/openat for "/dev/dri/card1"
 *     → connects to /tmp/vkms-lease.sock once, receives lease fd via SCM_RIGHTS,
 *       returns dup(lease_fd) for every subsequent open of the same path.
 *
 *   libseat_open_device(seat, "/dev/dri/card1", &fd)
 *     → wlroots 0.20 opens DRM devices via libseat (seatd), NOT via open().
 *       We intercept libseat_open_device so the seat-provided fd (which has
 *       no master/lease permissions because the bridge holds master) is replaced
 *       with our lease fd.  The real libseat_open_device is called first so
 *       seatd registers the session normally; we then substitute the fd.
 *
 *   ioctl DRM_IOCTL_SET_MASTER / DRM_IOCTL_DROP_MASTER
 *     → returns 0 (lease fd is already effectively master for leased objects;
 *       the kernel's DRM_IOCTL_SET_MASTER returns EACCES for lessees because
 *       the lessor holds the real master slot, but lessees CAN do atomic commits).
 *       DROP_MASTER is suppressed to prevent early lease revocation.
 *
 * Usage (any compositor, no recompile needed):
 *   WLR_DRM_DEVICES=/dev/dri/card1 LD_PRELOAD=/usr/lib/libfakekms.so sway
 *   WLR_DRM_DEVICES=/dev/dri/card1 LD_PRELOAD=/usr/lib/libfakekms.so gnome-shell --wayland
 *   WLR_DRM_DEVICES=/dev/dri/card1 LD_PRELOAD=/usr/lib/libfakekms.so startplasma-wayland
 *
 * Prerequisites: vkms_bridge must be running (holds DRM master + lease socket).
 *
 * Build (device, musl):
 *   gcc -O2 -shared -fPIC -o libfakekms.so libfakekms.c -ldl -lpthread \
 *       -I/usr/include/libdrm
 */

#define _GNU_SOURCE
#include <drm/drm.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#define LEASE_SOCK  "/tmp/vkms-lease.sock"
#define TARGET_DEV  "/dev/dri/card1"

/* Forward-declare the libseat opaque type so we can intercept without
 * needing the full libseat headers. */
struct libseat;


static pthread_once_t  g_once  = PTHREAD_ONCE_INIT;
static int             g_lease = -1; /* master lease fd (dup'd for each open) */
static int             g_sock  = -1; /* socket kept open to hold the lease   */

static void acquire_lease(void)
{
    g_sock = socket(AF_UNIX, SOCK_STREAM, 0);
    if (g_sock < 0) { perror("[fakekms] socket"); return; }

    struct sockaddr_un addr = { .sun_family = AF_UNIX };
    strncpy(addr.sun_path, LEASE_SOCK, sizeof(addr.sun_path) - 1);
    if (connect(g_sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        fprintf(stderr, "[fakekms] connect %s: %s\n", LEASE_SOCK, strerror(errno));
        close(g_sock); g_sock = -1; return;
    }

    char buf[1];
    struct iovec iov = { buf, 1 };
    char ctrl[CMSG_SPACE(sizeof(int))];
    memset(ctrl, 0, sizeof(ctrl));
    struct msghdr msg = {
        .msg_iov        = &iov,
        .msg_iovlen     = 1,
        .msg_control    = ctrl,
        .msg_controllen = sizeof(ctrl),
    };
    if (recvmsg(g_sock, &msg, 0) < 0) {
        fprintf(stderr, "[fakekms] recvmsg: %s\n", strerror(errno));
        close(g_sock); g_sock = -1; return;
    }
    struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
    if (!cmsg || cmsg->cmsg_type != SCM_RIGHTS) {
        fprintf(stderr, "[fakekms] no SCM_RIGHTS in reply\n");
        close(g_sock); g_sock = -1; return;
    }
    memcpy(&g_lease, CMSG_DATA(cmsg), sizeof(int));
    fprintf(stderr, "[fakekms] lease fd=%d acquired; %s intercepted → panel\n",
            g_lease, TARGET_DEV);
}

/* Returns a dup'd lease fd if path == TARGET_DEV, otherwise -1. */
static int maybe_lease_fd(const char *path)
{
    if (!path || strcmp(path, TARGET_DEV) != 0) return -1;
    pthread_once(&g_once, acquire_lease);
    if (g_lease < 0) return -1;
    int fd = dup(g_lease);
    fprintf(stderr, "[fakekms] open(%s) → lease dup=%d\n", path, fd);
    return fd;
}

/* ── syscall intercepts ─────────────────────────────────────────────── */

int open(const char *path, int flags, ...)
{
    int lfd = maybe_lease_fd(path);
    if (lfd >= 0) return lfd;

    static int (*real)(const char *, int, ...) = NULL;
    if (!real) real = dlsym(RTLD_NEXT, "open");
    va_list ap; va_start(ap, flags);
    mode_t mode = va_arg(ap, mode_t); va_end(ap);
    return real(path, flags, mode);
}

int open64(const char *path, int flags, ...)
{
    int lfd = maybe_lease_fd(path);
    if (lfd >= 0) return lfd;

    static int (*real)(const char *, int, ...) = NULL;
    if (!real) real = dlsym(RTLD_NEXT, "open64");
    va_list ap; va_start(ap, flags);
    mode_t mode = va_arg(ap, mode_t); va_end(ap);
    return real ? real(path, flags, mode) : open(path, flags, mode);
}

int openat(int dirfd, const char *path, int flags, ...)
{
    int lfd = maybe_lease_fd(path);
    if (lfd >= 0) return lfd;

    static int (*real)(int, const char *, int, ...) = NULL;
    if (!real) real = dlsym(RTLD_NEXT, "openat");
    va_list ap; va_start(ap, flags);
    mode_t mode = va_arg(ap, mode_t); va_end(ap);
    return real(dirfd, path, flags, mode);
}

int ioctl(int fd, int req, ...)
{
    static int (*real)(int, unsigned long, ...) = NULL;
    if (!real) real = dlsym(RTLD_NEXT, "ioctl");

    /*
     * Suppress SET_MASTER and DROP_MASTER when a lease is active.
     * The lease fd already has effective master for its leased objects;
     * the kernel's SET_MASTER ioctl returns EACCES for lessees (the real
     * master slot belongs to the lessor) and would cause compositors to abort.
     * DROP_MASTER is suppressed to prevent early lease revocation.
     */
    if (g_lease >= 0 &&
        ((unsigned long)req == DRM_IOCTL_SET_MASTER ||
         (unsigned long)req == DRM_IOCTL_DROP_MASTER)) {
        fprintf(stderr, "[fakekms] suppressed ioctl %s\n",
                (unsigned long)req == DRM_IOCTL_SET_MASTER ? "SET_MASTER" : "DROP_MASTER");
        return 0;
    }

    va_list ap; va_start(ap, req);
    void *arg = va_arg(ap, void *); va_end(ap);
    return real(fd, req, arg);
}

/*
 * libseat_open_device — intercept wlroots' seat-based DRM device open.
 *
 * wlroots 0.20 calls libseat_open_device(seat, "/dev/dri/card1", &fd) to
 * obtain its primary DRM fd.  seatd gives it a fresh fd (no master, no
 * lease) because the bridge holds DRM master permanently.  That non-master fd
 * causes EACCES on any ALLOW_MODESET atomic commit.
 *
 * Fix: call the real libseat_open_device first (so seatd records the device
 * open in the session), then replace the returned fd with dup(g_lease).
 * Close the seatd-provided fd so the kernel doesn't keep an extra reference.
 * The libseat handle (return value) is kept so libseat_close_device works.
 */
int libseat_open_device(struct libseat *seat, const char *path, int *fd)
{
    typedef int (*real_t)(struct libseat *, const char *, int *);
    static real_t real = NULL;
    if (!real) real = (real_t)dlsym(RTLD_NEXT, "libseat_open_device");

    int handle = real(seat, path, fd);

    if (handle >= 0 && path && strcmp(path, TARGET_DEV) == 0) {
        pthread_once(&g_once, acquire_lease);
        if (g_lease >= 0) {
            /* Replace seatd's non-master fd with the lease fd */
            int old_fd = *fd;
            int lease_dup = dup(g_lease);
            if (lease_dup >= 0) {
                *fd = lease_dup;
                close(old_fd);
                fprintf(stderr,
                        "[fakekms] libseat_open_device(%s): replaced fd %d → lease dup %d\n",
                        path, old_fd, lease_dup);
            } else {
                fprintf(stderr, "[fakekms] libseat_open_device: dup failed: %s\n",
                        strerror(errno));
            }
        }
    }
    return handle;
}
