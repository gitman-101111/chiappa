/*
 * vkms_client.c — DRM lease client for the vkms_bridge pipeline
 *
 * Receives a DRM lease fd from vkms_bridge via /tmp/vkms-lease.sock (SCM_RIGHTS)
 * and uses it to drive the VKMS primary plane with arbitrary content.
 * vkms_bridge captures the output via the writeback connector and pushes it to
 * the e-ink panel through einkbridge.
 *
 * The lease fd has is_master=yes for [crtc, primary_conn, plane] — the client
 * can commit plane FB_ID changes with DRM_MODE_ATOMIC_ALLOW_MODESET without
 * needing the global DRM master (which vkms_bridge holds permanently).
 *
 * Usage:
 *   # Terminal 1: rc-service chiappa-eink start && rc-service chiappa-vkms start
 *   # Terminal 2: vkms_client [--invert]
 *
 * --invert: fills the inverted 4-quadrant pattern (black TL, dark-gray TR,
 *           light-gray BL, white BR) to visually distinguish client from bridge.
 *
 * Build (on device, musl):
 *   gcc -O2 -o vkms_client vkms_client.c -ldrm -I/usr/include/libdrm
 */

#include <xf86drm.h>
#include <xf86drmMode.h>

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#define LEASE_SOCK "/tmp/vkms-lease.sock"
#define log(fmt, ...) fprintf(stderr, "[vkms_client] " fmt "\n", ##__VA_ARGS__)

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

static void fill_pattern(uint32_t *buf, uint32_t w, uint32_t h,
                          uint32_t pitch_px, bool invert)
{
    uint32_t cx = w / 2, cy = h / 2;
    for (uint32_t y = 0; y < h; y++) {
        for (uint32_t x = 0; x < w; x++) {
            uint32_t px;
            if ((x >= cx - 2 && x < cx + 2) || (y >= cy - 2 && y < cy + 2)) {
                px = invert ? 0x00FFFFFF : 0x00000000;
            } else if (x < cx && y < cy)  px = invert ? 0x00000000 : 0x00FFFFFF;
            else if (x >= cx && y < cy)   px = invert ? 0x00555555 : 0x00AAAAAA;
            else if (x < cx)              px = invert ? 0x00AAAAAA : 0x00555555;
            else                          px = invert ? 0x00FFFFFF : 0x00000000;
            buf[y * pitch_px + x] = px;
        }
    }
}

static int recv_lease_fd(void)
{
    int sock = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sock < 0) { perror("socket"); return -1; }

    struct sockaddr_un addr = { .sun_family = AF_UNIX };
    strncpy(addr.sun_path, LEASE_SOCK, sizeof(addr.sun_path) - 1);
    if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        log("connect %s: %s — is vkms_bridge running?", LEASE_SOCK, strerror(errno));
        close(sock);
        return -1;
    }

    char buf[1];
    struct iovec iov = { buf, 1 };
    char ctrl[CMSG_SPACE(sizeof(int))];
    memset(ctrl, 0, sizeof(ctrl));
    struct msghdr msg = {
        .msg_iov = &iov, .msg_iovlen = 1,
        .msg_control = ctrl, .msg_controllen = sizeof(ctrl)
    };
    ssize_t n = recvmsg(sock, &msg, 0);
    if (n < 0) { perror("recvmsg"); close(sock); return -1; }

    struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
    if (!cmsg || cmsg->cmsg_type != SCM_RIGHTS) {
        log("expected SCM_RIGHTS in recvmsg"); close(sock); return -1;
    }
    int lease_fd;
    memcpy(&lease_fd, CMSG_DATA(cmsg), sizeof(int));

    /* Keep sock open — vkms_bridge detects our exit via EOF on it */
    log("lease fd received: %d (sock=%d stays open until we exit)", lease_fd, sock);
    return lease_fd;
}

int main(int argc, char **argv)
{
    bool invert = false;
    for (int i = 1; i < argc; i++)
        if (!strcmp(argv[i], "--invert")) invert = true;

    int lease_fd = recv_lease_fd();
    if (lease_fd < 0) return 1;

    drmSetClientCap(lease_fd, DRM_CLIENT_CAP_ATOMIC, 1);
    drmSetClientCap(lease_fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1);

    /* Discover objects via the lease fd — it only sees leased objects */
    drmModeRes *res = drmModeGetResources(lease_fd);
    if (!res || res->count_crtcs == 0) { log("no CRTC in lease"); return 1; }
    uint32_t crtc_id = res->crtcs[0];

    drmModeCrtc *crtc = drmModeGetCrtc(lease_fd, crtc_id);
    if (!crtc || !crtc->mode_valid) {
        log("CRTC %u has no active mode — is vkms_bridge running?", crtc_id);
        return 1;
    }
    uint32_t w = crtc->mode.hdisplay, h = crtc->mode.vdisplay;
    drmModeFreeCrtc(crtc);
    drmModeFreeResources(res);
    log("mode: %ux%u", w, h);

    /* Find primary plane via lease fd */
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
    log("plane: %u", plane_id);

    /* Allocate + fill FB */
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
    fill_pattern(buf, w, h, cd.pitch / 4, invert);
    log("fb %u filled (%s)", fb_id, invert ? "inverted quadrants" : "normal quadrants");

    /* Commit: update primary plane FB via lease fd.
     * DRM_MODE_ATOMIC_ALLOW_MODESET allowed because lease fd has is_master=yes. */
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

    if (ret < 0) {
        log("atomic commit failed: %s", strerror(errno));
        return 1;
    }
    log("plane committed → fb %u; panel should update via vkms_bridge", fb_id);
    log("hold Ctrl+C to stop (releases lease and bridge reverts to its test pattern)");

    for (;;) sleep(60);
}
