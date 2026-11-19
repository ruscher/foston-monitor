/* app.c — monta a tela fixa: barra superior (48 px), conteúdo (360 px) e
 * barra inferior (72 px). Um único lv_timer de 1 s atualiza relógio/status. */
#define _DEFAULT_SOURCE
#include "app.h"
#include "nav.h"
#include "theme.h"
#include "settings.h"
#include "status.h"
#include "../widgets/topbar.h"
#include "../widgets/bottombar.h"
#include "../widgets/dialog.h"
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>

static lv_display_t *disp;
static lv_obj_t *content;
static lv_timer_t *tick;

lv_display_t *app_display(void) { return disp; }

static void tick_cb(lv_timer_t *t) { (void)t; topbar_update(); }

static void build(void)
{
    lv_obj_t *scr = lv_screen_active();
    lv_obj_clean(scr);
    lv_obj_remove_style_all(scr);
    lv_obj_add_style(scr, &bf_st_screen, 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *top = topbar_create(scr);
    lv_obj_set_pos(top, 0, 0);

    content = bf_plain(scr);
    lv_obj_set_pos(content, 0, BF_TOPBAR_H);
    lv_obj_set_size(content, BF_SCREEN_W, BF_CONTENT_H);

    lv_obj_t *bot = bottombar_create(scr);
    lv_obj_set_pos(bot, 0, BF_SCREEN_H - BF_BOTBAR_H);
}

void app_apply_hw_settings(void)
{
    const bf_status_provider_t *p = status_provider();
    if(p->backlight_get() >= 0) p->backlight_set(settings()->backlight);
    if(p->volume_get() >= 0) p->volume_set(settings()->volume);
}

void app_init(lv_display_t *d)
{
    disp = d;
    i18n_set_lang(settings()->lang);
    char cat[300];
    snprintf(cat, sizeof cat, "%s/lang/%s.txt", data_dir(), i18n_lang_code(settings()->lang));
    i18n_load_catalog(cat);
    theme_init(disp, settings()->dark);
    build();
    nav_init(content);
    nav_show_now();
    tick = lv_timer_create(tick_cb, 1000, NULL);
}

void app_rebuild_now(void)
{
    nav_teardown();
    lv_obj_clean(lv_layer_top());
    i18n_set_lang(settings()->lang);
    char cat[300];
    snprintf(cat, sizeof cat, "%s/lang/%s.txt", data_dir(), i18n_lang_code(settings()->lang));
    i18n_load_catalog(cat);
    theme_init(disp, settings()->dark);
    build();
    nav_set_content(content);
    nav_show_now();
}

static void rebuild_async(void *ud) { (void)ud; app_rebuild_now(); }
void app_rebuild(void) { lv_async_call(rebuild_async, NULL); }

static void power_err_cb(int b, void *ud) { (void)b; (void)ud; }

void app_power(bool reboot)
{
    if(status_provider()->simulated) {
        printf("bigfoston: %s solicitado (simulador: encerrando o programa)\n",
               reboot ? "reiniciar" : "desligar");
        exit(0);
    }
    const char *cmd = getenv(reboot ? "BIGFOSTON_REBOOT_CMD" : "BIGFOSTON_POWEROFF_CMD");
    if(!cmd || !*cmd) cmd = reboot ? "/sbin/reboot" : "/sbin/poweroff";
    pid_t pid = fork();
    if(pid == 0) {
        execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
        _exit(127);
    }
    int st = -1;
    if(pid > 0) waitpid(pid, &st, 0);
    if(pid < 0 || !WIFEXITED(st) || WEXITSTATUS(st) != 0) {
        static const char *btn[1];
        btn[0] = tr(S_CLOSE);
        dialog_open(BF_SYMBOL_ALERT, pal()->warning, tr(S_RUN_FAILED), cmd, btn, 1, 0, -1,
                    power_err_cb, NULL);
    }
}
