// bridge_test.c — test client for einkbridge
// Writes a colour gradient to the shared framebuffer then sends an update message.
// Compile on device: gcc -O2 -o bridge_test bridge_test.c
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>

#define W 954
#define H 1696
#define SHM_PATH    "/dev/shm/swtfb"
#define SOCKET_PATH "/tmp/swtfb.ipc"

struct swtfb_rect   { uint32_t top, left, width, height; };
struct swtfb_update { struct swtfb_rect region; uint32_t waveform; uint32_t flags; };

// Pack RGB888 -> RGB565
static uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b) {
    return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
}

int main(void) {
    // Map shared framebuffer
    int fd = open(SHM_PATH, O_RDWR);
    if (fd < 0) { perror("open " SHM_PATH); return 1; }
    uint16_t *fb = mmap(NULL, W * H * 2, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (fb == MAP_FAILED) { perror("mmap"); return 1; }
    close(fd);

    // Draw: horizontal rainbow gradient top half, black/white checkerboard bottom
    for (int y = 0; y < H / 2; y++) {
        for (int x = 0; x < W; x++) {
            uint8_t r = (uint8_t)(x * 255 / W);
            uint8_t g = (uint8_t)(y * 255 / (H / 2));
            uint8_t b = 128;
            fb[y * W + x] = rgb565(r, g, b);
        }
    }
    for (int y = H / 2; y < H; y++) {
        for (int x = 0; x < W; x++) {
            int cell = ((x / 60) + (y / 60)) & 1;
            fb[y * W + x] = cell ? 0xFFFF : 0x0000;
        }
    }
    printf("Framebuffer written (%dx%d RGB565)\n", W, H);

    // Send full-screen update
    int sock = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sock < 0) { perror("socket"); return 1; }
    struct sockaddr_un addr = { .sun_family = AF_UNIX };
    strncpy(addr.sun_path, SOCKET_PATH, sizeof(addr.sun_path) - 1);
    if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("connect " SOCKET_PATH);
        return 1;
    }
    struct swtfb_update msg = {
        .region   = { 0, 0, W, H },
        .waveform = 2,   // GC16 — full refresh
        .flags    = 0,
    };
    write(sock, &msg, sizeof(msg));
    close(sock);
    printf("Update sent — check the display\n");
    return 0;
}
