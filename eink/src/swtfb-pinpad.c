/* swtfb-pinpad — full-screen PIN lock drawn via the einkbridge framebuffer.
 *
 * Usage: swtfb-pinpad <pin-file>          block until the correct PIN is
 *                                         entered on the touchscreen; exit 0
 *        swtfb-pinpad --set <pin-file> <pin>
 *                                         hash <pin> (crypt SHA-512, random
 *                                         salt) into <pin-file> and exit
 *
 * If <pin-file> does not exist, lock mode exits 0 immediately (no PIN set =
 * no lock). This is an access deterrent, NOT encryption: the data on disk is
 * readable by anyone with SDP or physical eMMC access.
 *
 * There is no submit key: the entry is verified after every digit from
 * MIN_PIN on and unlocks on match. Reaching MAX_PIN without a match clears
 * the entry, shows a fail mark, and adds a 2 s delay after every 3rd failure.
 *
 * Rendering: compact centered 3x4 grid (1-9 / 0 / backspace) as hairline
 * circles with seven-segment digits; entry dots above. Keypresses update
 * only the dots band with a fast DU waveform — the full-quality refresh
 * happens once, at first draw. Touch: first evdev device advertising
 * ABS_MT_POSITION_X, coordinates scaled from the device's ABS ranges to the
 * panel; keys register on touch-DOWN.
 *
 * Env (REQUIRED): SWTFB_WIDTH / SWTFB_HEIGHT (same convention as einkbridge).
 * Env (optional): SWTFB_PINPAD_DEBUG=1 traces taps and verify attempts to
 * stderr (journal) — never the PIN itself.
 * Build: cc -O2 -o swtfb-pinpad swtfb-pinpad.c -lcrypt
 *        (-DPREVIEW_ONLY: draw once and exit — design preview, no touch)
 */
#define _GNU_SOURCE
#include <crypt.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <time.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#include <linux/input.h>

#define SHM_PATH "/dev/shm/swtfb"
#define IPC_PATH "/tmp/swtfb.ipc"
#define MAX_PIN 12
#define MIN_PIN 4

#define WF_GC16 2
#define WF_DU   1

#define WHITE 0xFFFF
#define BLACK 0x0000
#define GREY  0xC618 /* light grey in RGB565 */

typedef struct { uint32_t top, left, width, height; } rect_t;
typedef struct { rect_t region; uint32_t waveform; uint32_t flags; } update_t;

static int W, H;
static uint16_t *fb;
static int ipc = -1;
static int debug;

static int env_dim(const char *name) {
    const char *v = getenv(name);
    int n = v ? atoi(v) : 0;
    if (n <= 0) {
        fprintf(stderr,
                "swtfb-pinpad: %s is unset/invalid — set SWTFB_WIDTH and "
                "SWTFB_HEIGHT to the panel size in pixels\n", name);
        exit(2);
    }
    return n;
}

static void refresh(rect_t r, uint32_t wf) {
    update_t u = { r, wf, 0 };
    if (write(ipc, &u, sizeof u) != (ssize_t)sizeof u) { /* bridge gone */ }
}

static void fill(int x, int y, int w, int h, uint16_t c) {
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x + w > W) w = W - x;
    if (y + h > H) h = H - y;
    for (int j = y; j < y + h; j++) {
        uint16_t *row = fb + (size_t)j * W + x;
        for (int i = 0; i < w; i++) row[i] = c;
    }
}

/* Hairline ring / filled disc (w = r), drawn by radial distance. */
static void ring(int cx, int cy, int r, int w, uint16_t c) {
    for (int j = -r; j <= r; j++)
        for (int i = -r; i <= r; i++) {
            int d2 = i * i + j * j;
            if (d2 <= r * r && d2 >= (r - w) * (r - w)) {
                int px = cx + i, py = cy + j;
                if (px >= 0 && px < W && py >= 0 && py < H)
                    fb[(size_t)py * W + px] = c;
            }
        }
}

/* ── seven-segment digits ───────────────────────────────────────────────────
 * Segment layout (classic):  _a_
 *                           f| g |b     each glyph fits a (s wide × 2s tall)
 *                            |_ _|      box; stroke = s/6.
 *                           e|   |c
 *                            |_d_|                                          */
static const uint8_t SEG[10] = {
    /*0*/ 0b0111111, /*1*/ 0b0000110, /*2*/ 0b1011011, /*3*/ 0b1001111,
    /*4*/ 0b1100110, /*5*/ 0b1101101, /*6*/ 0b1111101, /*7*/ 0b0000111,
    /*8*/ 0b1111111, /*9*/ 0b1101111,
};

