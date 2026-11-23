/* ctl.c — ver ctl.h. */
#include "ctl.h"
#include "nav.h"
#include "../screens/screens.h"
#include "lvgl.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define CTL_DIR  "/run/bigfoston"
#define CTL_PATH CTL_DIR "/ctl"

static int fd = -1, keep = -1;
static char buf[1200];
static size_t len;

static void command(char *l)
{
    if(!strcmp(l, "app web")) nav_go(SCR_WEB, 0);
    else if(!strcmp(l, "app home") || !strcmp(l, "home")) nav_home();
    else if(!strncmp(l, "web ", 4)) {
        if(nav_current().scr == SCR_WEB) scr_web_demo(l + 4);
    }
}

static void tick(lv_timer_t *t)
{
    (void)t;
    ssize_t n = read(fd, buf + len, sizeof buf - 1 - len);
    if(n <= 0) return;
    len += (size_t)n;
    char *s = buf, *nl;
    while((nl = memchr(s, '\n', buf + len - s))) { *nl = 0; command(s); s = nl + 1; }
    len -= (size_t)(s - buf);
    memmove(buf, s, len);
    if(len == sizeof buf - 1) len = 0;
}

void ctl_init(void)
{
    if(geteuid() != 0) return;                   /* só no aparelho (shell como root) */
    mkdir(CTL_DIR, 0700);
    unlink(CTL_PATH);
    if(mkfifo(CTL_PATH, 0600)) return;
    fd = open(CTL_PATH, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    keep = open(CTL_PATH, O_WRONLY | O_NONBLOCK | O_CLOEXEC);   /* sem EOF quando o escritor sai */
    if(fd < 0) return;
    lv_timer_create(tick, 300, NULL);
}
