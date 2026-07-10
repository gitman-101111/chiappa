/*
 * vkms_bridge.c — VKMS writeback → rm2fb IPC bridge (reMarkable Paper Pro Move)
 *
 * Captures compositor output via the VKMS writeback connector and forwards it
 * to einkbridge via the rm2fb shared-memory IPC protocol (/dev/shm/swtfb).
 *
 * Pipeline:
 *   compositor (lease fd) → primary plane FB → VKMS CRTC compose
 *   → writeback capture FB → nearest-neighbour scale + XRGB8888→RGB565
 *   → /dev/shm/swtfb → swtfb.ipc notify → einkbridge → SWTCON → panel
 *
 * DRM atomic + lease constraints (verified with wb_probe.c on this kernel):
 *
 *   1. A connector cannot be attached to a DISABLED CRTC — attaching the
 *      writeback connector requires the CRTC to be enabled (MODE_ID + ACTIVE)
 *      in the same or an earlier commit.
 *
 *   2. Once a DRM lease containing the CRTC exists, the lessor's master fd
 *      CAN NO LONGER REFERENCE the CRTC in atomic commits — leased objects are
 *      invisible to the lessor (drm_mode_object_find fails → EINVAL).  So the
 *      writeback connector must be attached BEFORE drmModeCreateLease, and
 *      per-frame commits may only touch WRITEBACK_FB_ID on the writeback
 *      connector (which stays un-leased, under bridge control).
 *
 *   3. The compositor's startup commit does NOT conflict with the attached
 *      writeback connector as long as it programs the SAME mode the bridge
 *      set: identical modes → drm_atomic_helper_check_modeset sees no mode
 *      change → drm_atomic_add_affected_connectors is never called → the
 *      (non-leased) writeback connector is never pulled into the lessee's
 *      state.  The bridge therefore picks the PREFERRED mode, exactly like
 *      wlroots does.
 *
 *   4. Per-frame WRITEBACK_FB_ID commits require DRM master (EACCES without);
 *      the lease fd covers the compositor's plane flips.
 *
 * DRM lease architecture:
 *   wb_setup() resets VKMS state, performs a full modeset (primary connector,
 *   primary plane with a dumb FB, CRTC enabled) and attaches the writeback
 *   connector — all while the bridge still owns every object.  Only then does
 *   the lease server create a lease for [crtc, pri_conn, planes] and hand the
 *   lease fd to a compositor via /tmp/vkms-lease.sock (SCM_RIGHTS).  The
 *   compositor drives the primary plane through the lease fd; the bridge
 *   queues writeback captures with FB-only commits on the master fd.
 *
 * Build (on device, musl):
 *   gcc -O2 -o vkms_bridge vkms_bridge.c -ldrm -I/usr/include/libdrm -lpthread
 *
 * Run as root.  Prerequisites: vkms enable_writeback=1, einkbridge running.
 * Connect a compositor via:  vkms_client (provided companion tool)
 */

#include <drm/drm.h>
#include <drm/drm_mode.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#define PANEL_W    954
#define PANEL_H    1696
#define SHM_PATH   "/dev/shm/swtfb"
#define IPC_PATH   "/tmp/swtfb.ipc"
#define LEASE_SOCK "/tmp/vkms-lease.sock"
#define SHM_SIZE   ((size_t)PANEL_W * PANEL_H * 2)
#define POLL_MS    100

#define log(fmt, ...) fprintf(stderr, "[vkms_bridge] " fmt "\n", ##__VA_ARGS__)

/* ── rm2fb IPC ───────────────────────────────────────────────────────── */

typedef struct { uint32_t top, left, width, height; } swtfb_rect_t;
typedef struct { swtfb_rect_t region; uint32_t waveform; uint32_t flags; } swtfb_update_t;

/* rm2fb waveform ids */
#define WF_DU   1   /* fast 2-level — small/incremental updates */
#define WF_GC16 2   /* full-quality grayscale — large changes */

/* Small dirty regions get the fast DU waveform; big ones the full GC16.
 * Threshold: 20% of the panel area. */
static void send_update(swtfb_rect_t rect)
{
    int s = socket(AF_UNIX, SOCK_STREAM, 0);
    if (s < 0) return;
    struct sockaddr_un a = { .sun_family = AF_UNIX };
    strncpy(a.sun_path, IPC_PATH, sizeof(a.sun_path) - 1);
    if (connect(s, (struct sockaddr *)&a, sizeof(a)) == 0) {
        uint64_t area = (uint64_t)rect.width * rect.height;
        uint32_t wf = (area * 5 < (uint64_t)PANEL_W * PANEL_H) ? WF_DU : WF_GC16;
        swtfb_update_t m = { rect, wf, 0 };
        write(s, &m, sizeof(m));
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
        if (r) drmModeFreeProperty(r);
    }
    drmModeFreeObjectProperties(p);
    return id;
}