static void draw_digit(int digit, int x, int y, int s, uint16_t c) {
    int t = s / 6;
    if (t < 3) t = 3;
    uint8_t m = SEG[digit % 10];
    if (m & 0x01) fill(x, y, s, t, c);                     /* a */
    if (m & 0x02) fill(x + s - t, y, t, s, c);             /* b */
    if (m & 0x04) fill(x + s - t, y + s, t, s, c);         /* c */
    if (m & 0x08) fill(x, y + 2 * s - t, s, t, c);         /* d */
    if (m & 0x10) fill(x, y + s, t, s, c);                 /* e */
    if (m & 0x20) fill(x, y, t, s, c);                     /* f */
    if (m & 0x40) fill(x, y + s - t / 2, s, t, c);         /* g */
}

/* ── keypad geometry (compact, vendor-like) ─────────────────────────────────
 * key index 0..8 = digits 1-9; 10 = 0 (bottom middle); 11 = backspace
 * (bottom right); bottom-left cell is empty.                               */
static int pad_x, pad_y, cell;   /* square touch cells */
static int dots_y;

static void layout(void) {
    cell = W * 3 / 16;                 /* compact cells (r25 sizing) */
    pad_x = (W - 3 * cell) / 2;
    pad_y = H * 36 / 100;
    dots_y = H * 30 / 100;
}

static void key_center(int k, int *cx, int *cy) {
    int col = (k == 10) ? 1 : (k == 11) ? 2 : k % 3;
    int row = (k >= 9) ? 3 : k / 3;
    *cx = pad_x + col * cell + cell / 2;
    *cy = pad_y + row * cell + cell / 2;
}

static void draw_key(int k) {
    int cx, cy;
    key_center(k, &cx, &cy);
    int r = W * 9 / 200;                /* ~43 px ring radius (r25 sizing) */
    int s = r * 3 / 5;                  /* seven-segment glyph unit */

    ring(cx, cy, r, 2, GREY);
    if (k <= 8 || k == 10) {
        draw_digit(k == 10 ? 0 : k + 1, cx - s / 2, cy - s, s, BLACK);
    } else { /* backspace: X */
        int t = s / 5 < 3 ? 3 : s / 5;
        for (int i = -s / 2; i <= s / 2; i++) {
            fill(cx + i, cy + i, t, t, BLACK);
            fill(cx + i, cy - i, t, t, BLACK);
        }
    }
}

/* Momentary press feedback: filled disc over the key (DU), restored after the
 * tap is processed. */
static rect_t key_rect(int k) {
    int cx, cy;
    key_center(k, &cx, &cy);
    return (rect_t){ (uint32_t)(cy - cell / 2), (uint32_t)(cx - cell / 2),
                     (uint32_t)cell, (uint32_t)cell };
}

static void flash_key(int k) {
    int cx, cy;
    key_center(k, &cx, &cy);
    ring(cx, cy, W * 9 / 200, W * 9 / 200, BLACK);
    refresh(key_rect(k), WF_DU);
}

static void unflash_key(int k) {
    rect_t r = key_rect(k);
    fill((int)r.left, (int)r.top, (int)r.width, (int)r.height, WHITE);
    draw_key(k);
    refresh(r, WF_DU);
}

static rect_t dots_band(void) {
    int r = W / 80;
    return (rect_t){ (uint32_t)(dots_y - 4 * r), 0, (uint32_t)W, (uint32_t)(8 * r) };
}

static void draw_dots(int n, int fail) {
    int r = W / 80;
    rect_t b = dots_band();
    fill(b.left, b.top, b.width, b.height, WHITE);
    for (int i = 0; i < MAX_PIN && i < n; i++) {
        int x = W / 2 + (2 * i - n + 1) * 2 * r;
        ring(x, dots_y, r, r, BLACK);
    }
    if (fail)
        fill(W / 2 - W / 10, dots_y + 3 * r, W / 5, 2, BLACK);
}

static void draw_all(void) {
    for (size_t i = 0; i < (size_t)W * H; i++) fb[i] = WHITE;
    for (int k = 0; k <= 11; k++)
        if (k != 9) draw_key(k);
    draw_dots(0, 0);
    refresh((rect_t){0, 0, (uint32_t)W, (uint32_t)H}, WF_GC16);
}

