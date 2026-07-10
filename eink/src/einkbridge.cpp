// einkbridge — rm2fb-compatible IPC bridge for chiappa
//
// Creates a shared framebuffer (954x1696 RGB565) at /dev/shm/swtfb and a
// Unix socket at /tmp/swtfb.ipc.  Clients (e.g. KOReader) write pixels into
// the shared memory then send an swtfb_update message on the socket.  The
// bridge reads the dirty region, converts it to a QImage, and renders it
// through the epaper QPA → SWTCON → e-ink panel.
//
// Protocol (matches rm2fb / reMarkable 2 swtfb):
//   socket : /tmp/swtfb.ipc  (SOCK_STREAM, server)
//   shm    : /dev/shm/swtfb  (954 * 1696 * 2 bytes, RGB565)
//   msg    : struct swtfb_update { rect{top,left,width,height}; waveform; flags; }

#include <QGuiApplication>
#include <QQuickView>
#include <QQuickItem>
#include <QQuickImageProvider>
#include <QQmlContext>
#include <QQmlProperty>
#include <QObject>
#include <QThread>
#include <QImage>
#include <mutex>
#include <vector>

#include <poll.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <cerrno>

// ── panel geometry ──────────────────────────────────────────────────────────
// REQUIRED: SWTFB_WIDTH / SWTFB_HEIGHT env vars (panel size in pixels; e.g.
// 954 and 1696 for the reMarkable Paper Pro Move). No compiled-in default —
// all rm2fb clients (shim, show, fill) share the same convention; set them
// identically for every process on the bus.
static int env_dim(const char *name) {
    const char *v = getenv(name);
    int n = v ? atoi(v) : 0;
    if (n <= 0) {
        fprintf(stderr,
                "einkbridge: %s is unset/invalid — set SWTFB_WIDTH and "
                "SWTFB_HEIGHT to the panel size in pixels (e.g. 954 and 1696 "
                "for the reMarkable Paper Pro Move)\n", name);
        exit(2);
    }
    return n;
}
static const int W = env_dim("SWTFB_WIDTH");
static const int H = env_dim("SWTFB_HEIGHT");
static const int SHM_SIZE = W * H * 2; // RGB565

// ── IPC paths ────────────────────────────────────────────────────────────────
static constexpr const char *SOCKET_PATH = "/tmp/swtfb.ipc";
static constexpr const char *SHM_PATH    = "/dev/shm/swtfb";

// ── wire protocol ────────────────────────────────────────────────────────────
struct swtfb_rect   { uint32_t top, left, width, height; };
struct swtfb_update { swtfb_rect region; uint32_t waveform; uint32_t flags; };

// ── shared state ─────────────────────────────────────────────────────────────
static uint8_t  *g_shm   = nullptr;  // mmap of /dev/shm/swtfb
static std::mutex    g_mutex;
static QImage        g_frame(W, H, QImage::Format_RGB16);

// ── update notifier ──────────────────────────────────────────────────────────
// The socket thread bumps `seq`/`waveform` properties on the QML root item
// (queued into the GUI thread) once per received update; the QML Image binds
// its source URL to seq, so the provider is re-queried exactly once per
// update.  No timer — the process sleeps between client updates.
// (Properties are declared in QML; no moc'd QObject needed.)
static QQuickItem *g_qml_root = nullptr;
static int g_seq = 0;

// ── Image provider ────────────────────────────────────────────────────────────
class FramebufferProvider : public QQuickImageProvider {
public:
    FramebufferProvider() : QQuickImageProvider(QQuickImageProvider::Image) {}
    QImage requestImage(const QString &, QSize *sz, const QSize &) override {
        std::lock_guard<std::mutex> lk(g_mutex);
        if (sz) *sz = g_frame.size();
        return g_frame.copy();
    }
};

// ── Socket listener thread ────────────────────────────────────────────────────
class SocketThread : public QThread {
public:
    int server_fd = -1;

    bool setup() {
        unlink(SOCKET_PATH);
        server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (server_fd < 0) { perror("socket"); return false; }
        sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        strncpy(addr.sun_path, SOCKET_PATH, sizeof(addr.sun_path) - 1);
        if (bind(server_fd, (sockaddr *)&addr, sizeof(addr)) < 0) {
            perror("bind"); return false;
        }
        chmod(SOCKET_PATH, 0666);
        listen(server_fd, 8);
        return true;
    }