struct fb_info {
    uint32_t id;
    uint32_t handle;
    uint64_t size;
    uint32_t pitch;
    void    *map;
};

static int alloc_dumb_fb(int fd, uint32_t w, uint32_t h,
                          int map_prot, struct fb_info *out)
{
    struct drm_mode_create_dumb cd = { .width = w, .height = h, .bpp = 32 };
    if (drmIoctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &cd)) { perror("create_dumb"); return -1; }
    out->handle = cd.handle;
    out->size   = cd.size;
    out->pitch  = cd.pitch;
    if (drmModeAddFB(fd, w, h, 24, 32, cd.pitch, cd.handle, &out->id) < 0) {
        perror("drmModeAddFB"); return -1;
    }
    if (map_prot) {
        struct drm_mode_map_dumb md = { .handle = cd.handle };
        if (drmIoctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &md)) { perror("map_dumb"); return -1; }
        out->map = mmap(NULL, cd.size, map_prot, MAP_SHARED, fd, md.offset);
        if (out->map == MAP_FAILED) { perror("mmap"); return -1; }
    }
    return 0;
}

/* ── bridge context ──────────────────────────────────────────────────── */

typedef struct {
    int      fd;
    uint32_t crtc_id;
    uint32_t primary_conn_id;
    uint32_t wb_conn_id;
    uint32_t plane_id;          /* primary plane */
    uint32_t all_plane_ids[16]; /* all planes for the CRTC (for lease) */
    uint32_t n_planes;
    uint32_t mode_w, mode_h;

    struct fb_info capture;     /* writeback destination (bridge reads) */
    struct fb_info scanout;     /* dumb primary-plane FB for the initial modeset */

    uint32_t wb_fb_id_prop;
    uint32_t wb_crtc_id_prop;
    bool     wb_attached;
} wb_ctx_t;

/* ── DRM lease server (background thread) ────────────────────────────── */

/*
 * Serves the DRM lease fd to one client at a time via SCM_RIGHTS on a Unix
 * domain socket.  The client uses the lease fd to drive the primary plane;
 * the bridge uses the master fd to capture via writeback.
 *
 * Lease objects: [crtc, primary_conn, all planes] — primary connector must be
 * included, otherwise drmModeCreateLease returns EINVAL.  The writeback
 * connector is NOT leased; it stays under exclusive bridge control.
 *
 * When the client closes the lease fd, the DRM lease is revoked.  The thread
 * then creates a new lease ready for the next client.
 */

typedef struct {
    int master_fd;
    uint32_t crtc_id, conn_id;
    uint32_t plane_ids[16];
    uint32_t n_planes;
} lease_args_t;

/* Set by lease_server thread while a client holds the lease. */
static volatile int g_lease_client_active = 0;

