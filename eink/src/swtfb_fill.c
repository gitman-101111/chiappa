/* swtfb_fill — fill the einkbridge shared framebuffer and trigger a refresh.
 *
 * Usage: swtfb-fill [byte]      (default 0xFF = white; 0x00 = black)
 *
 * Fills /dev/shm/swtfb (RGB565) with the given byte value and sends a
 * full-screen GC16 update over the rm2fb IPC socket. Handy for manually
 * clearing the panel. Panel geometry: REQUIRED SWTFB_WIDTH/SWTFB_HEIGHT env
 * vars (same convention as einkbridge).
 */
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

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

typedef struct { uint32_t top, left, width, height; } rect_t;
typedef struct { rect_t region; uint32_t waveform; uint32_t flags; } update_t;

int main(int argc, char **argv)
{
    int fill = (argc > 1) ? (int)strtol(argv[1], NULL, 0) : 0xFF;
    int w = env_dim("SWTFB_WIDTH");
    int h = env_dim("SWTFB_HEIGHT");
    size_t sz = (size_t)w * h * 2;

    int fd = open("/dev/shm/swtfb", O_RDWR);
    if (fd < 0) { perror("swtfb"); return 1; }
    void *m = mmap(NULL, sz, PROT_WRITE, MAP_SHARED, fd, 0);
    if (m == MAP_FAILED) { perror("mmap"); return 1; }
    memset(m, fill & 0xFF, sz);
    munmap(m, sz);
    close(fd);

    int s = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un a = { .sun_family = AF_UNIX };
    strcpy(a.sun_path, "/tmp/swtfb.ipc");
    if (connect(s, (struct sockaddr *)&a, sizeof(a)) == 0) {
        update_t u = { { 0, 0, (uint32_t)w, (uint32_t)h }, 2 /* GC16 */, 0 };
        write(s, &u, sizeof(u));
    } else {
        perror("ipc connect");
    }
    close(s);
    return 0;
}
