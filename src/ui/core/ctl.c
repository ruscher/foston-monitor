/* ctl.c — ver ctl.h. */
#include "ctl.h"
#include "nav.h"
#include "hwkbd.h"
#include "perfui.h"
#include "apps.h"
#include "perf.h"
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
    else if(!strcmp(l, "app keyboard")) nav_go(SCR_KEYBOARD, 0);
    else if(!strncmp(l, "group ", 6)) nav_go(SCR_APPGROUP, atoi(l + 6));
    else if(!strcmp(l, "app sc-controller")) nav_go(SCR_SCC, 0);
    else if(!strcmp(l, "perfstat reset")) perf_reset();
    else if(!strcmp(l, "perfstat trace")) perf_trace_invalidate(1000);
    else if(!strncmp(l, "perfstat ", 9)) {          /* medidas desde o reset: /run/bigfoston/perf.txt */
        char b[600];
        perf_format(b, sizeof b, l + 9);
        FILE *f = fopen(CTL_DIR "/perf.txt", "a");
        if(f) { fprintf(f, "%s\n", b); fclose(f); }
        perf_reset();
    }
    else if(!strcmp(l, "app map")) nav_go(SCR_MAP, 0);
    else if(!strcmp(l, "app gps")) nav_go(SCR_GPS, 0);
    else if(!strcmp(l, "app phone")) nav_go(SCR_PHONE, 0);
    else if(!strcmp(l, "app bluetooth")) nav_go(SCR_BLUETOOTH, 0);
    else if(!strncmp(l, "install-ask ", 12)) apps_install_ask(l + 12);   /* o diálogo "Instalar?" */
    else if(!strncmp(l, "install ", 8)) apps_install_now(l + 8);       /* sem perguntar (bfapp.py) */
    else if(!strncmp(l, "uninstall ", 10)) apps_uninstall_now(l + 10);
    else if(!strncmp(l, "key ", 4)) {             /* tecla crua do teclado físico: "key 30 1" */
        int code = 0, val = 0;
        if(sscanf(l + 4, "%d %d", &code, &val) == 2 && code > 0 && code < 0x300) hwkbd_feed((uint16_t)code, val);
    }
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
