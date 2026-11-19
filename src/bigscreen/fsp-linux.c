/*
 * bigfoston-bigscreen - Big Screen Monitor Display client for BigFoston OS.
 *
 * The Linux counterpart of bsmd-ce (Windows CE): speaks FSP v1 (fsp.h, shared
 * byte for byte with big-screen-monitor-display) on a serial device - on the
 * FOSTON the USB gadget port (/dev/ttyGS0) - draws what the PC sends into the
 * framebuffer and reports touches from an evdev device.
 *
 *   bigfoston-bigscreen --tty /dev/ttyGS0 [--fb /dev/fb0 | --fb-file F:800x480]
 *                       [--input /dev/input/event0] [--once]
 *
 * --fb-file maps a plain file as an RGB565 framebuffer (host tests).
 * fsp.c/fsp.h are copied unchanged from big-screen-monitor-display
 * (device-clients/wince/src); keep them in sync.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <linux/input.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <sys/sysinfo.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include "fsp.h"

#define CLIENT_VERSION "bigfoston-bigscreen 0.1.0"
#define CREDIT_STEP (8u * 1024u)

static int g_tty = -1;
static uint16_t g_seq;
static volatile sig_atomic_t g_quit;

/* ------------------------------------------------------------- display */

static struct {
    uint16_t *fb;            /* mapped framebuffer */
    size_t fb_len;
    int width, height, stride;   /* stride in pixels */
    uint16_t *shadow;        /* what the host drew, shown on PRESENT */
} d;

static int fb_open_device(const char *path)
{
    struct fb_var_screeninfo var;
    struct fb_fix_screeninfo fix;
    int fd = open(path, O_RDWR);
    if (fd < 0)
        return -1;
    if (ioctl(fd, FBIOGET_VSCREENINFO, &var) || ioctl(fd, FBIOGET_FSCREENINFO, &fix) || var.bits_per_pixel != 16) {
        fprintf(stderr, "%s: not a 16-bit framebuffer\n", path);
        close(fd);
        return -1;
    }
    d.width = (int)var.xres;
    d.height = (int)var.yres;
    d.stride = (int)(fix.line_length / 2);
    d.fb_len = fix.smem_len;
    d.fb = mmap(NULL, d.fb_len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    return d.fb == MAP_FAILED ? -1 : 0;
}

static int fb_open_file(const char *spec)
{
    char path[256];
    const char *colon = strrchr(spec, ':');
    int fd;
    if (!colon || (size_t)(colon - spec) >= sizeof(path) || sscanf(colon + 1, "%dx%d", &d.width, &d.height) != 2)
        return -1;
    memcpy(path, spec, (size_t)(colon - spec));
    path[colon - spec] = 0;
    d.stride = d.width;
    d.fb_len = (size_t)d.width * d.height * 2;
    fd = open(path, O_RDWR | O_CREAT, 0644);
    if (fd < 0 || ftruncate(fd, (off_t)d.fb_len))
        return -1;
    d.fb = mmap(NULL, d.fb_len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    return d.fb == MAP_FAILED ? -1 : 0;
}

static void present(const fsp_box *b)
{
    int y;
    if (b->x1 <= b->x0 || b->y1 <= b->y0)
        return;
    for (y = b->y0; y < b->y1; y++)
        memcpy(d.fb + (size_t)y * d.stride + b->x0, d.shadow + (size_t)y * d.width + b->x0,
               (size_t)(b->x1 - b->x0) * 2);
}

/* ------------------------------------------------------------ transport */

static int write_all(const uint8_t *p, size_t n)
{
    while (n) {
        ssize_t w = write(g_tty, p, n);
        if (w < 0) {
            if (errno == EINTR || errno == EAGAIN) {
                struct pollfd pfd = {g_tty, POLLOUT, 0};
                poll(&pfd, 1, 100);
                continue;
            }
            return -1;
        }
        p += w;
        n -= (size_t)w;
    }
    return 0;
}

static void send_msg(uint8_t type, const uint8_t *payload, uint32_t len)
{
    uint8_t header[FSP_HEADER];
    fsp_header(header, type, g_seq++, payload, len);
    if (write_all(header, FSP_HEADER) == 0 && len)
        write_all(payload, len);
}

static uint32_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000u + ts.tv_nsec / 1000000u);
}