/* ── touch input ────────────────────────────────────────────────────────── */
static int t_fd = -1;
static int t_minx, t_maxx, t_miny, t_maxy;
static int mt_slots = 0, cur_slot = 0; /* MT protocol B slot state */

/* Per-slot value via EVIOCGMTSLOTS: the singleton EVIOCGABS state is not
 * maintained for ABS_MT_* axes. Returns -1 on failure. */
static int mt_slot_val(unsigned code, int slot) {
    struct { unsigned code; int v[64]; } req;
    if (slot < 0 || slot >= mt_slots || mt_slots > 64) return -1;
    req.code = code;
    if (ioctl(t_fd, EVIOCGMTSLOTS(sizeof(unsigned) + (size_t)mt_slots * sizeof(int)), &req) < 0)
        return -1;
    return req.v[slot];
}

/* EVIOCGRAB touch + pen while the pad is up: the SIGSTOPped reader app's
 * evdev fds still queue events, and on SIGCONT the PIN taps replay into it
 * as page turns. The pen fd is held open, never read; grabs die with the
 * process. */
static int open_touch(void) {
    DIR *d = opendir("/dev/input");
    if (!d) return -1;
    struct dirent *e;
    char path[300];
    unsigned long absbit[(ABS_MAX / (8 * sizeof(long))) + 1];
    unsigned long keybit[(KEY_MAX / (8 * sizeof(long))) + 1];
    int fd = -1;
    while ((e = readdir(d))) {
        if (strncmp(e->d_name, "event", 5)) continue;
        snprintf(path, sizeof path, "/dev/input/%s", e->d_name);
        int f = open(path, O_RDONLY | O_CLOEXEC);
        if (f < 0) continue;
        memset(absbit, 0, sizeof absbit);
        ioctl(f, EVIOCGBIT(EV_ABS, sizeof absbit), absbit);
        memset(keybit, 0, sizeof keybit);
        ioctl(f, EVIOCGBIT(EV_KEY, sizeof keybit), keybit);
        if (fd < 0 &&
            (absbit[ABS_MT_POSITION_X / (8 * sizeof(long))] &
             (1UL << (ABS_MT_POSITION_X % (8 * sizeof(long)))))) {
            struct input_absinfo ai;
            ioctl(f, EVIOCGABS(ABS_MT_POSITION_X), &ai);
            t_minx = ai.minimum; t_maxx = ai.maximum;
            ioctl(f, EVIOCGABS(ABS_MT_POSITION_Y), &ai);
            t_miny = ai.minimum; t_maxy = ai.maximum;
            if (ioctl(f, EVIOCGABS(ABS_MT_SLOT), &ai) >= 0) {
                mt_slots = ai.maximum + 1;
                cur_slot = ai.value;
            }
            if (debug) {
                char nm[80] = "?";
                ioctl(f, EVIOCGNAME(sizeof nm), nm);
                fprintf(stderr, "swtfb-pinpad: touch %s (%s) X %d..%d Y %d..%d\n",
                        path, nm, t_minx, t_maxx, t_miny, t_maxy);
            }
            if (ioctl(f, EVIOCGRAB, 1) < 0 && debug)
                fprintf(stderr, "swtfb-pinpad: touch grab failed: %s\n",
                        strerror(errno));
            fd = f;
            continue; /* keep scanning: the pen may come after the touch dev */
        }
        if (keybit[BTN_TOOL_PEN / (8 * sizeof(long))] &
            (1UL << (BTN_TOOL_PEN % (8 * sizeof(long))))) {
            if (debug) {
                char nm[80] = "?";
                ioctl(f, EVIOCGNAME(sizeof nm), nm);
                fprintf(stderr, "swtfb-pinpad: grabbing pen %s (%s)\n", path, nm);
            }
            ioctl(f, EVIOCGRAB, 1);
            continue; /* fd intentionally held open, never read */
        }
        close(f);
    }
    closedir(d);
    return fd;
}

/* Key hit at panel coords, or -1. */
static int hit(int x, int y) {
    if (x < pad_x || x >= pad_x + 3 * cell || y < pad_y || y >= pad_y + 4 * cell)
        return -1;
    int col = (x - pad_x) / cell, row = (y - pad_y) / cell;
    if (row < 3) return row * 3 + col;
    if (col == 1) return 10;
    if (col == 2) return 11;
    return -1;
}

