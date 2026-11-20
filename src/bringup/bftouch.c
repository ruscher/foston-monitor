/*
 * bftouch - touchscreen calibration and test for the FOSTON FS-707DC
 * (BigFoston OS). Draws on /dev/fb0 and reads /dev/input/event0.
 *
 *   bftouch cal  [file]          5-point calibration, saved in tslib pointercal format
 *   bftouch test [file] [secs]   draws where the finger touches, using the calibration
 *
 * pointercal: "a0 a1 a2 a3 a4 a5 a6 W H", x = (a2 + a0*X + a1*Y) / a6,
 *                                          y = (a5 + a3*X + a4*Y) / a6.
 *
 * The framebuffer of this board starts 0x30 bytes into its page, so the
 * mmap'ed pointer is moved by smem_start & (page size - 1).
 * Kernel messages drawn by the boot console are paused while it runs.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <errno.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <linux/input.h>
#include <math.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define DEFAULT_CAL	"/etc/bigfoston/touchcal"
#define PAUSE_PARAM	"/sys/module/mt3351_bootcon/parameters/paused"
#define MAX_SAMPLES	512
#define MAX_ERROR_PX	12.0

static uint16_t *fb;
static unsigned int W, H, stride;	/* stride in pixels */
static int ev_fd = -1;

#define RGB(r, g, b)	((uint16_t)((((r) >> 3) << 11) | (((g) >> 2) << 5) | ((b) >> 3)))
#define C_BG		RGB(24, 32, 40)
#define C_FG		RGB(230, 230, 230)
#define C_ACCENT	RGB(52, 132, 228)
#define C_OK		RGB(80, 200, 120)
#define C_BAD		RGB(230, 80, 70)

static void set_pause(int on)
{
	int fd = open(PAUSE_PARAM, O_WRONLY);

	if (fd >= 0) {
		if (write(fd, on ? "1" : "0", 1) < 0)
			perror(PAUSE_PARAM);
		close(fd);
	}
}

static void restore(void)
{
	set_pause(0);
}

static void on_signal(int sig)
{
	(void)sig;
	restore();
	_exit(1);
}

static void fill(int x0, int y0, int w, int h, uint16_t c)
{
	for (int y = y0; y < y0 + h; y++) {
		if (y < 0 || y >= (int)H)
			continue;
		for (int x = x0; x < x0 + w; x++)
			if (x >= 0 && x < (int)W)
				fb[y * stride + x] = c;
	}
}

static void clear(uint16_t c)
{
	fill(0, 0, W, H, c);
}

static void cross(int x, int y, uint16_t c)
{
	fill(x - 20, y - 1, 41, 3, c);
	fill(x - 1, y - 20, 3, 41, c);
	fill(x - 6, y - 6, 13, 2, c);
	fill(x - 6, y + 5, 13, 2, c);
	fill(x - 6, y - 6, 2, 13, c);
	fill(x + 5, y - 6, 2, 13, c);
}