static void *lease_server(void *arg)
{
    lease_args_t *la = arg;

    int srv = socket(AF_UNIX, SOCK_STREAM, 0);
    if (srv < 0) { perror("lease socket"); return NULL; }

    struct sockaddr_un addr = { .sun_family = AF_UNIX };
    strncpy(addr.sun_path, LEASE_SOCK, sizeof(addr.sun_path) - 1);
    unlink(LEASE_SOCK);
    if (bind(srv, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("lease bind"); return NULL;
    }
    listen(srv, 1);
    log("lease socket: %s — run vkms_client to connect", LEASE_SOCK);

    uint32_t objs[18];
    uint32_t n_objs = 0;
    objs[n_objs++] = la->crtc_id;
    objs[n_objs++] = la->conn_id;
    for (uint32_t i = 0; i < la->n_planes && n_objs < 18; i++)
        objs[n_objs++] = la->plane_ids[i];

    while (1) {
        /* Create a fresh lease (previous was revoked when client closed its fd) */
        uint32_t lessee_id = 0;
        int lease_fd = drmModeCreateLease(la->master_fd, objs, n_objs, O_CLOEXEC, &lessee_id);
        if (lease_fd < 0) {
            log("drmModeCreateLease failed: %s (retrying in 1s)", strerror(errno));
            sleep(1);
            continue;
        }
        log("lease ready (lessee_id=%u) — waiting for client", lessee_id);

        int cli = accept(srv, NULL, NULL);
        if (cli < 0) { close(lease_fd); continue; }

        /* Send lease_fd to client via SCM_RIGHTS */
        char buf[1] = {0};
        struct iovec iov = { buf, 1 };
        char ctrl[CMSG_SPACE(sizeof(int))];
        memset(ctrl, 0, sizeof(ctrl));
        struct msghdr msg = {
            .msg_iov = &iov, .msg_iovlen = 1,
            .msg_control = ctrl, .msg_controllen = sizeof(ctrl)
        };
        struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
        cmsg->cmsg_level = SOL_SOCKET;
        cmsg->cmsg_type  = SCM_RIGHTS;
        cmsg->cmsg_len   = CMSG_LEN(sizeof(int));
        memcpy(CMSG_DATA(cmsg), &lease_fd, sizeof(int));
        msg.msg_controllen = cmsg->cmsg_len;
        sendmsg(cli, &msg, 0);

        /* Bridge gives up its copy — client now holds the only reference */
        close(lease_fd);
        g_lease_client_active = 1;
        log("lease fd sent to client");

        /* Wait until client closes (EOF on socket = client exited) */
        char ch;
        recv(cli, &ch, 1, 0);
        close(cli);
        g_lease_client_active = 0;
        log("client disconnected — lease revoked");
    }
    return NULL;
}

/* ── setup ───────────────────────────────────────────────────────────── */

static int wb_setup(wb_ctx_t *ctx, const char *dev)
{
    ctx->fd = open(dev, O_RDWR | O_CLOEXEC);
    if (ctx->fd < 0) { perror(dev); return -1; }

    drmSetClientCap(ctx->fd, DRM_CLIENT_CAP_ATOMIC, 1);
    drmSetClientCap(ctx->fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1);
    drmSetClientCap(ctx->fd, DRM_CLIENT_CAP_WRITEBACK_CONNECTORS, 1);

    if (drmSetMaster(ctx->fd) < 0) { perror("drmSetMaster"); return -1; }

    drmModeRes *res = drmModeGetResources(ctx->fd);
    if (!res || res->count_crtcs == 0) { log("no CRTC on %s", dev); return -1; }
    ctx->crtc_id = res->crtcs[0];

    for (int i = 0; i < res->count_connectors; i++) {
        drmModeConnector *c = drmModeGetConnector(ctx->fd, res->connectors[i]);
        if (!c) continue;
        if (c->connector_type == DRM_MODE_CONNECTOR_WRITEBACK)
            ctx->wb_conn_id = c->connector_id;
        else
            ctx->primary_conn_id = c->connector_id;
        drmModeFreeConnector(c);
    }
    drmModeFreeResources(res);

    if (!ctx->wb_conn_id) { log("no writeback connector"); return -1; }

    /* Pick the PREFERRED mode (falling back to modes[0]) — the same choice
     * wlroots makes.  The compositor programming an identical mode is what
     * keeps its startup commit from being treated as a modeset (see header). */
    drmModeConnector *pc = drmModeGetConnector(ctx->fd, ctx->primary_conn_id);
    if (!pc || pc->count_modes == 0) { log("no modes"); return -1; }
    drmModeModeInfo mode = pc->modes[0];
    for (int i = 0; i < pc->count_modes; i++) {
        if (pc->modes[i].type & DRM_MODE_TYPE_PREFERRED) {
            mode = pc->modes[i]; break;
        }
    }
    drmModeFreeConnector(pc);
    ctx->mode_w = mode.hdisplay;
    ctx->mode_h = mode.vdisplay;
    log("mode: %ux%u@%u", ctx->mode_w, ctx->mode_h, mode.vrefresh);

    drmModePlaneRes *pres = drmModeGetPlaneResources(ctx->fd);
    for (uint32_t i = 0; i < (pres ? pres->count_planes : 0); i++) {
        drmModePlane *pl = drmModeGetPlane(ctx->fd, pres->planes[i]);
        if (!pl) continue;
        if (pl->possible_crtcs & 1) {
            if (ctx->n_planes < 16)
                ctx->all_plane_ids[ctx->n_planes++] = pl->plane_id;

            drmModeObjectProperties *pp = drmModeObjectGetProperties(
                ctx->fd, pl->plane_id, DRM_MODE_OBJECT_PLANE);
            for (uint32_t j = 0; j < (pp ? pp->count_props : 0); j++) {
                drmModePropertyRes *r = drmModeGetProperty(ctx->fd, pp->props[j]);
                if (r && !strcmp(r->name, "type") &&
                    pp->prop_values[j] == DRM_PLANE_TYPE_PRIMARY)
                    ctx->plane_id = pl->plane_id;
                drmModeFreeProperty(r);
            }
            drmModeFreeObjectProperties(pp);
        }
        drmModeFreePlane(pl);
    }
    drmModeFreePlaneResources(pres);
    if (!ctx->plane_id) { log("no primary plane"); return -1; }
    log("found %u planes for CRTC (leasing all)", ctx->n_planes);

    ctx->wb_fb_id_prop   = find_prop(ctx->fd, ctx->wb_conn_id,
                                     DRM_MODE_OBJECT_CONNECTOR, "WRITEBACK_FB_ID");
    ctx->wb_crtc_id_prop = find_prop(ctx->fd, ctx->wb_conn_id,
                                     DRM_MODE_OBJECT_CONNECTOR, "CRTC_ID");
    if (!ctx->wb_fb_id_prop || !ctx->wb_crtc_id_prop) {
        log("missing writeback properties"); return -1;
    }

    /* Writeback destination (bridge reads captured pixels from here) and a
     * dumb scanout FB for the primary plane (the modeset needs a plane FB;
     * dumb buffers are zero-filled → black until the compositor takes over) */
    if (alloc_dumb_fb(ctx->fd, ctx->mode_w, ctx->mode_h,
                      PROT_READ, &ctx->capture) < 0) return -1;
    if (alloc_dumb_fb(ctx->fd, ctx->mode_w, ctx->mode_h,
                      0, &ctx->scanout) < 0) return -1;

    /* Reset any stale VKMS state from a previous bridge run.  Clearing both
     * wb CRTC_ID and WRITEBACK_FB_ID in one commit also drops a stale FB
     * reference held by the kernel if the previous bridge died mid-capture. */
    {
        drmModeAtomicReq *req = drmModeAtomicAlloc();
        drmModeAtomicAddProperty(req, ctx->wb_conn_id, ctx->wb_crtc_id_prop, 0);
        drmModeAtomicAddProperty(req, ctx->wb_conn_id, ctx->wb_fb_id_prop,   0);
        int r = drmModeAtomicCommit(ctx->fd, req, DRM_MODE_ATOMIC_ALLOW_MODESET, NULL);
        drmModeAtomicFree(req);
        log("wb reset: %s", r < 0 ? strerror(errno) : "ok");
    }

    /* Full modeset while the bridge still owns everything (no lease yet):
     * enable the CRTC with the preferred mode, route the primary connector,
     * put the dumb FB on the primary plane, and attach the writeback
     * connector.  After the lease is created the master fd can no longer
     * reference the CRTC (constraint 2), so this is the only opportunity. */
    {
        uint32_t crtc_active_p  = find_prop(ctx->fd, ctx->crtc_id, DRM_MODE_OBJECT_CRTC, "ACTIVE");
        uint32_t crtc_mode_id_p = find_prop(ctx->fd, ctx->crtc_id, DRM_MODE_OBJECT_CRTC, "MODE_ID");
        uint32_t conn_crtc_id_p = find_prop(ctx->fd, ctx->primary_conn_id,
                                             DRM_MODE_OBJECT_CONNECTOR, "CRTC_ID");
        uint32_t pl = ctx->plane_id;
        uint32_t pl_fb_p      = find_prop(ctx->fd, pl, DRM_MODE_OBJECT_PLANE, "FB_ID");
        uint32_t pl_crtc_p    = find_prop(ctx->fd, pl, DRM_MODE_OBJECT_PLANE, "CRTC_ID");
        uint32_t pl_src_x_p   = find_prop(ctx->fd, pl, DRM_MODE_OBJECT_PLANE, "SRC_X");
        uint32_t pl_src_y_p   = find_prop(ctx->fd, pl, DRM_MODE_OBJECT_PLANE, "SRC_Y");
        uint32_t pl_src_w_p   = find_prop(ctx->fd, pl, DRM_MODE_OBJECT_PLANE, "SRC_W");
        uint32_t pl_src_h_p   = find_prop(ctx->fd, pl, DRM_MODE_OBJECT_PLANE, "SRC_H");
        uint32_t pl_crtc_x_p  = find_prop(ctx->fd, pl, DRM_MODE_OBJECT_PLANE, "CRTC_X");
        uint32_t pl_crtc_y_p  = find_prop(ctx->fd, pl, DRM_MODE_OBJECT_PLANE, "CRTC_Y");
        uint32_t pl_crtc_w_p  = find_prop(ctx->fd, pl, DRM_MODE_OBJECT_PLANE, "CRTC_W");
        uint32_t pl_crtc_h_p  = find_prop(ctx->fd, pl, DRM_MODE_OBJECT_PLANE, "CRTC_H");

        uint32_t mode_blob = 0;
        if (drmModeCreatePropertyBlob(ctx->fd, &mode, sizeof(mode), &mode_blob)) {
            perror("mode blob"); return -1;
        }

        drmModeAtomicReq *req = drmModeAtomicAlloc();
        drmModeAtomicAddProperty(req, ctx->crtc_id, crtc_mode_id_p, mode_blob);
        drmModeAtomicAddProperty(req, ctx->crtc_id, crtc_active_p, 1);
        drmModeAtomicAddProperty(req, ctx->primary_conn_id, conn_crtc_id_p, ctx->crtc_id);
        drmModeAtomicAddProperty(req, pl, pl_fb_p,     ctx->scanout.id);
        drmModeAtomicAddProperty(req, pl, pl_crtc_p,   ctx->crtc_id);
        drmModeAtomicAddProperty(req, pl, pl_src_x_p,  0);
        drmModeAtomicAddProperty(req, pl, pl_src_y_p,  0);
        drmModeAtomicAddProperty(req, pl, pl_src_w_p,  (uint64_t)ctx->mode_w << 16);
        drmModeAtomicAddProperty(req, pl, pl_src_h_p,  (uint64_t)ctx->mode_h << 16);
        drmModeAtomicAddProperty(req, pl, pl_crtc_x_p, 0);
        drmModeAtomicAddProperty(req, pl, pl_crtc_y_p, 0);
        drmModeAtomicAddProperty(req, pl, pl_crtc_w_p, ctx->mode_w);
        drmModeAtomicAddProperty(req, pl, pl_crtc_h_p, ctx->mode_h);
        /* Attach writeback in the same commit — CRTC is enabled here, and we
         * won't be able to reference it again after leasing. */
        drmModeAtomicAddProperty(req, ctx->wb_conn_id, ctx->wb_crtc_id_prop, ctx->crtc_id);

        int r = drmModeAtomicCommit(ctx->fd, req, DRM_MODE_ATOMIC_ALLOW_MODESET, NULL);
        drmModeAtomicFree(req);
        if (r < 0) {
            log("initial modeset + wb attach failed: %s", strerror(errno));
            return -1;
        }
        ctx->wb_attached = true;
        log("modeset done: crtc=%u active %ux%u, wb=%u attached, plane fb=%u",
            ctx->crtc_id, ctx->mode_w, ctx->mode_h, ctx->wb_conn_id, ctx->scanout.id);
    }

    log("VKMS ready: crtc=%u wb=%u plane=%u mode=%ux%u",
        ctx->crtc_id, ctx->wb_conn_id, ctx->plane_id, ctx->mode_w, ctx->mode_h);
    return 0;
}

/* ── per-frame capture ───────────────────────────────────────────────── */

/*
 * Queue one writeback capture.  The writeback connector was attached to the
 * enabled CRTC during wb_setup(); each capture only sets WRITEBACK_FB_ID on
 * the (non-leased) connector — the commit must not reference the CRTC or any
 * other leased object (constraint 2).
 */
static int wb_capture(wb_ctx_t *ctx)
{
    drmModeAtomicReq *req = drmModeAtomicAlloc();
    drmModeAtomicAddProperty(req, ctx->wb_conn_id,
                             ctx->wb_fb_id_prop, ctx->capture.id);
    int ret = drmModeAtomicCommit(ctx->fd, req, DRM_MODE_ATOMIC_ALLOW_MODESET, NULL);
    drmModeAtomicFree(req);
    return ret;
}

/* ── scale + convert ─────────────────────────────────────────────────── */

/* Convert + scale the captured frame; compute the dirty bounding rect vs the
 * previous frame.  Returns true (and fills *dirty_rect) when anything changed;
 * only the dirty rows are copied into the shared framebuffer. */
static bool convert_scale(const uint32_t *src, uint32_t src_w, uint32_t src_h,
                           uint16_t *dst, uint16_t *tmp, const uint16_t *prev,
                           swtfb_rect_t *dirty_rect)
{
    uint32_t min_y = PANEL_H, max_y = 0, min_x = PANEL_W, max_x = 0;

    for (uint32_t dy = 0; dy < PANEL_H; dy++) {
        uint32_t sy = (uint32_t)((uint64_t)dy * src_h / PANEL_H);
        const uint32_t *row = src + (size_t)sy * src_w;
        uint16_t *out = tmp + (size_t)dy * PANEL_W;
        for (uint32_t dx = 0; dx < PANEL_W; dx++) {
            uint32_t sx = (uint32_t)((uint64_t)dx * src_w / PANEL_W);
            uint32_t p = row[sx];
            out[dx] = (uint16_t)(((p >> 8) & 0xF800) |
                                 ((p >> 5) & 0x07E0) |
                                 ((p >> 3) & 0x001F));
        }
        /* row-level diff, then refine the x-extent of the change */
        const uint16_t *prow = prev + (size_t)dy * PANEL_W;
        if (memcmp(out, prow, PANEL_W * 2) != 0) {
            if (dy < min_y) min_y = dy;
            if (dy > max_y) max_y = dy;
            uint32_t x0 = 0, x1 = PANEL_W - 1;
            while (x0 < PANEL_W && out[x0] == prow[x0]) x0++;
            while (x1 > x0 && out[x1] == prow[x1]) x1--;
            if (x0 < min_x) min_x = x0;
            if (x1 > max_x) max_x = x1;
        }
    }

    if (min_y > max_y) return false;   /* no change */

    for (uint32_t dy = min_y; dy <= max_y; dy++)
        memcpy(dst + (size_t)dy * PANEL_W, tmp + (size_t)dy * PANEL_W, PANEL_W * 2);

    dirty_rect->top    = min_y;
    dirty_rect->left   = min_x;
    dirty_rect->width  = max_x - min_x + 1;
    dirty_rect->height = max_y - min_y + 1;
    return true;
}

/* ── main ────────────────────────────────────────────────────────────── */

int main(int argc, char **argv)
{
    const char *dev = (argc > 1) ? argv[1] : "/dev/dri/card1";

    wb_ctx_t ctx = {};
    if (wb_setup(&ctx, dev) < 0) return 1;

    /* Start DRM lease server thread — only AFTER the modeset + wb attach,
     * since creating the lease removes the CRTC from the master fd's reach. */
    lease_args_t la = {
        .master_fd = ctx.fd,
        .crtc_id   = ctx.crtc_id,
        .conn_id   = ctx.primary_conn_id,
        .n_planes  = ctx.n_planes,
    };
    memcpy(la.plane_ids, ctx.all_plane_ids, ctx.n_planes * sizeof(uint32_t));
    pthread_t thr;
    pthread_create(&thr, NULL, lease_server, &la);

    /* Wait for einkbridge swtfb (up to 15 s) */
    int shm_fd = -1;
    for (int i = 0; i < 30 && shm_fd < 0; i++) {
        shm_fd = open(SHM_PATH, O_RDWR);
        if (shm_fd < 0) {
            if (i == 0) log("waiting for einkbridge shm at %s ...", SHM_PATH);
            usleep(500000);
        }
    }
    if (shm_fd < 0) { log("einkbridge shm not found after 15 s"); return 1; }

    uint16_t *swtfb = mmap(NULL, SHM_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
    close(shm_fd);
    if (swtfb == MAP_FAILED) { perror("mmap swtfb"); return 1; }
    log("shm mapped: %s", SHM_PATH);

    uint16_t *prev = calloc(1, SHM_SIZE);
    uint16_t *tmp  = malloc(SHM_SIZE);
    if (!prev || !tmp) return 1;

    log("capture loop: %s → %s @ %d ms", dev, SHM_PATH, POLL_MS);

    bool err_logged = false;
    for (;;) {
        /* Only capture while a compositor holds the lease — otherwise the
         * scanout is just our dummy black FB. */
        if (!g_lease_client_active) {
            usleep((useconds_t)POLL_MS * 1000);
            continue;
        }

        if (wb_capture(&ctx) < 0) {
            if (!err_logged) {
                log("wb capture failed: %s (suppressing repeats)", strerror(errno));
                err_logged = true;
            }
            usleep((useconds_t)POLL_MS * 1000);
            continue;
        }
        err_logged = false;

        swtfb_rect_t dirty;
        if (convert_scale((const uint32_t *)ctx.capture.map,
                          ctx.mode_w, ctx.mode_h,
                          swtfb, tmp, prev, &dirty)) {
            memcpy(prev, tmp, SHM_SIZE);
            send_update(dirty);
        }

        usleep((useconds_t)POLL_MS * 1000);
    }
}
