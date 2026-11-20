/*
 * bftest - BigFoston OS userland bring-up test, run as PID 1 (rdinit=/bin/bftest).
 *
 * A single sleeping process ran for 20 min on the FS-707DC without any
 * problem, but the shell crashed as soon as it started children. This
 * program walks through the mechanisms one at a time and reports each step
 * on the kernel log (which the framebuffer console shows):
 *   1  checksum of its own code (is code in RAM changing?)
 *   2  fork + _exit + waitpid, no signal handler
 *   3  the same with a SIGCHLD handler installed (the shell's case)
 *   4  two processes ping-ponging over pipes (context switches), data checked
 *   5  fork + execve of another program
 *   7  copy-on-write isolation: a child overwrites 1 MB of heap and stack
 *      it shares with its parent after fork(); the parent's copy must not
 *      change (and the reverse for the child)
 *   8  r4-r11 held in a loop while SIGALRM fires every 1 ms (context
 *      save/restore), plus syscalls interrupted and restarted by signals
 *   9  BusyBox sh run as a child on small scripts: which construct dies
 *  10  unaligned 16/32-bit loads and stores at every offset, including
 *      across 32-byte cache lines and 4 KB pages, and memcpy/memset with
 *      odd sizes/alignments, checked byte by byte (the kernel's blake2s
 *      "align 1" self-test failed on the device; corrupted shell pointers
 *      keep only their top byte)
 *  11  large memcpy/memmove (0..1100 bytes, every src/dst alignment 0..7,
 *      near page boundaries) against a byte loop: musl's ARM memcpy uses
 *      ldm/stm blocks, pld and shift-merging for misaligned copies
 *  12  r4-r11 held across a blocking read() that SIGCHLD/SIGALRM handlers
 *      interrupt (syscall restart path); every crash of the shell had
 *      LR in the sigpage (sigreturn trampoline) with r4 = top byte only
 *  13  memset(p, 0, 8) via musl's byte-store order (0,7,1,2,6,5,3,4) millions
 *      of times over dirty memory, with timer signals and a busy sibling
 *      process: every shell crash had a freshly xzalloc'ed 8-byte object
 *      whose second word kept only its old top byte (lost byte store?)
 *   6  /bin/busybox in the page cache against per-page CRC32s computed on the
 *      PC at build time (/etc/busybox.pagecrc): a changed page is reported
 *      with its physical address (/proc/self/pagemap), to find a bus master
 *      left running by Windows CE that writes into RAM
 * then repeats, forever, printing a round counter.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <unistd.h>

static int kmsg = -1;

static uint32_t crc32_table[256];

static uint32_t crc32_buf(const uint8_t *p, size_t n)
{
    uint32_t c = 0xFFFFFFFFu;
    if (!crc32_table[1]) {
        uint32_t i, k, v;
        for (i = 0; i < 256; i++) {
            for (v = i, k = 0; k < 8; k++)
                v = v & 1 ? 0xEDB88320u ^ (v >> 1) : v >> 1;
            crc32_table[i] = v;
        }
    }
    while (n--)
        c = crc32_table[(c ^ *p++) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

static void say(const char *fmt, ...)
{
    char buf[200];
    va_list ap;
    int n = snprintf(buf, sizeof(buf), "bftest[%d]: ", getpid());
    va_start(ap, fmt);
    n += vsnprintf(buf + n, sizeof(buf) - (size_t)n, fmt, ap);
    va_end(ap);
    write(kmsg >= 0 ? kmsg : 1, buf, (size_t)n);   /* stdout when there is no /dev/kmsg (qemu) */
}

extern char __executable_start[], etext[];

static uint32_t code_sum(void)
{
    const uint32_t *p = (const uint32_t *)__executable_start;
    const uint32_t *end = (const uint32_t *)etext;
    uint32_t s = 0x12345678;
    while (p < end)
        s = (s << 5 | s >> 27) ^ *p++;
    return s;
}

static volatile sig_atomic_t chld_count;

static void on_chld(int sig)
{
    (void)sig;
    chld_count++;
}

static int forks(int n, const char *label)
{
    int i;
    for (i = 0; i < n; i++) {
        int status = 0;
        pid_t pid = fork();
        if (pid < 0) {
            say("%s: fork failed at %d: %s\n", label, i, strerror(errno));
            return -1;
        }
        if (pid == 0)
            _exit(i & 0x7f);
        while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
            ;
        if (!WIFEXITED(status) || WEXITSTATUS(status) != (i & 0x7f)) {
            say("%s: child %d bad status 0x%x\n", label, i, status);
            return -1;
        }
    }
    return 0;
}

static int pingpong(int rounds)
{
    int a[2], b[2], i;
    pid_t pid;
    if (pipe(a) || pipe(b))
        return -1;
    pid = fork();
    if (pid == 0) {
        uint32_t v;
        for (i = 0; i < rounds; i++) {
            if (read(a[0], &v, 4) != 4)
                _exit(2);
            v = v * 2654435761u + 1;
            if (write(b[1], &v, 4) != 4)
                _exit(3);
        }
        _exit(0);
    }
    for (i = 0; i < rounds; i++) {
        uint32_t v = (uint32_t)i * 7u, r;
        if (write(a[1], &v, 4) != 4 || read(b[0], &r, 4) != 4) {
            say("pingpong: pipe error at %d\n", i);
            return -1;
        }
        if (r != v * 2654435761u + 1) {
            say("pingpong: WRONG DATA at %d: %08x\n", i, r);
            return -1;
        }
    }
    waitpid(pid, NULL, 0);
    close(a[0]); close(a[1]); close(b[0]); close(b[1]);
    return 0;
}