static int scale_x(int v) { return (int)((int64_t)(v - t_minx) * (W - 1) / (t_maxx - t_minx)); }
static int scale_y(int v) { return (int)((int64_t)(v - t_miny) * (H - 1) / (t_maxy - t_miny)); }

/* Coordinates persist across contacts (t_x/t_y): evdev suppresses ABS events
 * whose value is unchanged, so a same-spot tap emits no X/Y — the last-seen
 * value IS the current one. Seeded once at startup from the quiescent slot
 * state (see open_touch). */
static int t_x = -1, t_y = -1;

/* Block until a new contact's packet completes (SYN_REPORT after touch-DOWN),
 * return the key hit (or -1), then swallow events until the contact lifts.
 * Registration at SYN, not at the down event: the packet's own X/Y must be
 * consumed first (queue-consistent even under event backlog). */
static int read_tap(void) {
    struct input_event ev;
    int down = 0;
    for (;;) {
        if (read(t_fd, &ev, sizeof ev) != (ssize_t)sizeof ev) return -1;
        if (ev.type == EV_ABS && ev.code == ABS_MT_SLOT) {
            cur_slot = ev.value;
        } else if (ev.type == EV_ABS && ev.code == ABS_MT_TRACKING_ID) {
            if (ev.value >= 0) down = 1;
        } else if (ev.type == EV_KEY && ev.code == BTN_TOUCH && ev.value == 1) {
            down = 1; /* protocol-A style */
        } else if (ev.type == EV_ABS && ev.code == ABS_MT_POSITION_X) {
            t_x = scale_x(ev.value);
        } else if (ev.type == EV_ABS && ev.code == ABS_MT_POSITION_Y) {
            t_y = scale_y(ev.value);
        } else if (ev.type == EV_SYN && ev.code == SYN_REPORT && down) {
            if (t_x < 0 || t_y < 0) { down = 0; continue; } /* no position known yet */
            int k = hit(t_x, t_y);
            if (debug)
                fprintf(stderr, "swtfb-pinpad: tap (%d,%d) -> key %d\n", t_x, t_y, k);
            /* swallow until lift so a held finger is one keypress */
            while (read(t_fd, &ev, sizeof ev) == (ssize_t)sizeof ev) {
                if ((ev.type == EV_ABS && ev.code == ABS_MT_TRACKING_ID && ev.value < 0) ||
                    (ev.type == EV_KEY && ev.code == BTN_TOUCH && ev.value == 0))
                    break;
                if (ev.type == EV_ABS && ev.code == ABS_MT_POSITION_X) t_x = scale_x(ev.value);
                if (ev.type == EV_ABS && ev.code == ABS_MT_POSITION_Y) t_y = scale_y(ev.value);
            }
            return k;
        }
    }
}

