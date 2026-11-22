/* app.c — monta a tela fixa: barra superior (48 px), conteúdo (360 px) e
 * barra inferior (72 px). Um único lv_timer de 1 s atualiza relógio/status. */
#define _DEFAULT_SOURCE
#include "btstat.h"
#include "../screens/screens.h"
#include "app.h"
#include "nav.h"
#include "theme.h"
#include "settings.h"
#include "status.h"
#include "fx.h"
#include "clock.h"
#include "devwatch.h"
#include "powerkey.h"
#include "../widgets/topbar.h"
#include "../widgets/bottombar.h"
#include "../widgets/dialog.h"
#include "../assets/logo/bf_logo.h"
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>

static lv_display_t *disp;
static lv_obj_t *content;
static lv_timer_t *tick;

lv_display_t *app_display(void) { return disp; }
lv_obj_t *app_content(void) { return content; }

static void tick_cb(lv_timer_t *t)
{
    (void)t;
    topbar_update();
    bt_poll_global();
    fx_tick();
    clock_tick();
    /* modo Automático: troca claro/escuro quando passa das 7h/19h */
    if(theme_dark_now() != pal()->dark) app_rebuild();
}

static void build(void)
{
    lv_obj_t *scr = lv_screen_active();
    lv_obj_clean(scr);
    lv_obj_remove_style_all(scr);
    lv_obj_add_style(scr, &bf_st_screen, 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    /* papel de parede fixo atrás do conteúdo (as transições deslizam só o conteúdo) */
    const lv_image_dsc_t *wp = theme_wallpaper();
    if(wp) {
        lv_obj_t *wall = lv_image_create(scr);
        lv_image_set_src(wall, wp);
        lv_obj_set_pos(wall, 0, theme_glass() ? 0 : BF_TOPBAR_H);   /* Glass: atrás da barra translúcida */
        lv_obj_remove_flag(wall, LV_OBJ_FLAG_CLICKABLE);
    }

    lv_obj_t *top = topbar_create(scr);
    lv_obj_set_pos(top, 0, 0);

    content = bf_plain(scr);
    lv_obj_set_pos(content, 0, BF_TOPBAR_H);
    lv_obj_set_size(content, BF_SCREEN_W, BF_CONTENT_H);

    if(!theme_glass()) {                 /* Glass: a navegação fica na barra superior */
        lv_obj_t *bot = bottombar_create(scr);
        lv_obj_set_pos(bot, 0, BF_SCREEN_H - BF_BOTBAR_H);
    }
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
    clock_init();                 /* fuso antes do tema (modo Automático usa a hora local) */
    theme_init(disp);
    build();
    nav_init(content);
    nav_show_now();
    tick = lv_timer_create(tick_cb, 1000, NULL);
    music_service_init();               /* música do celular reconhecida sem abrir a Multimídia */
    fx_init();
    devwatch_init();
    powerkey_init();
    fx_sound(FX_STARTUP);
}

void app_rebuild_now(void)
{
    nav_teardown();
    lv_obj_clean(lv_layer_top());
    i18n_set_lang(settings()->lang);
    char cat[300];
    snprintf(cat, sizeof cat, "%s/lang/%s.txt", data_dir(), i18n_lang_code(settings()->lang));
    i18n_load_catalog(cat);
    theme_init(disp);
    build();
    nav_set_content(content);
    nav_show_now();
}

static void rebuild_async(void *ud) { (void)ud; app_rebuild_now(); }
void app_rebuild(void) { lv_async_call(rebuild_async, NULL); }

static void power_err_cb(int b, void *ud) { (void)b; (void)ud; }

static bool shutting_down;

/* Reiniciar sem desligar: o reset do watchdog deixa o aparelho desligado,
 * então o Linux reinicia a si mesmo com kexec (firmware/.../bigfoston-reboot) */
#define REBOOT_KEXEC "/usr/sbin/bigfoston-reboot"
bool app_shutting_down(void) { return shutting_down; }

/* Tela de encerramento (como a de abertura): fica até o aparelho apagar. O
 * shell mantém o console de texto do kernel pausado (main.c, on_term). */
lv_obj_t *app_bye_screen(bool reboot)
{
    lv_obj_t *o = lv_obj_create(lv_layer_sys());
    lv_obj_remove_style_all(o);
    lv_obj_set_size(o, BF_SCREEN_W, BF_SCREEN_H);
    lv_obj_set_style_bg_color(o, lv_color_hex(0x0a226a), 0);
    lv_obj_set_style_bg_grad_color(o, lv_color_hex(0x03102e), 0);
    lv_obj_set_style_bg_grad_dir(o, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_add_flag(o, LV_OBJ_FLAG_CLICKABLE);          /* engole os toques */
    lv_obj_t *img = lv_image_create(o);
    lv_image_set_src(img, &bf_logo_big);
    lv_obj_align(img, LV_ALIGN_TOP_MID, 0, 56);
    lv_obj_t *l = bf_label(o, tr(reboot ? S_BYE_REBOOT : S_BYE_OFF), BF_FONT_L, lv_color_hex(0xeef1ff));
    lv_obj_align(l, LV_ALIGN_TOP_MID, 0, 330);
    if(reboot) {
        lv_obj_t *h = bf_label(o, tr(access(REBOOT_KEXEC, X_OK) == 0 ? S_BYE_REBOOT_KEXEC : S_BYE_REBOOT_HINT),
                               BF_FONT_S, lv_color_hex(0x8d9cc8));
        lv_obj_align(h, LV_ALIGN_TOP_MID, 0, 386);
    }
    return o;
}

void app_power(bool reboot)
{
    if(status_provider()->simulated) {
        printf("bigfoston: %s solicitado (simulador: encerrando o programa)\n",
               reboot ? "reiniciar" : "desligar");
        exit(0);
    }
    const char *cmd = getenv(reboot ? "BIGFOSTON_REBOOT_CMD" : "BIGFOSTON_POWEROFF_CMD");
    if(!cmd || !*cmd)
        cmd = !reboot ? "/sbin/poweroff" : access(REBOOT_KEXEC, X_OK) == 0 ? REBOOT_KEXEC : "/sbin/reboot";
    lv_obj_t *bye = app_bye_screen(reboot);      /* camada do sistema: cobre o diálogo */
    lv_refr_now(NULL);
    shutting_down = true;
    int pfd[2] = { -1, -1 };
    if(pipe(pfd) != 0) pfd[0] = pfd[1] = -1;
    pid_t pid = fork();
    if(pid == 0) {
        if(pfd[1] >= 0) { dup2(pfd[1], 2); close(pfd[0]); close(pfd[1]); }
        execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
        _exit(127);
    }
    if(pfd[1] >= 0) close(pfd[1]);
    /* a mensagem de erro do comando (stderr), para mostrar no diálogo */
    static char err[320];
    size_t n = 0;
    if(pfd[0] >= 0) {
        ssize_t r;
        while(n < sizeof err - 1 && (r = read(pfd[0], err + n, sizeof err - 1 - n)) > 0) n += (size_t)r;
        close(pfd[0]);
    }
    err[n] = 0;
    while(n && (err[n - 1] == '\n' || err[n - 1] == ' ')) err[--n] = 0;
    int st = -1;
    if(pid > 0) waitpid(pid, &st, 0);
    if(pid < 0 || !WIFEXITED(st) || WEXITSTATUS(st) != 0) {
        shutting_down = false;
        lv_obj_delete(bye);
        static const char *btn[1];
        btn[0] = tr(S_CLOSE);
        dialog_open(BF_SYMBOL_ALERT, pal()->warning, tr(S_RUN_FAILED), n ? err : cmd, btn, 1, 0, -1,
                    power_err_cb, NULL);
    }
}
