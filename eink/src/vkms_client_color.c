/*
 * vkms_client_color.c — color waveform tester for the vkms_bridge pipeline
 *
 * Receives a DRM lease from vkms_bridge, fills the primary plane with saturated
 * primary-color quadrants, then sends the rm2fb IPC update with a selectable
 * waveform mode.  Run with different --waveform values to find which renders
 * correct color on the Gallery 3 panel.
 *
 * Quadrant layout:
 *   TL=Red   TR=Green
 *   BL=Blue  BR=White
 *
 * Usage:
 *   vkms_client_color [--waveform N]   (default N=2, GC16)
 *
 * Common waveform IDs (Gallery 3 may add more above 6):
 *   0  INIT    full flash, black→white
 *   1  DU      1-bit direct update (fastest, no gray)
 *   2  GC16    16-gray, high quality (default on rm2fb)
 *   3  GL16    gray lineup 16
 *   4  GLR16   gray lineup 16 with reduction
 *   5  GLD16   gray lineup 16 differential
 *   6  GLRC16  gray lineup 16 + color (Gallery/Kaleido color mode)
 *   7  DU4     4-level direct update
 *
 * Build (on device, musl):
 *   gcc -O2 -o vkms_client_color vkms_client_color.c -ldrm -I/usr/include/libdrm
 */

#include <xf86drm.h>
#include <xf86drmMode.h>

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

#define LEASE_SOCK "/tmp/vkms-lease.sock"
#define IPC_PATH   "/tmp/swtfb.ipc"
#define PANEL_W    954
#define PANEL_H    1696

#define log(fmt, ...) fprintf(stderr, "[color_client] " fmt "\n", ##__VA_ARGS__)

/* ── rm2fb IPC ───────────────────────────────────────────────────────── */

typedef struct { uint32_t top, left, width, height; } swtfb_rect_t;
typedef struct { swtfb_rect_t region; uint32_t waveform; uint32_t flags; } swtfb_update_t;

static void send_update(uint32_t waveform)
{
    int s = socket(AF_UNIX, SOCK_STREAM, 0);
    if (s < 0) return;
    struct sockaddr_un a = { .sun_family = AF_UNIX };
    strncpy(a.sun_path, IPC_PATH, sizeof(a.sun_path) - 1);
    if (connect(s, (struct sockaddr *)&a, sizeof(a)) == 0) {
        swtfb_update_t m = { { 0, 0, PANEL_W, PANEL_H }, waveform, 0 };
        write(s, &m, sizeof(m));
        log("sent IPC update: waveform=%u", waveform);
    } else {
        log("IPC connect failed: %s", strerror(errno));
    }
    close(s);
}

/* ── DRM helpers ─────────────────────────────────────────────────────── */

static uint32_t find_prop(int fd, uint32_t obj_id, uint32_t obj_type, const char *name)
{
    drmModeObjectProperties *p = drmModeObjectGetProperties(fd, obj_id, obj_type);
    if (!p) return 0;
    uint32_t id = 0;
    for (uint32_t i = 0; i < p->count_props && !id; i++) {
        drmModePropertyRes *r = drmModeGetProperty(fd, p->props[i]);
        if (r && !strcmp(r->name, name)) id = r->prop_id;
        drmModeFreeProperty(r);
    }
    drmModeFreeObjectProperties(p);
    return id;
}

/* XRGB8888 color quadrants:
 *   TL=Red   TR=Green
 *   BL=Blue  BR=White
 * Cross-hair at center to confirm scaling alignment. */
static void fill_color(uint32_t *buf, uint32_t w, uint32_t h, uint32_t pitch_px)
{
    uint32_t cx = w / 2, cy = h / 2;
    for (uint32_t y = 0; y < h; y++) {
        for (uint32_t x = 0; x < w; x++) {
            uint32_t px;
            if ((x >= cx - 2 && x < cx + 2) || (y >= cy - 2 && y < cy + 2))
                px = 0x00000000; /* black cross-hair */
            else if (x < cx && y < cy)  px = 0x00FF0000; /* Red   TL */
            else if (x >= cx && y < cy) px = 0x0000FF00; /* Green TR */
            else if (x < cx)            px = 0x000000FF; /* Blue  BL */
            else                        px = 0x00FFFFFF; /* White BR */
            buf[y * pitch_px + x] = px;
        }
    }
}

/* ── lease receive ───────────────────────────────────────────────────── */

static int recv_lease_fd(int *sock_out)
{
    int sock = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sock < 0) { perror("socket"); return -1; }

    struct sockaddr_un addr = { .sun_family = AF_UNIX };
    strncpy(addr.sun_path, LEASE_SOCK, sizeof(addr.sun_path) - 1);
    if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        log("connect %s: %s — is vkms_bridge running?", LEASE_SOCK, strerror(errno));
        close(sock); return -1;
    }

    char buf[1];
    struct iovec iov = { buf, 1 };
    char ctrl[CMSG_SPACE(sizeof(int))];
    memset(ctrl, 0, sizeof(ctrl));
    struct msghdr msg = {
        .msg_iov = &iov, .msg_iovlen = 1,
        .msg_control = ctrl, .msg_controllen = sizeof(ctrl)
    };
    if (recvmsg(sock, &msg, 0) < 0) { perror("recvmsg"); close(sock); return -1; }

    struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
    if (!cmsg || cmsg->cmsg_type != SCM_RIGHTS) {
        log("no SCM_RIGHTS in reply"); close(sock); return -1;
    }
    int lease_fd;
    memcpy(&lease_fd, CMSG_DATA(cmsg), sizeof(int));
    *sock_out = sock;
    return lease_fd;
}

