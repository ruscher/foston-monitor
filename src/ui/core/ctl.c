/* ctl.c — ver ctl.h. */
#include "ctl.h"
#include "nav.h"
#include "perfui.h"
#include "../screens/screens.h"
#include "lvgl.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define CTL_DIR  "/run/bigfoston"
#define CTL_PATH CTL_DIR "/ctl"

static int fd = -1, keep = -1;
static char buf[1200];
static size_t len;

static lv_obj_t *scrollable(lv_obj_t *o)
{
    if(lv_obj_get_scroll_bottom(o) > 0 || lv_obj_get_scroll_top(o) > 0) return o;
    for(uint32_t i = 0; i < lv_obj_get_child_count(o); i++) {
        lv_obj_t *r = scrollable(lv_obj_get_child(o, (int32_t)i));
        if(r) return r;
    }
    return NULL;
}

static void command(char *l)
{
    if(!strcmp(l, "app web")) nav_go(SCR_WEB, 0);
    else if(!strcmp(l, "app apps")) nav_go(SCR_HOME, HOME_ARG_APPS);   /* grade com todos os apps */
    else if(!strncmp(l, "app webapp ", 11)) nav_go(SCR_WEB, WEB_ARG_APP0 + atoi(l + 11));
    else if(!strncmp(l, "page ", 5)) scr_home_show_page(atoi(l + 5));
    else if(!strcmp(l, "app perfmode")) nav_go(SCR_PERFMODE, 0);
    else if(!strcmp(l, "perf on")) perfui_manual(true);
    else if(!strcmp(l, "perf off")) perfui_disable();
    else if(!strcmp(l, "perf dialog")) perfui_dialog();
    else if(!strncmp(l, "scroll ", 7)) {           /* rola a primeira área rolável da tela */
        lv_obj_t *o = scrollable(lv_screen_active());
        if(o) lv_obj_scroll_by(o, 0, -atoi(l + 7), LV_ANIM_OFF);
    }
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