static int execs(int n)
{
    int i;
    for (i = 0; i < n; i++) {
        int status = 0;
        pid_t pid = fork();
        if (pid == 0) {
            char *argv[] = {"true", NULL};
            execv("/bin/true", argv);
            _exit(127);
        }
        while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
            ;
        if (!WIFEXITED(status) || WEXITSTATUS(status)) {
            say("exec: run %d status 0x%x\n", i, status);
            return -1;
        }
    }
    return 0;
}

#define COW_WORDS (256 * 1024)		/* 1 MB */

static int cow_check(const volatile uint32_t *buf, uint32_t seed, const char *who, unsigned round)
{
    size_t i;
    int bad = 0;
    for (i = 0; i < COW_WORDS; i++) {
        uint32_t want = seed ^ (uint32_t)i * 2654435761u;
        if (buf[i] != want) {
            if (bad < 4)
                say("round %u step7 %s word %zu = %08x want %08x (va %p)\n", round, who, i, buf[i], want,
                    (void *)&buf[i]);
            bad++;
        }
    }
    return bad;
}

static void cow_fill(volatile uint32_t *buf, uint32_t seed)
{
    size_t i;
    for (i = 0; i < COW_WORDS; i++)
        buf[i] = seed ^ (uint32_t)i * 2654435761u;
}

static int cow_test(unsigned round)
{
    volatile uint32_t *buf = malloc(COW_WORDS * 4);
    int status = 0, bad;
    pid_t pid;
    int sync[2];
    char c;
    if (!buf || pipe(sync))
        return -1;
    cow_fill(buf, 0x11111111u);
    pid = fork();
    if (pid == 0) {
        /* child: overwrite everything, then check our own copy */
        cow_fill(buf, 0x22222222u);
        write(sync[1], "x", 1);
        usleep(200000);
        _exit(cow_check(buf, 0x22222222u, "child", round) ? 1 : 0);
    }
    read(sync[0], &c, 1);                 /* the child has written all of it */
    bad = cow_check(buf, 0x11111111u, "parent-after-child-write", round);
    cow_fill(buf, 0x33333333u);           /* parent writes while the child still lives */
    waitpid(pid, &status, 0);
    if (!WIFEXITED(status) || WEXITSTATUS(status))
        say("round %u step7 child saw its copy change (status 0x%x)\n", round, status);
    bad += cow_check(buf, 0x33333333u, "parent-final", round);
    free((void *)buf);
    close(sync[0]);
    close(sync[1]);
    return bad || !WIFEXITED(status) || WEXITSTATUS(status);
}

static volatile sig_atomic_t alarms;

static void on_alarm(int sig)
{
    (void)sig;
    alarms++;
}

/* Hold known values in r4-r11 for `loops` iterations; return the first
 * register found changed (4..11) or 0. Signals keep interrupting it. */
static int regs_hold(unsigned loops)
{
    register unsigned n __asm__("r0") = loops;
    register unsigned bad __asm__("r1") = 0;
    __asm__ __volatile__(
        "push  {r4-r11}\n"
        "ldr   r4, =0x44444444\n"
        "ldr   r5, =0x55555555\n"
        "ldr   r6, =0x66666666\n"
        "ldr   r7, =0x77777777\n"
        "ldr   r8, =0x88888888\n"
        "ldr   r9, =0x99999999\n"
        "ldr   r10, =0xaaaaaaaa\n"
        "ldr   r11, =0xbbbbbbbb\n"
        "1:\n"
        "ldr   r2, =0x44444444\n cmp r4, r2\n movne r1, #4\n bne 2f\n"
        "ldr   r2, =0x55555555\n cmp r5, r2\n movne r1, #5\n bne 2f\n"
        "ldr   r2, =0x66666666\n cmp r6, r2\n movne r1, #6\n bne 2f\n"
        "ldr   r2, =0x77777777\n cmp r7, r2\n movne r1, #7\n bne 2f\n"
        "ldr   r2, =0x88888888\n cmp r8, r2\n movne r1, #8\n bne 2f\n"
        "ldr   r2, =0x99999999\n cmp r9, r2\n movne r1, #9\n bne 2f\n"
        "ldr   r2, =0xaaaaaaaa\n cmp r10, r2\n movne r1, #10\n bne 2f\n"
        "ldr   r2, =0xbbbbbbbb\n cmp r11, r2\n movne r1, #11\n bne 2f\n"
        "subs  r0, r0, #1\n"
        "bne   1b\n"
        "2:\n"
        "pop   {r4-r11}\n"
        "b     3f\n"
        ".ltorg\n"
        "3:\n"
        : "+r"(n), "+r"(bad)
        :
        : "r2", "r3", "cc", "memory");
    return (int)bad;
}

