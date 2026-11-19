/* main.c — bigfoston-shell: interface gráfica do BigFoston OS (LVGL 9.2).
 *
 * Builds:
 *   make sim     -> janela SDL2 800x480 (mouse = toque) + modo --screenshot
 *   make target  -> /dev/fb0 (lv_linux_fbdev) + evdev (lv_evdev), ARMv6 soft-float
 *
 * Variáveis de ambiente:
 *   BIGFOSTON_DATA          pasta de dados (padrão /data/bigfoston no alvo, ./data no sim)
 *   BIGFOSTON_FBDEV         framebuffer (padrão /dev/fb0)
 *   BIGFOSTON_TOUCH         dispositivo evdev do toque (padrão: autodetecção)
 *   BIGFOSTON_APPS          pastas de .desktop separadas por ':'
 *   BIGFOSTON_RUN_USER      usuário sem privilégios para o Run (padrão "bigfoston")
 *   BIGFOSTON_ALLOW_ROOT=1  permite o Run executar como root (explícito)
 *   BIGFOSTON_POWEROFF_CMD / BIGFOSTON_REBOOT_CMD
 */
#define _DEFAULT_SOURCE
#include "lvgl.h"
#include "core/app.h"
#include "core/settings.h"
#include "core/status.h"
#include "core/screenshot.h"
#include "core/touch_calib.h"
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#ifdef BIGFOSTON_TARGET
#include <dirent.h>
#include <fcntl.h>
#include <linux/input.h>
#include <sys/ioctl.h>
#endif

static uint32_t tick_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000u + (uint32_t)(ts.tv_nsec / 1000000));
}

static void usage(const char *argv0)
{
    printf("uso: %s [opções]\n"
           "  --screenshot DIR   renderiza cada tela em DIR/*.png (800x480) e sai (sem janela)\n"
           "  --real-status      usa sysfs/proc reais para a barra de status\n"
           "  --sim-status       usa valores simulados (padrão do simulador)\n"
           "  --lang pt|en|es    língua (não salva)\n"
           "  --theme dark|light tema (não salvo)\n"
           "  --data DIR         pasta de dados (= BIGFOSTON_DATA)\n"
           "  --help\n", argv0);
}

#ifdef BIGFOSTON_TARGET
#define TEST_BIT(b, a) ((a[(b) / (8 * sizeof(long))] >> ((b) % (8 * sizeof(long)))) & 1)
/* Procura um /dev/input/eventN com ABS_X/ABS_Y e BTN_TOUCH (touchscreen). */
static const char *find_touch(char *buf, size_t n)
{
    DIR *d = opendir("/dev/input");
    if(!d) return NULL;
    struct dirent *e;
    const char *found = NULL;
    while(!found && (e = readdir(d))) {
        if(strncmp(e->d_name, "event", 5)) continue;
        snprintf(buf, n, "/dev/input/%s", e->d_name);
        int fd = open(buf, O_RDONLY | O_NONBLOCK);
        if(fd < 0) continue;
        unsigned long absb[(ABS_MAX + 1) / (8 * sizeof(long)) + 1] = { 0 };
        unsigned long keyb[(KEY_MAX + 1) / (8 * sizeof(long)) + 1] = { 0 };
        ioctl(fd, EVIOCGBIT(EV_ABS, sizeof absb), absb);
        ioctl(fd, EVIOCGBIT(EV_KEY, sizeof keyb), keyb);
        if(TEST_BIT(ABS_X, absb) && TEST_BIT(ABS_Y, absb) && TEST_BIT(BTN_TOUCH, keyb)) found = buf;
        close(fd);
    }
    closedir(d);
    return found;
}
#endif

int main(int argc, char **argv)
{
    const char *shot_dir = NULL, *data = NULL, *lang = NULL, *theme = NULL;
#ifdef BIGFOSTON_TARGET
    const bf_status_provider_t *prov = &bf_status_linux;
#else
    const bf_status_provider_t *prov = &bf_status_sim;
#endif
    for(int i = 1; i < argc; i++) {
        if(!strcmp(argv[i], "--screenshot") && i + 1 < argc) shot_dir = argv[++i];
        else if(!strcmp(argv[i], "--real-status")) prov = &bf_status_linux;
        else if(!strcmp(argv[i], "--sim-status")) prov = &bf_status_sim;
        else if(!strcmp(argv[i], "--lang") && i + 1 < argc) lang = argv[++i];
        else if(!strcmp(argv[i], "--theme") && i + 1 < argc) theme = argv[++i];
        else if(!strcmp(argv[i], "--data") && i + 1 < argc) data = argv[++i];
        else { usage(argv[0]); return strcmp(argv[i], "--help") ? 2 : 0; }
    }

    signal(SIGPIPE, SIG_IGN);
    data_dir_init(data);
    settings_load();
    if(lang) settings()->lang = i18n_lang_from_code(lang);
    if(theme) settings()->dark = strcmp(theme, "light") != 0;
    status_select(prov);

    lv_init();
    lv_tick_set_cb(tick_ms);

    if(shot_dir) return screenshot_run(shot_dir);

    lv_display_t *disp = NULL;
#if defined(BIGFOSTON_TARGET)
    const char *fbdev = getenv("BIGFOSTON_FBDEV");
    disp = lv_linux_fbdev_create();
    lv_linux_fbdev_set_file(disp, fbdev && *fbdev ? fbdev : "/dev/fb0");
    char tbuf[300];
    const char *touch = getenv("BIGFOSTON_TOUCH");
    if(!touch || !*touch) touch = find_touch(tbuf, sizeof tbuf);
    if(touch) {
        lv_indev_t *in = lv_evdev_create(LV_INDEV_TYPE_POINTER, touch);
        if(!in) fprintf(stderr, "bigfoston: não foi possível abrir o toque %s\n", touch);
        else fprintf(stderr, "bigfoston: toque em %s\n", touch);
    }
    else fprintf(stderr, "bigfoston: nenhum touchscreen encontrado em /dev/input (defina BIGFOSTON_TOUCH)\n");
    bf_calib_t cal;
    if(calib_load(&cal)) touch_calibration_apply(&cal);
#elif defined(BIGFOSTON_SIM_SDL)
    disp = lv_sdl_window_create(800, 480);
    lv_sdl_mouse_create();
    lv_group_t *g = lv_group_create();
    lv_group_set_default(g);
    lv_indev_t *kbd = lv_sdl_keyboard_create();
    lv_indev_set_group(kbd, g);
#else
    fprintf(stderr, "bigfoston: build sem driver de vídeo; use --screenshot DIR\n");
    return 2;
#endif
    if(!disp) { fprintf(stderr, "bigfoston: falha ao abrir o display\n"); return 1; }

    app_init(disp);
    app_apply_hw_settings();

    for(;;) {
        uint32_t idle = lv_timer_handler();
        if(idle == LV_NO_TIMER_READY || idle > 100) idle = 100;
        usleep(idle * 1000);
    }
    return 0;
}