static int fb_open(void)
{
	struct fb_fix_screeninfo fix;
	struct fb_var_screeninfo var;
	long page = sysconf(_SC_PAGESIZE);
	unsigned long off;
	uint8_t *map;
	int fd = open("/dev/fb0", O_RDWR);

	if (fd < 0 || ioctl(fd, FBIOGET_FSCREENINFO, &fix) || ioctl(fd, FBIOGET_VSCREENINFO, &var)) {
		perror("/dev/fb0");
		return -1;
	}
	if (var.bits_per_pixel != 16) {
		fprintf(stderr, "bftouch: %u bpp, only 16 is supported\n", var.bits_per_pixel);
		return -1;
	}
	off = fix.smem_start & (page - 1);
	map = mmap(NULL, fix.smem_len + off, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (map == MAP_FAILED) {
		perror("mmap /dev/fb0");
		return -1;
	}
	fb = (uint16_t *)(map + off);
	W = var.xres;
	H = var.yres;
	stride = fix.line_length / 2;
	printf("fb0: %ux%u, stride %u, smem 0x%lx+0x%x (page offset 0x%lx)\n",
	       W, H, fix.line_length, (unsigned long)fix.smem_start, fix.smem_len, off);
	return 0;
}

/* blocks until the next complete touch (down..up); stores the raw samples */
static int read_touch(int *xs, int *ys, int *n)
{
	struct input_event ev;
	int x = -1, y = -1, down = 0;

	*n = 0;
	for (;;) {
		if (read(ev_fd, &ev, sizeof(ev)) != sizeof(ev)) {
			perror("event0");
			return -1;
		}
		if (ev.type == EV_ABS && ev.code == ABS_X)
			x = ev.value;
		else if (ev.type == EV_ABS && ev.code == ABS_Y)
			y = ev.value;
		else if (ev.type == EV_KEY && ev.code == BTN_TOUCH) {
			down = ev.value;
			if (!down && *n)
				return 0;
		} else if (ev.type == EV_SYN && down && x >= 0 && *n < MAX_SAMPLES) {
			xs[*n] = x;
			ys[*n] = y;
			(*n)++;
		}
	}
}

static int cmp_int(const void *a, const void *b)
{
	return *(const int *)a - *(const int *)b;
}

/* median of the second half: the first readings drift while the finger presses down */
static int settle(int *v, int n)
{
	int h = n / 2;

	qsort(v + h, n - h, sizeof(int), cmp_int);
	return v[h + (n - h) / 2];
}

/* least squares for out = a*X + b*Y + c over n points (normal equations, Cramer) */
static void fit(const double *X, const double *Y, const double *out, int n, double r[3])
{
	double sxx = 0, sxy = 0, sx = 0, syy = 0, sy = 0, so = 0, sxo = 0, syo = 0;

	for (int i = 0; i < n; i++) {
		sxx += X[i] * X[i]; sxy += X[i] * Y[i]; sx += X[i];
		syy += Y[i] * Y[i]; sy += Y[i];
		so += out[i]; sxo += X[i] * out[i]; syo += Y[i] * out[i];
	}
	double m[3][3] = { { sxx, sxy, sx }, { sxy, syy, sy }, { sx, sy, n } };
	double v[3] = { sxo, syo, so };
	double det = m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1])
		   - m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0])
		   + m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);

	for (int k = 0; k < 3; k++) {
		double t[3][3];

		memcpy(t, m, sizeof(t));
		for (int i = 0; i < 3; i++)
			t[i][k] = v[i];
		r[k] = (t[0][0] * (t[1][1] * t[2][2] - t[1][2] * t[2][1])
		      - t[0][1] * (t[1][0] * t[2][2] - t[1][2] * t[2][0])
		      + t[0][2] * (t[1][0] * t[2][1] - t[1][1] * t[2][0])) / det;
	}
}

static long cal[7];

static void apply(int X, int Y, int *x, int *y)
{
	*x = (int)((cal[2] + cal[0] * (long long)X + cal[1] * (long long)Y) / cal[6]);
	*y = (int)((cal[5] + cal[3] * (long long)X + cal[4] * (long long)Y) / cal[6]);
}

static int load_cal(const char *path)
{
	FILE *f = fopen(path, "r");
	int ok;

	if (!f) {
		perror(path);
		return -1;
	}
	ok = fscanf(f, "%ld %ld %ld %ld %ld %ld %ld", &cal[0], &cal[1], &cal[2], &cal[3],
		    &cal[4], &cal[5], &cal[6]) == 7 && cal[6];
	fclose(f);
	if (!ok)
		fprintf(stderr, "%s: invalid calibration\n", path);
	return ok ? 0 : -1;
}

static int calibrate(const char *path)
{
	static int xs[MAX_SAMPLES], ys[MAX_SAMPLES];
	const int m = 50;
	int tx[5] = { m, W - 1 - m, W - 1 - m, m, W / 2 };
	int ty[5] = { m, m, H - 1 - m, H - 1 - m, H / 2 };
	double X[5], Y[5], px[5], py[5], a[3], b[3], worst;
	int n;

	for (int attempt = 1; attempt <= 3; attempt++) {
		clear(C_BG);
		for (int i = 0; i < 5; i++) {
			cross(tx[i], ty[i], C_ACCENT);
			do {
				if (read_touch(xs, ys, &n))
					return -1;
			} while (n < 4);	/* ignore taps too short to settle */
			X[i] = settle(xs, n);
			Y[i] = settle(ys, n);
			px[i] = tx[i];
			py[i] = ty[i];
			printf("ponto %d (%d,%d): bruto %.0f,%.0f de %d amostras\n",
			       i + 1, tx[i], ty[i], X[i], Y[i], n);
			cross(tx[i], ty[i], C_OK);
		}
		fit(X, Y, px, 5, a);
		fit(X, Y, py, 5, b);
		cal[6] = 65536;
		cal[0] = lround(a[0] * cal[6]); cal[1] = lround(a[1] * cal[6]); cal[2] = lround(a[2] * cal[6]);
		cal[3] = lround(b[0] * cal[6]); cal[4] = lround(b[1] * cal[6]); cal[5] = lround(b[2] * cal[6]);

		worst = 0;
		for (int i = 0; i < 5; i++) {
			int x, y;

			apply(X[i], Y[i], &x, &y);
			double e = hypot(x - tx[i], y - ty[i]);

			printf("ponto %d: erro %.1f px\n", i + 1, e);
			if (e > worst)
				worst = e;
		}
		if (worst <= MAX_ERROR_PX) {
			FILE *f = fopen(path, "w");

			if (!f) {
				perror(path);
				return -1;
			}
			fprintf(f, "%ld %ld %ld %ld %ld %ld %ld %u %u\n", cal[0], cal[1], cal[2],
				cal[3], cal[4], cal[5], cal[6], W, H);
			fclose(f);
			printf("calibracao salva em %s (erro maximo %.1f px)\n", path, worst);
			clear(C_OK);
			usleep(600000);
			return 0;
		}
		printf("tentativa %d: erro maximo %.1f px > %.0f, repetindo\n", attempt, worst, MAX_ERROR_PX);
		clear(C_BAD);
		usleep(800000);
	}
	return -1;
}

