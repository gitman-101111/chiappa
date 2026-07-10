/*
 * vkms_test.c — test compositor for the vkms_bridge pipeline
 *
 * Pushes a four-quadrant grayscale test pattern into the VKMS primary plane.
 * vkms_bridge (running separately, holding DRM master) captures this via the
 * writeback connector and forwards it to einkbridge → panel.
 *
 * Run order:
 *   rc-service chiappa-eink start    # starts einkbridge
 *   rc-service chiappa-vkms start    # loads vkms, starts vkms_bridge (takes master)
 *   vkms_test                        # updates primary plane → appears on panel
 *
 * Architecture note:
 *   vkms_bridge holds DRM master for the lifetime of the bridge process.
 *   vkms_test cannot acquire master (bridge already has it).  Instead, it
 *   opens /dev/dri/card1 WITHOUT taking master and uses atomic commits to
 *   update only the primary PLANE's FB_ID.  The CRTC and writeback connector
 *   are already configured by vkms_bridge's setup phase.
 *
 *   The plane FB update does NOT trigger the DRM encoder clone check (only
 *   connector CRTC_ID changes trigger it), so this is safe to do without
 *   master on a CRTC that has an active writeback connector.
 *
 *   For DRM_MODE_ATOMIC_NONBLOCK, master IS required (returns EACCES without
 *   it).  Use blocking commits (no NONBLOCK flag) instead — they work without
 *   master for plane-only changes.
 *
 * Build (on device, musl):
 *   gcc -O2 -o vkms_test vkms_test.c -ldrm -I/usr/include/libdrm
 */

#include <xf86drm.h>
#include <xf86drmMode.h>

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#define log(fmt, ...) fprintf(stderr, "[vkms_test] " fmt "\n", ##__VA_ARGS__)

static uint32_t find_prop(int fd, uint32_t obj_id, uint32_t obj_type, const char *name)
{
    drmModeObjectProperties *p = drmModeObjectGetProperties(fd, obj_id, obj_type);
    if (!p) return 0;
    uint32_t id = 0;
    for (uint32_t i = 0; i < p->count_props && !id; i++) {
        drmModePropertyRes *r = drmModeGetProperty(fd, p->props[i]);
        if (r && !strcmp(r->name, name)) id = r->prop_id;
        if (r) drmModeFreeProperty(r);
    }
    drmModeFreeObjectProperties(p);
    return id;
}

static void fill_pattern(uint32_t *buf, uint32_t w, uint32_t h, uint32_t pitch_px)
{
    uint32_t cx = w / 2, cy = h / 2;
    for (uint32_t y = 0; y < h; y++) {
        for (uint32_t x = 0; x < w; x++) {
            uint32_t px;
            if ((x >= cx - 2 && x < cx + 2) || (y >= cy - 2 && y < cy + 2))
                px = 0x00000000;
            else if (x < cx && y < cy)   px = 0x00FFFFFF;
            else if (x >= cx && y < cy)  px = 0x00AAAAAA;
            else if (x < cx)             px = 0x00555555;
            else                         px = 0x00000000;
            buf[y * pitch_px + x] = px;
        }
    }
}

int main(int argc, char **argv)
{
    const char *dev = (argc > 1) ? argv[1] : "/dev/dri/card1";

    int fd = open(dev, O_RDWR | O_CLOEXEC);
    if (fd < 0) { perror(dev); return 1; }

    drmSetClientCap(fd, DRM_CLIENT_CAP_ATOMIC, 1);
    drmSetClientCap(fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1);
    /* Do NOT take DRM master — vkms_bridge already holds it */

    drmModeRes *res = drmModeGetResources(fd);
    if (!res || res->count_crtcs == 0) { log("no resources on %s", dev); return 1; }
    uint32_t crtc_id = res->crtcs[0];

    /* Get current CRTC mode to know the FB dimensions */
    drmModeCrtc *crtc = drmModeGetCrtc(fd, crtc_id);
    if (!crtc || !crtc->mode_valid) {
        log("CRTC %u has no active mode — is vkms_bridge running?", crtc_id);
        return 1;
    }
    uint32_t w = crtc->mode.hdisplay, h = crtc->mode.vdisplay;
    drmModeFreeCrtc(crtc);
    drmModeFreeResources(res);

    /* Find primary plane */
    uint32_t plane_id = 0;
    drmModePlaneRes *pres = drmModeGetPlaneResources(fd);
    for (uint32_t i = 0; i < (pres ? pres->count_planes : 0) && !plane_id; i++) {
        drmModePlane *pl = drmModeGetPlane(fd, pres->planes[i]);
        if (!pl) continue;
        if (pl->possible_crtcs & 1) {
            drmModeObjectProperties *pp = drmModeObjectGetProperties(
                fd, pl->plane_id, DRM_MODE_OBJECT_PLANE);
            for (uint32_t j = 0; j < (pp ? pp->count_props : 0); j++) {
                drmModePropertyRes *r = drmModeGetProperty(fd, pp->props[j]);
                if (r && !strcmp(r->name, "type") &&
                    pp->prop_values[j] == DRM_PLANE_TYPE_PRIMARY)
                    plane_id = pl->plane_id;
                drmModeFreeProperty(r);
            }
            drmModeFreeObjectProperties(pp);
        }
        drmModeFreePlane(pl);
    }
    drmModeFreePlaneResources(pres);
    if (!plane_id) { log("no primary plane"); return 1; }

    /* Allocate + fill FB */
    struct drm_mode_create_dumb cd = { .width = w, .height = h, .bpp = 32 };
    if (ioctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &cd)) { perror("create_dumb"); return 1; }
    uint32_t fb_id = 0;
    if (drmModeAddFB(fd, w, h, 24, 32, cd.pitch, cd.handle, &fb_id) < 0) {
        perror("drmModeAddFB"); return 1;
    }
    struct drm_mode_map_dumb md = { .handle = cd.handle };
    ioctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &md);
    uint32_t *buf = mmap(NULL, cd.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, md.offset);
    if (buf == (void *)-1) { perror("mmap"); return 1; }
    fill_pattern(buf, w, h, cd.pitch / 4);
    log("pattern filled: %ux%u fb=%u", w, h, fb_id);

    /* Atomic commit: update primary plane FB only.
     * Blocking (no NONBLOCK) — works without DRM master for plane-only changes. */
    uint32_t pl_fb_id = find_prop(fd, plane_id, DRM_MODE_OBJECT_PLANE, "FB_ID");
    if (!pl_fb_id) { log("FB_ID property not found"); return 1; }

    drmModeAtomicReq *req = drmModeAtomicAlloc();
    drmModeAtomicAddProperty(req, plane_id, pl_fb_id, fb_id);
    int ret = drmModeAtomicCommit(fd, req, 0, NULL);
    drmModeAtomicFree(req);

    if (ret < 0) {
        log("atomic commit failed: %s", strerror(errno));
        log("hint: ensure vkms_bridge is running and has set up the plane geometry");
        return 1;
    }
    log("plane %u → fb %u; vkms_bridge should forward to panel", plane_id, fb_id);
    log("Ctrl+C to stop");

    for (;;) sleep(60);
}