static int signal_regs_test(unsigned round)
{
    struct itimerval it = {{0, 1000}, {0, 1000}};
    struct sigaction sa;
    int bad, i, p[2], restarted = 0;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_alarm;
    sa.sa_flags = SA_RESTART;
    sigaction(SIGALRM, &sa, NULL);
    alarms = 0;
    setitimer(ITIMER_REAL, &it, NULL);
    bad = regs_hold(30000000u);
    /* blocking read interrupted (and restarted) by the alarms, data from a child */
    if (pipe(p) == 0) {
        pid_t pid = fork();
        if (pid == 0) {
            usleep(300000);
            write(p[1], "ok", 2);
            _exit(0);
        }
        {
            char buf[2] = {0, 0};
            unsigned before = (unsigned)alarms;
            restarted = read(p[0], buf, 2) == 2 && buf[0] == 'o' && buf[1] == 'k' &&
                        (unsigned)alarms > before;
        }
        waitpid(pid, NULL, 0);
        close(p[0]);
        close(p[1]);
    }
    memset(&it, 0, sizeof(it));
    setitimer(ITIMER_REAL, &it, NULL);
    signal(SIGALRM, SIG_DFL);
    for (i = 0; i < 1; i++)
        ;
    if (bad)
        say("round %u step8 REGISTER r%d CHANGED across signals (%d alarms)\n", round, bad, (int)alarms);
    else
        say("round %u step8 OK registers intact over %d alarms; restarted read %s\n", round, (int)alarms,
            restarted ? "OK" : "FAILED");
    return bad || !restarted;
}

static const char *const sh_tests[][2] = {
    {"echo", "echo hi >/dev/null"},
    {"func", "k() { echo \"$*\" >/dev/null; }; k a; k b; k c"},
    {"subst", "x=$(echo hi); [ \"$x\" = hi ]"},
    {"func+subst", "k() { echo \"$*\" >/dev/null; }; k \"$(echo hi)\"; k \"$(uname -a)\""},
    {"grep", "k() { :; }; k \"$(grep -m1 -i processor /proc/cpuinfo)\""},
    {"sleep", "sleep 1"},
    {"init-like", "k() { echo \"x: $*\" >/dev/null; }; k one; k \"$(uname -a)\"; "
                  "k \"$(grep MemTotal /proc/meminfo) $(grep MemFree /proc/meminfo)\"; sleep 1 && k ok; k end"},
};

static void shell_tests(unsigned round)
{
    size_t t;
    for (t = 0; t < sizeof(sh_tests) / sizeof(sh_tests[0]); t++) {
        int status = 0;
        pid_t pid = fork();
        if (pid == 0) {
            char *argv[] = {"sh", "-c", (char *)sh_tests[t][1], NULL};
            int fd = open("/dev/null", O_RDWR);
            dup2(fd, 0);
            dup2(kmsg, 1);
            dup2(kmsg, 2);
            execv("/bin/busybox", argv);
            _exit(127);
        }
        waitpid(pid, &status, 0);
        if (WIFSIGNALED(status))
            say("round %u step9 sh '%s' KILLED by signal %d\n", round, sh_tests[t][0], WTERMSIG(status));
        else
            say("round %u step9 sh '%s' exit %d\n", round, sh_tests[t][0], WEXITSTATUS(status));
    }
}