static void send_hello_ack(void)
{
    uint8_t p[512];
    size_t n = 32;
    struct utsname u;
    struct sysinfo si;
    unsigned major = 0, minor = 0, patch = 0;
    uname(&u);
    sscanf(u.release, "%u.%u.%u", &major, &minor, &patch);
    sysinfo(&si);
    memset(p, 0, sizeof(p));
    fsp_put16(p, FSP_VERSION);
    fsp_put16(p + 2, (uint16_t)d.width);
    fsp_put16(p + 4, (uint16_t)d.height);
    fsp_put16(p + 6, 16);
    fsp_put32(p + 8, major);               /* the Windows CE client reports its OS version here */
    fsp_put32(p + 12, minor);
    fsp_put32(p + 16, patch);
    fsp_put32(p + 20, (uint32_t)((uint64_t)si.totalram * si.mem_unit / 1024));
    fsp_put32(p + 24, (uint32_t)((uint64_t)si.freeram * si.mem_unit / 1024));
    p[28] = 0;                             /* brightness control: not yet */
    p[29] = 100;
    p[30] = 1;                             /* touch */
    p[31] = 2;                             /* sends FSP_CREDIT */
#define ADD_TEXT(s) do { size_t l_ = strlen(s); if (n + l_ + 1 < sizeof(p)) { memcpy(p + n, s, l_ + 1); n += l_ + 1; } } while (0)
    ADD_TEXT(CLIENT_VERSION);
    ADD_TEXT("BigFoston OS");
    ADD_TEXT(u.release);
    ADD_TEXT("ttyGS0");
    ADD_TEXT("");
#undef ADD_TEXT
    send_msg(FSP_HELLO_ACK, p, (uint32_t)n);
}

static void send_touch(int kind, int x, int y)
{
    uint8_t p[12];
    p[0] = (uint8_t)kind;
    p[1] = 0;
    fsp_put16(p + 2, (uint16_t)(x < 0 ? 0 : x));
    fsp_put16(p + 4, (uint16_t)(y < 0 ? 0 : y));
    fsp_put16(p + 6, 0);
    fsp_put32(p + 8, now_ms());
    send_msg(FSP_TOUCH, p, 12);
}

/* ---------------------------------------------------------------- touch */

static struct {
    int fd;
    int min_x, max_x, min_y, max_y;
    int x, y, down, was_down, moved;
} t = {-1, 0, 0, 0, 0, 0, 0, 0, 0, 0};

static void touch_open(const char *path)
{
    struct input_absinfo a;
    t.fd = open(path, O_RDONLY | O_NONBLOCK);
    if (t.fd < 0) {
        perror(path);
        return;
    }
    t.max_x = d.width - 1;
    t.max_y = d.height - 1;
    if (!ioctl(t.fd, EVIOCGABS(ABS_X), &a) && a.maximum > a.minimum) {
        t.min_x = a.minimum;
        t.max_x = a.maximum;
    }
    if (!ioctl(t.fd, EVIOCGABS(ABS_Y), &a) && a.maximum > a.minimum) {
        t.min_y = a.minimum;
        t.max_y = a.maximum;
    }
}

static int scale(int v, int lo, int hi, int size)
{
    long r = (long)(v - lo) * (size - 1) / (hi - lo ? hi - lo : 1);
    return r < 0 ? 0 : r >= size ? size - 1 : (int)r;
}

static void touch_read(int connected)
{
    struct input_event ev;
    while (read(t.fd, &ev, sizeof(ev)) == sizeof(ev)) {
        if (ev.type == EV_ABS && ev.code == ABS_X) {
            t.x = scale(ev.value, t.min_x, t.max_x, d.width);
            t.moved = 1;
        } else if (ev.type == EV_ABS && ev.code == ABS_Y) {
            t.y = scale(ev.value, t.min_y, t.max_y, d.height);
            t.moved = 1;
        } else if (ev.type == EV_KEY && ev.code == BTN_TOUCH) {
            t.down = ev.value != 0;
        } else if (ev.type == EV_SYN && ev.code == SYN_REPORT && connected) {
            if (t.down && !t.was_down)
                send_touch(FSP_TOUCH_DOWN, t.x, t.y);
            else if (t.down && t.moved)
                send_touch(FSP_TOUCH_MOVE, t.x, t.y);
            else if (!t.down && t.was_down)
                send_touch(FSP_TOUCH_UP, t.x, t.y);
            t.was_down = t.down;
            t.moved = 0;
        }
    }
}

/* ----------------------------------------------------------------- main */

static int tty_open(const char *path)
{
    struct termios tio;
    int fd = open(path, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd < 0)
        return -1;
    if (!tcgetattr(fd, &tio)) {
        cfmakeraw(&tio);
        tcsetattr(fd, TCSANOW, &tio);
    }
    return fd;
}

static void on_signal(int sig)
{
    (void)sig;
    g_quit = 1;
}