static int test(int secs)
{
	struct input_event ev;
	int X = -1, Y = -1, down = 0, lx = -1, ly = -1, touches = 0;
	time_t end = time(NULL) + secs;

	clear(C_BG);
	for (int x = 0; x < (int)W; x += 100)
		fill(x, 0, 1, H, RGB(50, 60, 70));
	for (int y = 0; y < (int)H; y += 100)
		fill(0, y, W, 1, RGB(50, 60, 70));
	while (time(NULL) < end) {
		struct pollfd p = { .fd = ev_fd, .events = POLLIN };

		if (poll(&p, 1, 500) <= 0)
			continue;
		if (read(ev_fd, &ev, sizeof(ev)) != sizeof(ev))
			return -1;
		if (ev.type == EV_ABS && ev.code == ABS_X)
			X = ev.value;
		else if (ev.type == EV_ABS && ev.code == ABS_Y)
			Y = ev.value;
		else if (ev.type == EV_KEY && ev.code == BTN_TOUCH) {
			down = ev.value;
			if (down)
				touches++;
			else if (lx >= 0) {
				printf("toque %d solto em %d,%d\n", touches, lx, ly);
				cross(lx, ly, C_ACCENT);
			}
			lx = ly = -1;
		} else if (ev.type == EV_SYN && down && X >= 0) {
			int x, y;

			apply(X, Y, &x, &y);
			if (lx >= 0) {	/* line from the previous point */
				int steps = abs(x - lx) > abs(y - ly) ? abs(x - lx) : abs(y - ly);

				for (int s = 1; s <= steps; s++)
					fill(lx + (x - lx) * s / steps - 1, ly + (y - ly) * s / steps - 1, 3, 3, C_FG);
			}
			fill(x - 2, y - 2, 5, 5, C_OK);
			lx = x;
			ly = y;
		}
	}
	printf("%d toques\n", touches);
	return 0;
}

int main(int argc, char **argv)
{
	const char *mode = argc > 1 ? argv[1] : "";
	const char *path = argc > 2 ? argv[2] : DEFAULT_CAL;
	int ret;

	if (strcmp(mode, "cal") && strcmp(mode, "test")) {
		fprintf(stderr, "uso: bftouch cal [arquivo] | bftouch test [arquivo] [segundos]\n");
		return 2;
	}
	setvbuf(stdout, NULL, _IOLBF, 0);
	if (fb_open())
		return 1;
	ev_fd = open("/dev/input/event0", O_RDONLY);
	if (ev_fd < 0) {
		perror("/dev/input/event0");
		return 1;
	}
	if (!strcmp(mode, "test") && load_cal(path))
		return 1;
	if (!strcmp(mode, "cal")) {
		char dir[256];
		char *slash;

		snprintf(dir, sizeof(dir), "%s", path);
		slash = strrchr(dir, '/');
		if (slash && slash != dir) {
			*slash = 0;
			mkdir(dir, 0755);
		}
	}
	signal(SIGINT, on_signal);
	signal(SIGTERM, on_signal);
	set_pause(1);
	atexit(restore);
	ret = !strcmp(mode, "cal") ? calibrate(path) : test(argc > 3 ? atoi(argv[3]) : 60);
	clear(C_BG);
	return ret ? 1 : 0;
}