static int unaligned_test(unsigned round)
{
    /* two pages, page aligned, so offsets near 4096 cross a page boundary */
    static uint8_t raw[3 * 4096];
    uint8_t *buf = (uint8_t *)(((uintptr_t)raw + 4095) & ~(uintptr_t)4095);
    static const unsigned bases[] = {8, 28, 29, 30, 31, 32, 60, 61, 62, 63, 4092, 4093, 4094, 4095};
    int bad = 0;
    size_t b, k, i;
    for (b = 0; b < sizeof(bases) / sizeof(bases[0]); b++) {
        unsigned off = bases[b];
        for (k = 0; k < 4; k++) {
            uint8_t *p = buf + off + k;
            uint32_t want, got;
            uint16_t want16, got16;
            for (i = 0; i < 2 * 4096; i++)
                buf[i] = (uint8_t)(i * 13 + 7);
            /* 32-bit load */
            want = (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
            got = *(volatile uint32_t *)p;
            if (got != want) {
                if (bad < 6)
                    say("round %u step10 LDR at +%u (page off %u) = %08x want %08x\n", round,
                        off + (unsigned)k, (unsigned)((uintptr_t)p & 4095), got, want);
                bad++;
            }
            want16 = (uint16_t)(p[0] | p[1] << 8);
            got16 = *(volatile uint16_t *)p;
            if (got16 != want16) {
                if (bad < 6)
                    say("round %u step10 LDRH at +%u = %04x want %04x\n", round, off + (unsigned)k, got16, want16);
                bad++;
            }
            /* 32-bit store, then the neighbours must be untouched */
            *(volatile uint32_t *)p = 0xA1B2C3D4u;
            if (p[0] != 0xD4 || p[1] != 0xC3 || p[2] != 0xB2 || p[3] != 0xA1 ||
                p[-1] != (uint8_t)((p - 1 - buf) * 13 + 7) || p[4] != (uint8_t)((p + 4 - buf) * 13 + 7)) {
                if (bad < 6)
                    say("round %u step10 STR at +%u wrote %02x %02x %02x %02x [%02x|%02x]\n", round,
                        off + (unsigned)k, p[0], p[1], p[2], p[3], p[-1], p[4]);
                bad++;
            }
        }
    }
    /* memcpy / memset with odd alignments and sizes against byte loops */
    {
        static uint8_t src[600], dst[600], ref[600];
        size_t so, dof, n;
        for (i = 0; i < sizeof(src); i++)
            src[i] = (uint8_t)(i * 7 + 3);
        for (so = 0; so < 8; so++)
            for (dof = 0; dof < 8; dof++)
                for (n = 0; n < 70; n += 3) {
                    memset(dst, 0x5A, sizeof(dst));
                    memset(ref, 0x5A, sizeof(ref));
                    for (i = 0; i < n; i++)
                        ref[dof + 8 + i] = src[so + i];
                    memcpy(dst + dof + 8, src + so, n);
                    if (memcmp(dst, ref, sizeof(dst))) {
                        if (bad < 10)
                            say("round %u step10 memcpy src+%zu dst+%zu n=%zu WRONG\n", round, so, dof, n);
                        bad++;
                    }
                    memset(dst, 0x5A, sizeof(dst));
                    memset(dst + dof + 8, 0, n);
                    for (i = 0; i < sizeof(dst); i++)
                        if (dst[i] != ((i >= dof + 8 && i < dof + 8 + n) ? 0 : 0x5A))
                            break;
                    if (i != sizeof(dst)) {
                        if (bad < 10)
                            say("round %u step10 memset dst+%zu n=%zu WRONG at %zu\n", round, dof, n, i);
                        bad++;
                    }
                }
    }
    if (!bad)
        say("round %u step10 OK (unaligned loads/stores, memcpy, memset)\n", round);
    else
        say("round %u step10 %d FAILURES in unaligned access / memcpy / memset\n", round, bad);
    return bad;
}

static int bigcopy_test(unsigned round)
{
    static uint8_t raw[4 * 4096];
    uint8_t *page = (uint8_t *)(((uintptr_t)raw + 4095) & ~(uintptr_t)4095);
    uint8_t *src = page + 4096 - 600;          /* copies straddle a page boundary */
    uint8_t *dst = page + 2 * 4096 - 500;
    static uint8_t ref[2400];
    int bad = 0, pass;
    size_t so, dof, n, i;
    for (pass = 0; pass < 3; pass++)
        for (so = 0; so < 8; so++)
            for (dof = 0; dof < 8; dof++)
                for (n = 0; n <= 1100; n += (n < 80 ? 1 : 17)) {
                    for (i = 0; i < 1200; i++)
                        src[i] = (uint8_t)(i * 29 + pass * 7 + 1);
                    memset(dst - 16, 0xC5, 1200);
                    memcpy(dst + dof, src + so, n);
                    for (i = 0; i < n; i++)
                        if (dst[dof + i] != src[so + i])
                            break;
                    if (i != n || dst[dof - 1] != 0xC5 || dst[dof + n] != 0xC5) {
                        if (bad < 8) {
                            size_t w = i & ~(size_t)3;
                            say("round %u step11 memcpy n=%zu src+%zu dst+%zu: first bad byte %zu, "
                                "got %02x%02x%02x%02x want %02x%02x%02x%02x\n", round, n, so, dof, i,
                                dst[dof + w + 3], dst[dof + w + 2], dst[dof + w + 1], dst[dof + w],
                                src[so + w + 3], src[so + w + 2], src[so + w + 1], src[so + w]);
                        }
                        bad++;
                    }
                    /* overlapping memmove, both directions */
                    if (n && n < 900 && (n & 7) == so) {
                        for (i = 0; i < n + 16; i++)
                            ref[i] = src[i];
                        memmove(src + dof + 1, src, n);
                        for (i = 0; i < n; i++)
                            if (src[dof + 1 + i] != ref[i])
                                break;
                        if (i != n) {
                            if (bad < 8)
                                say("round %u step11 memmove n=%zu shift %zu: bad at %zu\n", round, n, dof + 1, i);
                            bad++;
                        }
                    }
                }
    if (!bad)
        say("round %u step11 OK (large memcpy/memmove, all alignments)\n", round);
    else
        say("round %u step11 %d FAILURES in large memcpy/memmove\n", round, bad);
    return bad;
}

static volatile sig_atomic_t chld12;

static void on_chld12(int sig)
{
    (void)sig;
    chld12++;
}

/* read(fd, buf, n) through svc with r4-r6, r8-r11 holding patterns (r7 =
 * syscall number). Returns the first callee-saved register changed (4..11)
 * or 0; *ret gets the syscall result. */
static int read_regs_hold(int fd, void *buf, unsigned n, int *ret)
{
    register int r0 __asm__("r0") = fd;
    register void *r1 __asm__("r1") = buf;
    register unsigned r2 __asm__("r2") = n;
    register unsigned r3 __asm__("r3") = 0;
    __asm__ __volatile__(
        "push  {r4-r11}\n"
        "ldr   r4, =0x44444444\n ldr r5, =0x55555555\n ldr r6, =0x66666666\n"
        "ldr   r8, =0x88888888\n ldr r9, =0x99999999\n ldr r10, =0xaaaaaaaa\n ldr r11, =0xbbbbbbbb\n"
        "mov   r7, #3\n"                       /* __NR_read (EABI) */
        "svc   #0\n"
        "ldr   r12, =0x44444444\n cmp r4, r12\n movne r3, #4\n bne 2f\n"
        "ldr   r12, =0x55555555\n cmp r5, r12\n movne r3, #5\n bne 2f\n"
        "ldr   r12, =0x66666666\n cmp r6, r12\n movne r3, #6\n bne 2f\n"
        "ldr   r12, =0x88888888\n cmp r8, r12\n movne r3, #8\n bne 2f\n"
        "ldr   r12, =0x99999999\n cmp r9, r12\n movne r3, #9\n bne 2f\n"
        "ldr   r12, =0xaaaaaaaa\n cmp r10, r12\n movne r3, #10\n bne 2f\n"
        "ldr   r12, =0xbbbbbbbb\n cmp r11, r12\n movne r3, #11\n bne 2f\n"
        "cmp   r7, #3\n movne r3, #7\n"
        "2:\n"
        "pop   {r4-r11}\n"
        "b     3f\n"
        ".ltorg\n"
        "3:\n"
        : "+r"(r0), "+r"(r1), "+r"(r2), "+r"(r3)
        :
        : "r12", "cc", "memory");
    *ret = r0;
    return (int)r3;
}

static int syscall_signal_test(unsigned round)
{
    struct sigaction sa;
    struct itimerval it = {{0, 1000}, {0, 1000}};
    int i, bad = 0, short_reads = 0;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_chld12;
    sa.sa_flags = SA_RESTART;
    sigaction(SIGCHLD, &sa, NULL);
    sa.sa_handler = on_alarm;
    sigaction(SIGALRM, &sa, NULL);
    chld12 = 0;
    alarms = 0;
    setitimer(ITIMER_REAL, &it, NULL);
    for (i = 0; i < 200; i++) {
        int p[2], ret = 0, reg;
        char buf[128];
        pid_t pid;
        if (pipe(p))
            break;
        pid = fork();
        if (pid == 0) {
            /* like $(uname -a): ~90 bytes, then exit (SIGCHLD to the parent) */
            static const char line[] = "Linux (none) 6.12.117-bigfoston #1 Thu Nov 19 11:57:25 -03 2026 armv6l GNU/Linux\n";
            usleep(3000 + (i % 7) * 1000);
            write(p[1], line, sizeof(line) - 1);
            _exit(0);
        }
        close(p[1]);
        reg = read_regs_hold(p[0], buf, sizeof(buf), &ret);
        if (reg) {
            if (bad < 6)
                say("round %u step12 REGISTER r%d CHANGED across interrupted read (iter %d, ret %d)\n",
                    round, reg, i, ret);
            bad++;
        }
        if (ret != 81)
            short_reads++;
        close(p[0]);
        waitpid(pid, NULL, 0);
    }
    memset(&it, 0, sizeof(it));
    setitimer(ITIMER_REAL, &it, NULL);
    signal(SIGALRM, SIG_DFL);
    signal(SIGCHLD, SIG_DFL);
    if (!bad)
        say("round %u step12 OK registers intact across 200 interrupted reads (%d alarms, %d SIGCHLD, %d short)\n",
            round, (int)alarms, (int)chld12, short_reads);
    else
        say("round %u step12 %d FAILURES: registers changed across interrupted syscalls\n", round, bad);
    return bad;
}

static void *(*volatile memset_ptr)(void *, int, size_t) = memset;   /* never inlined */

static int byte_store_test(unsigned round)
{
    enum { SLOTS = 16384 };                 /* 128 KB of 8-byte slots */
    static uint32_t slots[SLOTS * 2];
    struct itimerval it = {{0, 1000}, {0, 1000}};
    struct sigaction sa;
    unsigned long iter, total = 0;
    int bad = 0, pass;
    pid_t sibling;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_alarm;
    sa.sa_flags = SA_RESTART;
    sigaction(SIGALRM, &sa, NULL);
    alarms = 0;
    sibling = fork();                       /* competes for the CPU: context switches */
    if (sibling == 0) {
        volatile unsigned x = 0;
        for (;;)
            x++;
    }
    setitimer(ITIMER_REAL, &it, NULL);
    for (pass = 0; pass < 40; pass++) {
        size_t i;
        uint32_t stale = 0xb6f00000u + (uint32_t)pass * 0x10101u;
        for (i = 0; i < SLOTS * 2; i++)
            slots[i] = stale + (uint32_t)i;
        for (iter = 0; iter < SLOTS; iter++) {
            uint32_t *p = &slots[iter * 2];
            memset_ptr(p, 0, 8);
            if (p[0] || p[1]) {
                if (bad < 6)
                    say("round %u step13 memset(8) LOST: slot %lu words %08x %08x (were %08x %08x)\n", round,
                        iter, p[0], p[1], stale + (uint32_t)(iter * 2), stale + (uint32_t)(iter * 2 + 1));
                bad++;
            }
        }
        total += SLOTS;
    }
    memset(&it, 0, sizeof(it));
    setitimer(ITIMER_REAL, &it, NULL);
    signal(SIGALRM, SIG_DFL);
    kill(sibling, SIGKILL);
    waitpid(sibling, NULL, 0);
    if (!bad)
        say("round %u step13 OK %lu x memset(8) with %d alarms and a busy sibling\n", round, total, (int)alarms);
    else
        say("round %u step13 %d LOST STORES in %lu x memset(8)\n", round, bad, total);
    return bad;
}

/* ---- byte-store experiments (kernel command line: bftest=bytes) ---- */

#define BX_SLOTS 16384
static uint32_t bx[BX_SLOTS * 2] __attribute__((aligned(32)));

static inline void dsb(void)
{
    __asm__ __volatile__("mcr p15, 0, %0, c7, c10, 4" :: "r"(0) : "memory");
}

static void v_memset(uint32_t *p) { memset_ptr(p, 0, 8); }
static void v_words(uint32_t *p) { ((volatile uint32_t *)p)[0] = 0; ((volatile uint32_t *)p)[1] = 0; }
static void v_bytes_musl(uint32_t *p)
{
    volatile uint8_t *b = (volatile uint8_t *)p;
    b[0] = 0; b[7] = 0; b[1] = 0; b[2] = 0; b[6] = 0; b[5] = 0; b[3] = 0; b[4] = 0;
}
static void v_one_byte(uint32_t *p)      /* a single byte store to byte 7 */
{
    ((volatile uint8_t *)p)[7] = 0;
}
static void v_halfwords(uint32_t *p)
{
    volatile uint16_t *h = (volatile uint16_t *)p;
    h[0] = 0; h[3] = 0; h[1] = 0; h[2] = 0;
}

/* Unaligned stores: the suspected MT3351 defect (halfword store to an odd address). */
static void v_strh_odd1(uint32_t *p) { uint8_t *b = (uint8_t *)p + 1; __asm__ __volatile__("strh %1, [%0]" :: "r"(b), "r"(0) : "memory"); }
static void v_strh_odd5(uint32_t *p) { uint8_t *b = (uint8_t *)p + 5; __asm__ __volatile__("strh %1, [%0]" :: "r"(b), "r"(0) : "memory"); }
static void v_strh_odd3(uint32_t *p) { uint8_t *b = (uint8_t *)p + 3; __asm__ __volatile__("strh %1, [%0]" :: "r"(b), "r"(0) : "memory"); }
static void v_strh_even2(uint32_t *p) { uint8_t *b = (uint8_t *)p + 2; __asm__ __volatile__("strh %1, [%0]" :: "r"(b), "r"(0) : "memory"); }
static void v_str_off1(uint32_t *p) { uint8_t *b = (uint8_t *)p + 1; __asm__ __volatile__("str %1, [%0]" :: "r"(b), "r"(0) : "memory"); }
static void v_str_off2(uint32_t *p) { uint8_t *b = (uint8_t *)p + 2; __asm__ __volatile__("str %1, [%0]" :: "r"(b), "r"(0) : "memory"); }

struct bx_variant {
    const char *name;
    void (*fn)(uint32_t *);
    unsigned mask;                          /* bit i set: byte i must become 0; clear: byte i unchanged */
};

/* Store with a write fault (copy-on-write after fork) on the store itself. */
static uint32_t strb_postindex(volatile uint8_t *p, uint32_t step)
{
    register volatile uint8_t *r3 __asm__("r3") = p;
    register uint32_t r2 __asm__("r2") = step;
    register uint32_t r1 __asm__("r1") = 0;
    __asm__ __volatile__("strb r1, [r3], r2" : "+r"(r3) : "r"(r1), "r"(r2) : "memory");
    return (uint32_t)(uintptr_t)r3;
}

static void cow_fault_experiments(void)
{
    enum { ROUNDS = 400 };
    const char *names[5] = {"COW memset(8) musl", "COW strb post-index", "COW strb plain",
                            "COW strb b0 + strh odd p+1", "COW strb b0 + strb b1,b2"};
    int kind;
    for (kind = 0; kind < 5; kind++) {
        int r, lost = 0, base_wrong = 0, shown = 0;
        for (r = 0; r < ROUNDS; r++) {
            uint8_t *buf = mmap(NULL, 8192, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
            uint8_t *q;
            pid_t pid;
            size_t i;
            int ok = 1;
            if (buf == MAP_FAILED)
                break;
            for (i = 0; i < 8192; i++)
                buf[i] = 0xA5;
            pid = fork();                          /* pages become copy-on-write */
            if (pid == 0) {
                pause();
                _exit(0);
            }
            q = buf + 4096 + 64;                  /* first write to this page faults */
            if (kind == 0) {
                memset_ptr(q, 0, 8);
                for (i = 0; i < 8; i++)
                    ok &= q[i] == 0;
            } else if (kind == 1) {
                uint32_t end = strb_postindex(q, 8);
                if (end != (uint32_t)(uintptr_t)(q + 8))
                    base_wrong++, ok = 0;
                ok &= q[0] == 0;
            } else if (kind == 2) {
                ((volatile uint8_t *)q)[0] = 0;
                ok &= q[0] == 0;
            } else {                               /* the musl pair: byte store then odd halfword */
                uint8_t *q1 = q + 1;
                if (kind == 3)
                    __asm__ __volatile__("strb %2, [%0]\n\tstrh %2, [%1]" :: "r"(q), "r"(q1), "r"(0) : "memory");
                else
                    __asm__ __volatile__("strb %2, [%0]\n\tstrb %2, [%1]\n\tstrb %2, [%1, #1]" :: "r"(q), "r"(q1), "r"(0) : "memory");
                ok &= q[0] == 0 && q[1] == 0 && q[2] == 0 && q[3] == 0xA5;
            }
            for (i = 1; i <= 8; i++)              /* neighbours untouched */
                ok &= q[-(long)i] == 0xA5;
            ok &= (kind != 0 || q[8] == 0xA5) && (kind == 0 || kind >= 3 || q[1] == 0xA5);
            if (!ok) {
                if (shown++ < 3)
                    say("bytes %s WRONG: %02x %02x %02x %02x %02x %02x %02x %02x | before %02x %02x %02x %02x\n",
                        names[kind], q[0], q[1], q[2], q[3], q[4], q[5], q[6], q[7], q[-4], q[-3], q[-2], q[-1]);
                lost++;
            }
            kill(pid, SIGKILL);
            waitpid(pid, NULL, 0);
            munmap(buf, 8192);
        }
        say("bytes '%s': %d wrong of %d (base register wrong %d)\n", names[kind], lost, ROUNDS, base_wrong);
    }
}

static void bytes_experiments(void)
{
    static const struct bx_variant v[] = {
        {"memset(8) musl", v_memset, 0xFF},
        {"strh odd p+1", v_strh_odd1, 0x06},
        {"strh odd p+5", v_strh_odd5, 0x60},
        {"strh odd p+3", v_strh_odd3, 0x18},
        {"strh even p+2", v_strh_even2, 0x0C},
        {"str p+1", v_str_off1, 0x1E},
        {"str p+2", v_str_off2, 0x3C},
        {"words str+str", v_words, 0xFF},
        {"bytes musl order", v_bytes_musl, 0xFF},
        {"halfwords", v_halfwords, 0xFF},
        {"one byte 7", v_one_byte, 0x80},
    };
    unsigned run;
    size_t k;
    for (run = 1;; run++) {
        int trap = !(run & 1);              /* odd runs: trap off; even runs: trap on */
        int fd = open("/sys/module/timer_mt3351_gpt/parameters/align_trap", O_WRONLY);
        if (fd >= 0) {
            if (write(fd, trap ? "1" : "0", 1) != 1)
                say("bytes: align_trap write failed\n");
            close(fd);
        } else {
            say("bytes: no align_trap switch\n");
        }
        say("bytes ===== run %u, alignment trap %s =====\n", run, trap ? "ON" : "OFF");
        cow_fault_experiments();
        for (k = 0; k < sizeof(v) / sizeof(v[0]); k++) {
            unsigned long it, lost = 0;
            int shown = 0, pass;
            for (pass = 0; pass < 10; pass++) {
                size_t i;
                uint32_t stale = 0xb6f00000u + (uint32_t)pass * 0x10101u + (uint32_t)k;
                for (i = 0; i < BX_SLOTS * 2; i++)
                    bx[i] = stale + (uint32_t)i * 0x01010101u;
                for (it = 0; it < BX_SLOTS; it++) {
                    uint32_t *p = &bx[it * 2], w0, w1, b0 = p[0], b1 = p[1];
                    uint64_t before, after, keep = 0;
                    int ok, j;
                    v[k].fn(p);
                    w0 = p[0];
                    w1 = p[1];
                    for (j = 0; j < 8; j++)
                        if (!(v[k].mask & (1u << j)))
                            keep |= 0xFFull << (8 * j);
                    before = (uint64_t)b1 << 32 | b0;
                    after = (uint64_t)w1 << 32 | w0;
                    ok = after == (before & keep);
                    if (!ok) {
                        if (shown < 2 && run <= 2)
                            say("bytes '%s' LOST at %p (word offs %lu): %08x %08x  before %08x %08x\n",
                                v[k].name, (void *)p, (unsigned long)((uintptr_t)p & 31), w0, w1, b0, b1);
                        shown++;
                        lost++;
                    }
                }
            }
            say("bytes run %u trap %s '%s': %lu lost of %u\n", run, trap ? "ON" : "OFF", v[k].name, lost, 10u * BX_SLOTS);
        }
        sleep(5);
    }
}

static uint32_t *ref_crc;
static size_t ref_pages;

static void load_reference(void)
{
    FILE *f = fopen("/etc/busybox.pagecrc", "r");
    unsigned v;
    size_t cap = 0;
    if (!f) {
        say("step6: no /etc/busybox.pagecrc\n");
        return;
    }
    while (fscanf(f, "%x", &v) == 1) {
        if (ref_pages == cap) {
            cap = cap ? cap * 2 : 512;
            ref_crc = realloc(ref_crc, cap * sizeof(*ref_crc));
        }
        ref_crc[ref_pages++] = v;
    }
    fclose(f);
}

static unsigned long pfn_of(const void *va)
{
    uint64_t entry = 0;
    int fd = open("/proc/self/pagemap", O_RDONLY);
    if (fd < 0)
        return 0;
    pread(fd, &entry, 8, (off_t)((uintptr_t)va / 4096) * 8);
    close(fd);
    return (entry & (1ULL << 63)) ? (unsigned long)(entry & ((1ULL << 55) - 1)) : 0;
}

/* returns the number of pages that differ from the build-time reference */
static int check_busybox(unsigned round)
{
    struct stat st;
    int fd = open("/bin/busybox", O_RDONLY), bad = 0;
    size_t i, pages;
    uint8_t *m;
    if (fd < 0 || fstat(fd, &st) || !ref_pages)
        return -1;
    pages = ((size_t)st.st_size + 4095) / 4096;
    m = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_SHARED, fd, 0);
    close(fd);
    if (m == MAP_FAILED)
        return -1;
    for (i = 0; i < pages && i < ref_pages; i++) {
        size_t len = i + 1 < pages ? 4096 : (size_t)st.st_size - i * 4096;
        uint32_t c = crc32_buf(m + i * 4096, len);
        if (c != ref_crc[i]) {
            if (bad < 12)
                say("round %u step6 busybox page %zu (file 0x%zx) CHANGED crc %08x want %08x pa=0x%08lx\n",
                    round, i, i * 4096, c, ref_crc[i], pfn_of(m + i * 4096) << 12);
            bad++;
        }
    }
    munmap(m, (size_t)st.st_size);
    return bad;
}

int main(void)
{
    uint32_t sum0;
    unsigned round = 0;
    mount("devtmpfs", "/dev", "devtmpfs", 0, NULL);
    mount("proc", "/proc", "proc", 0, NULL);
    mkdir("/sys", 0755);
    mount("sysfs", "/sys", "sysfs", 0, NULL);
    kmsg = open("/dev/kmsg", O_WRONLY);
    {
        /* log every user-mode alignment trap the kernel emulates (mode 3 = warn + fixup) */
        int fd = open("/proc/cpu/alignment", O_WRONLY);
        if (fd >= 0) {
            write(fd, "3", 1);
            close(fd);
        }
    }
    {
        char cmd[512] = "";
        int fd = open("/proc/cmdline", O_RDONLY);
        if (fd >= 0) {
            ssize_t n = read(fd, cmd, sizeof(cmd) - 1);
            cmd[n > 0 ? n : 0] = 0;
            close(fd);
        }
        if (strstr(cmd, "bftest=bytes") || (getenv("BFTEST") && !strcmp(getenv("BFTEST"), "bytes")))
            bytes_experiments();
    }
    sum0 = code_sum();
    load_reference();
    {
        int bad = check_busybox(0);
        say("step6 at start: busybox %zu pages, %d differ from the build\n", ref_pages, bad);
    }
    say("start: code %p..%p sum %08x\n", (void *)__executable_start, (void *)etext, sum0);
    for (;;) {
        uint32_t s;
        round++;
        s = code_sum();
        say("round %u step1 code sum %08x %s\n", round, s, s == sum0 ? "OK" : "CHANGED");
        say("round %u step2 forks...\n", round);
        if (forks(300, "step2") == 0)
            say("round %u step2 OK (300 fork/exit/wait)\n", round);
        signal(SIGCHLD, on_chld);
        chld_count = 0;
        say("round %u step3 forks with SIGCHLD handler...\n", round);
        if (forks(300, "step3") == 0)
            say("round %u step3 OK, %d SIGCHLD\n", round, (int)chld_count);
        signal(SIGCHLD, SIG_DFL);
        say("round %u step4 pingpong...\n", round);
        if (pingpong(20000) == 0)
            say("round %u step4 OK (20000 round trips)\n", round);
        say("round %u step5 exec...\n", round);
        if (execs(50) == 0)
            say("round %u step5 OK (50 fork+exec)\n", round);
        say("round %u step13 byte stores...\n", round);
        byte_store_test(round);
        say("round %u step12 interrupted syscalls...\n", round);
        syscall_signal_test(round);
        say("round %u step11 large memcpy...\n", round);
        bigcopy_test(round);
        say("round %u step10 unaligned access...\n", round);
        unaligned_test(round);
        say("round %u step8 signals vs registers...\n", round);
        signal_regs_test(round);
        say("round %u step9 shell scripts...\n", round);
        shell_tests(round);
        say("round %u step7 copy-on-write...\n", round);
        if (cow_test(round) == 0)
            say("round %u step7 OK (1 MB isolated both ways)\n", round);
        else
            say("round %u step7 FAILED: fork copy-on-write does not isolate\n", round);
        {
            int bad = check_busybox(round);
            say("round %u step6 busybox: %d of %zu pages differ\n", round, bad, ref_pages);
        }
        {
            char abuf[400];
            int fd = open("/proc/cpu/alignment", O_RDONLY), n = -1, j;
            if (fd >= 0) {
                n = (int)read(fd, abuf, sizeof(abuf) - 1);
                close(fd);
            }
            if (n > 0) {
                abuf[n] = 0;
                for (j = 0; j < n; j++)
                    if (abuf[j] == '\n')
                        abuf[j] = ' ';
                say("round %u alignment: %s\n", round, abuf);
            }
        }
        sleep(2);
    }
}