/* ── main ────────────────────────────────────────────────────────────── */

int main(int argc, char **argv)
{
    uint32_t waveform = 2; /* GC16 default */
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--waveform") && i + 1 < argc)
            waveform = (uint32_t)atoi(argv[++i]);
    }
    log("waveform mode: %u", waveform);

    int sock = -1;
    int lease_fd = recv_lease_fd(&sock);
    if (lease_fd < 0) return 1;

    drmSetClientCap(lease_fd, DRM_CLIENT_CAP_ATOMIC, 1);
    drmSetClientCap(lease_fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1);

    drmModeRes *res = drmModeGetResources(lease_fd);
    if (!res || res->count_crtcs == 0) { log("no CRTC in lease"); return 1; }
    uint32_t crtc_id = res->crtcs[0];

    drmModeCrtc *crtc = drmModeGetCrtc(lease_fd, crtc_id);
    if (!crtc || !crtc->mode_valid) { log("no active mode on CRTC"); return 1; }
    uint32_t w = crtc->mode.hdisplay, h = crtc->mode.vdisplay;
    drmModeFreeCrtc(crtc);
    drmModeFreeResources(res);
    log("mode: %ux%u", w, h);

    uint32_t plane_id = 0;
    drmModePlaneRes *pres = drmModeGetPlaneResources(lease_fd);
    for (uint32_t i = 0; i < (pres ? pres->count_planes : 0) && !plane_id; i++) {
        drmModePlane *pl = drmModeGetPlane(lease_fd, pres->planes[i]);
        if (!pl) continue;
        drmModeObjectProperties *pp = drmModeObjectGetProperties(
            lease_fd, pl->plane_id, DRM_MODE_OBJECT_PLANE);
        for (uint32_t j = 0; j < (pp ? pp->count_props : 0); j++) {
            drmModePropertyRes *r = drmModeGetProperty(lease_fd, pp->props[j]);
            if (r && !strcmp(r->name, "type") &&
                pp->prop_values[j] == DRM_PLANE_TYPE_PRIMARY)
                plane_id = pl->plane_id;
            drmModeFreeProperty(r);
        }
        drmModeFreeObjectProperties(pp);
        drmModeFreePlane(pl);
    }
    drmModeFreePlaneResources(pres);
    if (!plane_id) { log("no primary plane in lease"); return 1; }

    struct drm_mode_create_dumb cd = { .width = w, .height = h, .bpp = 32 };
    if (drmIoctl(lease_fd, DRM_IOCTL_MODE_CREATE_DUMB, &cd)) { perror("create_dumb"); return 1; }
    uint32_t fb_id = 0;
    if (drmModeAddFB(lease_fd, w, h, 24, 32, cd.pitch, cd.handle, &fb_id) < 0) {
        perror("drmModeAddFB"); return 1;
    }
    struct drm_mode_map_dumb md = { .handle = cd.handle };
    drmIoctl(lease_fd, DRM_IOCTL_MODE_MAP_DUMB, &md);
    uint32_t *buf = mmap(NULL, cd.size, PROT_READ | PROT_WRITE, MAP_SHARED, lease_fd, md.offset);
    if (buf == MAP_FAILED) { perror("mmap"); return 1; }

    fill_color(buf, w, h, cd.pitch / 4);
    log("fb %u filled: TL=Red TR=Green BL=Blue BR=White", fb_id);

    uint32_t pl_fb_id   = find_prop(lease_fd, plane_id, DRM_MODE_OBJECT_PLANE, "FB_ID");
    uint32_t pl_crtc_id = find_prop(lease_fd, plane_id, DRM_MODE_OBJECT_PLANE, "CRTC_ID");
    uint32_t pl_crtc_w  = find_prop(lease_fd, plane_id, DRM_MODE_OBJECT_PLANE, "CRTC_W");
    uint32_t pl_crtc_h  = find_prop(lease_fd, plane_id, DRM_MODE_OBJECT_PLANE, "CRTC_H");
    uint32_t pl_src_w   = find_prop(lease_fd, plane_id, DRM_MODE_OBJECT_PLANE, "SRC_W");
    uint32_t pl_src_h   = find_prop(lease_fd, plane_id, DRM_MODE_OBJECT_PLANE, "SRC_H");

    drmModeAtomicReq *req = drmModeAtomicAlloc();
    drmModeAtomicAddProperty(req, plane_id, pl_fb_id,   fb_id);
    drmModeAtomicAddProperty(req, plane_id, pl_crtc_id, crtc_id);
    drmModeAtomicAddProperty(req, plane_id, pl_crtc_w,  w);
    drmModeAtomicAddProperty(req, plane_id, pl_crtc_h,  h);
    drmModeAtomicAddProperty(req, plane_id, pl_src_w,   (uint64_t)w << 16);
    drmModeAtomicAddProperty(req, plane_id, pl_src_h,   (uint64_t)h << 16);
    int ret = drmModeAtomicCommit(lease_fd, req, DRM_MODE_ATOMIC_ALLOW_MODESET, NULL);
    drmModeAtomicFree(req);

    if (ret < 0) { log("atomic commit failed: %s", strerror(errno)); return 1; }
    log("plane committed; sending IPC waveform=%u", waveform);

    /* Wait one capture cycle so bridge has the frame before we notify */
    usleep(150000);
    send_update(waveform);

    log("done — Ctrl+C to release lease");
    for (;;) sleep(60);
}