int main(int argc, char **argv)
{
    const char *tty = "/dev/ttyGS0", *fb = "/dev/fb0", *fb_file = NULL, *input = NULL;
    fsp_parser parser;
    fsp_box dirty = {0, 0, 0, 0};
    uint8_t *rx, *scratch, chunk[4096], reply[12];
    size_t scratch_len;
    uint32_t consumed = 0, credited = 0;
    int connected = 0, once = 0, i;

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--tty") && i + 1 < argc) tty = argv[++i];
        else if (!strcmp(argv[i], "--fb") && i + 1 < argc) fb = argv[++i];
        else if (!strcmp(argv[i], "--fb-file") && i + 1 < argc) fb_file = argv[++i];
        else if (!strcmp(argv[i], "--input") && i + 1 < argc) input = argv[++i];
        else if (!strcmp(argv[i], "--once")) once = 1;
        else {
            fprintf(stderr, "usage: %s [--tty DEV] [--fb DEV | --fb-file FILE:WxH] [--input EVDEV] [--once]\n", argv[0]);
            return 2;
        }
    }
    if ((fb_file ? fb_open_file(fb_file) : fb_open_device(fb)) < 0) {
        fprintf(stderr, "cannot open framebuffer %s\n", fb_file ? fb_file : fb);
        return 1;
    }
    g_tty = tty_open(tty);
    if (g_tty < 0) {
        perror(tty);
        return 1;
    }
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGPIPE, SIG_IGN);
    d.shadow = calloc((size_t)d.width * d.height, 2);
    scratch_len = (size_t)d.width * d.height * 2;
    scratch = malloc(scratch_len);
    rx = malloc(FSP_HEADER + FSP_MAX_PAYLOAD);
    if (!d.shadow || !scratch || !rx)
        return 1;
    if (input)
        touch_open(input);
    fsp_parser_init(&parser, rx, FSP_HEADER + FSP_MAX_PAYLOAD);
    fprintf(stderr, "%s: %dx%d on %s\n", CLIENT_VERSION, d.width, d.height, tty);

    while (!g_quit) {
        struct pollfd pfd[2] = {{g_tty, POLLIN, 0}, {t.fd, POLLIN, 0}};
        int n = poll(pfd, t.fd >= 0 ? 2 : 1, 1000);
        if (n < 0 && errno != EINTR)
            break;
        if (t.fd >= 0 && (pfd[1].revents & POLLIN))
            touch_read(connected);
        if (pfd[0].revents & (POLLHUP | POLLERR))
            break;
        if (!(pfd[0].revents & POLLIN))
            continue;
        for (;;) {
            ssize_t got = read(g_tty, chunk, sizeof(chunk));
            size_t off = 0;
            fsp_msg m;
            int r;
            if (got <= 0)
                break;
            while (off < (size_t)got) {
                off += fsp_feed(&parser, chunk + off, (size_t)got - off);
                while ((r = fsp_next(&parser, &m)) != FSP_NEED_MORE) {
                    fsp_surface s = {d.shadow, d.width, d.height, d.width};
                    if (r == FSP_BAD_CRC) {
                        fsp_put16(reply, m.seq);
                        reply[2] = FSP_NAK_CRC;
                        send_msg(FSP_NAK, reply, 3);
                        continue;
                    }
                    if (m.type == FSP_HELLO)
                        consumed = credited = 0;
                    else
                        consumed += FSP_HEADER + m.len;
                    switch (m.type) {
                    case FSP_HELLO:
                        connected = 1;
                        dirty.x0 = dirty.x1 = dirty.y0 = dirty.y1 = 0;
                        send_hello_ack();
                        break;
                    case FSP_PING:
                        if (m.len >= 4) {
                            memcpy(reply, m.payload, 4);
                            fsp_put32(reply + 4, now_ms());
                            send_msg(FSP_PONG, reply, 8);
                        }
                        break;
                    case FSP_RECT:
                    case FSP_FILL: {
                        int err = fsp_apply(&s, m.type, m.payload, m.len, scratch, scratch_len, &dirty);
                        if (err) {
                            fsp_put16(reply, m.seq);
                            reply[2] = (uint8_t)err;
                            send_msg(FSP_NAK, reply, 3);
                        }
                        break;
                    }
                    case FSP_PRESENT: {
                        uint32_t start = now_ms();
                        if (m.len < 4)
                            break;
                        present(&dirty);
                        dirty.x0 = dirty.x1 = dirty.y0 = dirty.y1 = 0;
                        memcpy(reply, m.payload, 4);
                        fsp_put32(reply + 4, 0);
                        fsp_put32(reply + 8, now_ms() - start);
                        send_msg(FSP_PRESENTED, reply, 12);
                        break;
                    }
                    case FSP_TOUCHCFG:
                    case FSP_BRIGHTNESS:
                    case FSP_CALIBRATE:
                        break;          /* not wired yet on Linux */
                    case FSP_BYE:
                        connected = 0;
                        if (m.len >= 1 && m.payload[0] == 1)
                            g_quit = 1;
                        if (once)
                            g_quit = 1;
                        break;
                    default:
                        fsp_put16(reply, m.seq);
                        reply[2] = FSP_NAK_UNKNOWN;
                        send_msg(FSP_NAK, reply, 3);
                        break;
                    }
                    fsp_consume(&parser);
                    if (consumed - credited >= CREDIT_STEP || (m.type == FSP_PRESENT && consumed != credited)) {
                        credited = consumed;
                        fsp_put32(reply, consumed);
                        send_msg(FSP_CREDIT, reply, 4);
                    }
                }
            }
        }
    }
    return 0;
}
