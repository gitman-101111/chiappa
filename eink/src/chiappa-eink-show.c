/* chiappa-eink-show — blit a full-screen image to the einkbridge panel.
 *
 * Usage: chiappa-eink-show <file> [waveform]
 *
 * <file> is a raw, headerless image sized to the panel (REQUIRED
 * SWTFB_WIDTH x SWTFB_HEIGHT env vars; e.g. 954x1696 on the Paper Pro Move):
 *   - W*H     bytes → 8-bit grayscale (each byte replicated to RGB565)
 *   - W*H*2   bytes → RGB565 already (memcpy'd straight in)
 *   - W*H*3   bytes → RGB888 (packed to RGB565 — for the color Gallery 3
 *                     panel; flatten alpha onto a background first)
 * waveform: rm2fb waveform id (default 2 = GC16, full-quality grayscale;
 *           1 = DU, fast 2-level). See eink/src/vkms_bridge.c.
 *
 * This is the generic "show a status frame" client used by the lifecycle
 * screens (power-on, sleeping, shutting down, rebooting, low battery). It links
 * nothing but libc — the einkbridge does all the panel/waveform work; a client
 * only writes pixels into the shared framebuffer and pokes the IPC socket.
 * (Sibling to swtfb_fill.c; same rm2fb protocol as einkbridge.cpp.)
 */
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

/* Panel geometry: REQUIRED SWTFB_WIDTH/SWTFB_HEIGHT env vars (same
 * convention as einkbridge). No compiled-in default. */
static int env_dim(const char *name) {
    const char *v = getenv(name);
    int n = v ? atoi(v) : 0;
    if (n <= 0) {
        fprintf(stderr,
                "%s is unset/invalid — set SWTFB_WIDTH and SWTFB_HEIGHT to "
                "the panel size in pixels (e.g. 954 and 1696 for the "
                "reMarkable Paper Pro Move)\n", name);
        exit(2);
    }
    return n;
}
#define W ((size_t)g_w)
#define H ((size_t)g_h)
#define NPX ((size_t)g_w * (size_t)g_h)
#define SZ (NPX * 2) /* RGB565 framebuffer size */
static int g_w, g_h;

typedef struct { uint32_t top, left, width, height; } rect_t;
typedef struct { rect_t region; uint32_t waveform; uint32_t flags; } update_t;

static inline uint16_t gray565(uint8_t g) {
    return (uint16_t)(((g >> 3) << 11) | ((g >> 2) << 5) | (g >> 3));
}

static inline uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b) {
    return (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s <file> [waveform]\n", argv[0]);
        return 2;
    }
    g_w = env_dim("SWTFB_WIDTH");
    g_h = env_dim("SWTFB_HEIGHT");
    uint32_t waveform = (argc > 2) ? (uint32_t)strtoul(argv[2], NULL, 0) : 2;

    /* Read the image file whole. */
    int in = open(argv[1], O_RDONLY);
    if (in < 0) { perror("open image"); return 1; }
    struct stat st;
    if (fstat(in, &st) < 0) { perror("fstat"); return 1; }
    if ((size_t)st.st_size != NPX && (size_t)st.st_size != SZ &&
        (size_t)st.st_size != NPX * 3) {
        fprintf(stderr, "%s: %s is %lld bytes; expected %zu (gray), %zu (rgb565) or %zu (rgb888)\n",
                argv[0], argv[1], (long long)st.st_size, NPX, SZ, NPX * 3);
        return 2;
    }
    uint8_t *img = malloc(st.st_size);
    if (!img) { perror("malloc"); return 1; }
    for (off_t off = 0; off < st.st_size; ) {
        ssize_t n = read(in, img + off, st.st_size - off);
        if (n <= 0) { perror("read image"); return 1; }
        off += n;
    }
    close(in);

    /* Map the shared framebuffer and write the frame. */
    int fd = open("/dev/shm/swtfb", O_RDWR);
    if (fd < 0) { perror("open /dev/shm/swtfb (is einkbridge running?)"); return 1; }
    uint16_t *fb = mmap(NULL, SZ, PROT_WRITE, MAP_SHARED, fd, 0);
    if (fb == MAP_FAILED) { perror("mmap"); return 1; }
    close(fd);

    if ((size_t)st.st_size == SZ) {
        memcpy(fb, img, SZ);
    } else if ((size_t)st.st_size == NPX * 3) { /* RGB888 → RGB565 */
        for (size_t i = 0; i < NPX; i++)
            fb[i] = rgb565(img[i * 3], img[i * 3 + 1], img[i * 3 + 2]);
    } else { /* grayscale → RGB565 */
        for (size_t i = 0; i < NPX; i++) fb[i] = gray565(img[i]);
    }
    munmap(fb, SZ);
    free(img);

    /* Trigger a full-frame refresh. */
    int s = socket(AF_UNIX, SOCK_STREAM, 0);
    if (s < 0) { perror("socket"); return 1; }
    struct sockaddr_un a = { .sun_family = AF_UNIX };
    strncpy(a.sun_path, "/tmp/swtfb.ipc", sizeof(a.sun_path) - 1);
    if (connect(s, (struct sockaddr *)&a, sizeof(a)) < 0) { perror("connect ipc"); return 1; }
    update_t u = { { 0, 0, (uint32_t)g_w, (uint32_t)g_h }, waveform, 0 };
    if (write(s, &u, sizeof(u)) != (ssize_t)sizeof(u)) { perror("write update"); return 1; }
    close(s);
    return 0;
}