    void run() override {
        fprintf(stderr, "[bridge] socket listening on %s\n", SOCKET_PATH);
        // Multi-client: poll the listen fd plus every connected client. A
        // single-accept loop starves later clients — the app (KoReader) holds
        // its connection for life, so a one-shot client (chiappa-eink-show
        // drawing a lifecycle screen) would connect fine into the backlog but
        // never be read.
        std::vector<pollfd> fds;
        fds.push_back({server_fd, POLLIN, 0});
        while (true) {
            if (poll(fds.data(), fds.size(), -1) < 0) {
                if (errno == EINTR) continue;
                perror("poll");
                break;
            }
            // New connection?
            if (fds[0].revents & POLLIN) {
                int client = accept(server_fd, nullptr, nullptr);
                if (client >= 0) fds.push_back({client, POLLIN, 0});
                else perror("accept");
            }
            // Service every readable client; drop the closed ones.
            for (size_t i = 1; i < fds.size();) {
                if (fds[i].revents & (POLLIN | POLLHUP | POLLERR)) {
                    if (!readUpdate(fds[i].fd)) {
                        close(fds[i].fd);
                        fds.erase(fds.begin() + i);
                        continue;
                    }
                }
                ++i;
            }
        }
    }

    // Read one update message from a client; false = disconnected/error.
    bool readUpdate(int fd) {
        swtfb_update msg{};
        ssize_t n = recv(fd, &msg, sizeof(msg), MSG_WAITALL);
        if (n <= 0) return false;
        if ((size_t)n < sizeof(msg)) return true;

        // Clamp rect to screen bounds
        uint32_t top  = std::min(msg.region.top,    (uint32_t)(H - 1));
        uint32_t left = std::min(msg.region.left,   (uint32_t)(W - 1));
        uint32_t rw   = std::min(msg.region.width,  (uint32_t)(W - left));
        uint32_t rh   = std::min(msg.region.height, (uint32_t)(H - top));
        if (rw == 0 || rh == 0) { applyFullFrame(); return true; }

        {
            std::lock_guard<std::mutex> lk(g_mutex);
            // Copy the dirty rect from shm into g_frame
            for (uint32_t y = 0; y < rh; ++y) {
                const uint8_t *src = g_shm + ((top + y) * W + left) * 2;
                uint8_t *dst = g_frame.scanLine(top + y) + left * 2;
                memcpy(dst, src, rw * 2);
            }
        }
        notify(msg.waveform);
        return true;
    }

    void applyFullFrame() {
        {
            std::lock_guard<std::mutex> lk(g_mutex);
            memcpy(g_frame.bits(), g_shm, SHM_SIZE);
        }
        notify(2 /* GC16 */);
    }

    void notify(uint32_t waveform) {
        // Hop into the GUI thread; triggers exactly one QML repaint.
        QMetaObject::invokeMethod(qApp, [waveform]() {
            if (!g_qml_root) return;
            QQmlProperty::write(g_qml_root, "waveform", (int)waveform);
            QQmlProperty::write(g_qml_root, "seq", ++g_seq);
        }, Qt::QueuedConnection);
    }
};
// ── main ──────────────────────────────────────────────────────────────────────
int main(int argc, char **argv)
{
    // Create/open shared memory
    int shm_fd = open(SHM_PATH, O_RDWR | O_CREAT, 0666);
    if (shm_fd < 0) { perror("open shm"); return 1; }
    if (ftruncate(shm_fd, SHM_SIZE) < 0) { perror("ftruncate"); return 1; }
    // World-writable so unprivileged clients (KOReader etc.) can write pixels.
    fchmod(shm_fd, 0666);
    g_shm = (uint8_t *)mmap(nullptr, SHM_SIZE, PROT_READ | PROT_WRITE,
                             MAP_SHARED, shm_fd, 0);
    if (g_shm == MAP_FAILED) { perror("mmap"); return 1; }
    close(shm_fd);

    // Clear the frame to white (0xFFFF in RGB565)
    memset(g_shm, 0xFF, SHM_SIZE);
    g_frame.fill(Qt::white);

    fprintf(stderr, "[bridge] shm ready at %s (%d bytes, %dx%d RGB565)\n",
            SHM_PATH, SHM_SIZE, W, H);

    QGuiApplication app(argc, argv);

    auto *provider = new FramebufferProvider();

    auto *sockThread = new SocketThread();

    // QML view
    QQuickView view;
    view.engine()->addImageProvider("framebuffer", provider);
    view.setResizeMode(QQuickView::SizeRootObjectToView);
    view.resize(W, H);

    const char *qml = (argc > 1) ? argv[1] : "/opt/eink/bridge.qml";
    view.setSource(QUrl::fromLocalFile(qml));
    if (view.status() == QQuickView::Error) {
        for (const auto &e : view.errors())
            fprintf(stderr, "[bridge] QML error: %s\n", qPrintable(e.toString()));
        return 2;
    }
    view.show();
    g_qml_root = view.rootObject();
    // Bind the socket only now, with the render path up: the socket file IS
    // the readiness signal clients gate on ([ -S /tmp/swtfb.ipc ]). Binding
    // earlier made clients connect and queue updates seconds before the
    // engine could render them — a splash drawn into that window is
    // overwritten unseen the moment rendering starts.
    if (!sockThread->setup()) return 1;
    sockThread->start();

    fprintf(stderr, "[bridge] running — clients connect to %s\n", SOCKET_PATH);
    return app.exec();
}