/* ── main ───────────────────────────────────────────────────────────────── */
int main(int argc, char **argv) {
    debug = getenv("SWTFB_PINPAD_DEBUG") != NULL;

    if (argc == 4 && !strcmp(argv[1], "--set")) {
        size_t len = strlen(argv[3]);
        if (len < MIN_PIN || len > MAX_PIN ||
            strspn(argv[3], "0123456789") != len) {
            fprintf(stderr,
                    "swtfb-pinpad: PIN must be %d-%d digits (0-9 only — the "
                    "on-screen pad cannot enter anything else)\n",
                    MIN_PIN, MAX_PIN);
            return 2;
        }
        char *salt = crypt_gensalt("$6$", 0, NULL, 0);
        if (!salt) { perror("crypt_gensalt"); return 1; }
        char *hash = crypt(argv[3], salt);
        if (!hash || hash[0] == '*') { perror("crypt"); return 1; }
        FILE *f = fopen(argv[2], "w");
        if (!f) { perror(argv[2]); return 1; }
        /* line 1: digit count (lets the pad verify + clear at the exact
         * length, vendor-style — this is not a meaningful secret). line 2:
         * the salted hash. */
        fprintf(f, "%zu\n%s\n", len, hash);
        fclose(f);
        chmod(argv[2], 0600);
        printf("swtfb-pinpad: PIN set in %s\n", argv[2]);
        return 0;
    }
    if (argc != 2) {
        fprintf(stderr, "usage: %s <pin-file> | %s --set <pin-file> <pin>\n",
                argv[0], argv[0]);
        return 2;
    }

    /* pin file is two lines: digit count, then the salted hash. Anything
     * else (missing, malformed) means no valid PIN → no lock. */
    char l1[256], stored[256];
    int pin_len = 0;
    FILE *pf = fopen(argv[1], "r");
    if (!pf) return 0;
    if (fgets(l1, sizeof l1, pf) && fgets(stored, sizeof stored, pf)) {
        pin_len = atoi(l1);
        stored[strcspn(stored, "\n")] = 0;
    }
    fclose(pf);
    if (pin_len < MIN_PIN || pin_len > MAX_PIN || stored[0] != '$')
        return 0;

    W = env_dim("SWTFB_WIDTH");
    H = env_dim("SWTFB_HEIGHT");
    layout();

    int fbfd = open(SHM_PATH, O_RDWR);
    if (fbfd < 0) { perror("swtfb-pinpad: open " SHM_PATH); return 1; }
    fb = mmap(NULL, (size_t)W * H * 2, PROT_WRITE, MAP_SHARED, fbfd, 0);
    if (fb == MAP_FAILED) { perror("swtfb-pinpad: mmap"); return 1; }
    close(fbfd);

    ipc = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un a = { .sun_family = AF_UNIX };
    strncpy(a.sun_path, IPC_PATH, sizeof(a.sun_path) - 1);
    if (connect(ipc, (struct sockaddr *)&a, sizeof a) < 0) {
        perror("swtfb-pinpad: connect " IPC_PATH);
        return 1;
    }

#ifdef PREVIEW_ONLY
    /* Design preview build (-DPREVIEW_ONLY): draw once into the framebuffer
     * and exit — no touch device needed. */
    draw_all();
    return 0;
#endif

    t_fd = open_touch();
    if (t_fd < 0) {
        fprintf(stderr, "swtfb-pinpad: no touch device — cannot lock, letting boot proceed\n");
        return 0;
    }
    /* Seed the persisted position from the (quiescent) slot state, covering a
     * first tap whose coordinates the kernel suppresses as unchanged. */
    {
        int v;
        if ((v = mt_slot_val(ABS_MT_POSITION_X, cur_slot)) >= 0) t_x = scale_x(v);
        if ((v = mt_slot_val(ABS_MT_POSITION_Y, cur_slot)) >= 0) t_y = scale_y(v);
    }

    /* Snapshot the frame the pad paints over and restore it on unlock: the
     * reader app is not told a suspend happened (button-path suspends bypass
     * it), so nothing else repaints — without this the dead pad image stays
     * on the panel while the app runs live underneath. */
    uint16_t *saved = malloc((size_t)W * H * 2);
    if (saved) memcpy(saved, fb, (size_t)W * H * 2);

    char pin[MAX_PIN + 1];
    int n = 0, fails = 0, fail_flag = 0, flashed = -1;
    draw_all();
    time_t last = time(NULL);
    for (;;) {
        int k = read_tap();
        if (k < 0) continue;

        /* A tap after a long idle → the panel may have been blanked by an
         * external refresh; the pad only repaints the dots band per key, so
         * redraw the whole thing before handling this tap. */
        time_t now = time(NULL);
        if (now - last > 20) { draw_all(); n = 0; fail_flag = 0; flashed = -1; }
        last = now;

        /* Press highlight held until the NEXT tap: an immediate restore races
         * the engine's framebuffer sampling and the flash never renders. */
        if (flashed >= 0) unflash_key(flashed);
        flash_key(k);
        flashed = k;

        if (k <= 8 || k == 10) {           /* digit */
            if (n < pin_len) pin[n++] = (k == 10) ? '0' : (char)('1' + k);
            fail_flag = 0;
            if (n == pin_len) {           /* full entry: verify + clear */
                pin[n] = 0;
                char *h = crypt(pin, stored);
                if (debug)
                    fprintf(stderr, "swtfb-pinpad: verify -> %s\n",
                            (h && !strcmp(h, stored)) ? "MATCH" : "no");
                if (h && !strcmp(h, stored)) {
                    memset(pin, 0, sizeof pin);
                    if (saved) {
                        memcpy(fb, saved, (size_t)W * H * 2);
                        refresh((rect_t){0, 0, (uint32_t)W, (uint32_t)H}, WF_GC16);
                    }
                    return 0;
                }
                n = 0;
                fail_flag = 1;
                if (++fails % 3 == 0) sleep(2);
            }
        } else if (k == 11) {              /* backspace */
            if (n > 0) n--;
        }
        if (fail_flag) { unflash_key(k); flashed = -1; } /* wrong PIN: clear the highlight */
        draw_dots(n, fail_flag);
        refresh(dots_band(), WF_DU);
    }
}
