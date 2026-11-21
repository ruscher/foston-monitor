/* screenshot.c — renderização sem janela (display LVGL em memória, RGB565
 * igual ao do aparelho) e gravação PNG com lodepng. Usado para revisar as
 * telas sem monitor e para gerar docs/images/ui/. */
#define _DEFAULT_SOURCE
#include "screenshot.h"
#include <stdio.h>

#if !BIGFOSTON_SCREENSHOT
int screenshot_run(const char *dir)
{
    fprintf(stderr, "bigfoston: --screenshot não incluído neste build (SCREENSHOT=1)\n");
    (void)dir;
    return 2;
}
#else
#include "app.h"
#include "devwatch.h"
#include "nav.h"
#include "settings.h"
#include "../widgets/imgview.h"
#include "status.h"
#include "theme.h"
#include "../screens/screens.h"
#include "../widgets/bottombar.h"
#include "../widgets/dialog.h"
#include "lodepng.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>

static lv_display_t *disp;
static uint8_t *fb;
static const char *outdir;
static int n_ok;

static void flush_cb(lv_display_t *d, const lv_area_t *a, uint8_t *px)
{
    (void)a; (void)px;
    lv_display_flush_ready(d);
}

static uint32_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

static void pump(uint32_t ms)
{
    uint32_t end = now_ms() + ms;
    do {
        lv_timer_handler();
        usleep(5000);
    } while((int32_t)(end - now_ms()) > 0);
}

static void shot(const char *name)
{
    pump(60);
    lv_refr_now(disp);
    unsigned w = BF_SCREEN_W, h = BF_SCREEN_H;
    unsigned char *rgb = malloc(w * h * 3);
    if(!rgb) return;
    const uint16_t *src = (const uint16_t *)fb;
    for(unsigned i = 0; i < w * h; i++) {
        uint16_t c = src[i];
        unsigned r = (c >> 11) & 0x1f, g = (c >> 5) & 0x3f, b = c & 0x1f;
        rgb[i * 3 + 0] = (unsigned char)((r << 3) | (r >> 2));
        rgb[i * 3 + 1] = (unsigned char)((g << 2) | (g >> 4));
        rgb[i * 3 + 2] = (unsigned char)((b << 3) | (b >> 2));
    }
    char path[512];
    snprintf(path, sizeof path, "%s/%s.png", outdir, name);
    unsigned err = lodepng_encode24_file(path, rgb, w, h);
    free(rgb);
    if(err) fprintf(stderr, "screenshot: %s: %s\n", path, lodepng_error_text(err));
    else { printf("screenshot: %s\n", path); n_ok++; }
}

static void go(bf_screen_t s, int arg)
{
    dialog_close_all();
    nav_go(s, arg);
    pump(250);                      /* transição deslizante de 170 ms */
}

static void scroll_content_end(void)
{
    /* rola o primeiro filho rolável do conteúdo até o fim */
    lv_obj_t *content = app_content();
    lv_obj_t *pg = content ? lv_obj_get_child(content, 0) : NULL;
    if(pg) { lv_obj_scroll_to_y(pg, LV_COORD_MAX, LV_ANIM_OFF); lv_obj_scroll_to_y(pg, lv_obj_get_scroll_y(pg) + lv_obj_get_scroll_bottom(pg), LV_ANIM_OFF); }
}

static void wait_runner(void)
{
    extern bool runner_busy(void);
    for(int i = 0; i < 100 && runner_busy(); i++) pump(50);
    pump(150);
}

int screenshot_run(const char *dir)
{
    outdir = dir;
    char cmd[600];
    snprintf(cmd, sizeof cmd, "%s", dir);
    for(char *p = cmd + 1; *p; p++) if(*p == '/') { *p = 0; mkdir(cmd, 0755); *p = '/'; }
    mkdir(cmd, 0755);

    disp = lv_display_create(BF_SCREEN_W, BF_SCREEN_H);
    lv_display_set_color_format(disp, LV_COLOR_FORMAT_RGB565);
    size_t sz = (size_t)BF_SCREEN_W * BF_SCREEN_H * 2;
    fb = lv_malloc(sz);
    memset(fb, 0, sz);
    lv_display_set_buffers(disp, fb, NULL, (uint32_t)sz, LV_DISPLAY_RENDER_MODE_DIRECT);
    lv_display_set_flush_cb(disp, flush_cb);

    /* ajustes determinísticos (não são salvos) */
    bf_settings_t *s = settings();
    settings_defaults(s);
    app_init(disp);
    pump(600);                      /* entrada animada dos ícones */

    shot("01-inicio");
    scr_home_show_page(1);
    shot("02-inicio-pagina-2");
    scr_home_show_page(0);
    go(SCR_SETTINGS, 0);            shot("03-ajustes");
    go(SCR_APPEARANCE, 0);          shot("03b-aparencia");
    s->home_mode = BF_HOME_PANEL;
    nav_home(); pump(450);          shot("01b-inicio-painel");
    s->home_mode = BF_HOME_KIOSK;
    app_rebuild_now(); pump(450);   shot("01c-inicio-quiosque");
    s->home_mode = BF_HOME_GRID;
    app_rebuild_now(); pump(50);
    go(SCR_APPEARANCE, 0);
    scroll_content_end();           shot("03c-aparencia-fim");
    scroll_content_end();           shot("04-ajustes-fim");
    go(SCR_SYSINFO, 0);             shot("05-sistema-de-informacao");
    go(SCR_MONITOR, 0);  pump(6500); shot("06-monitor");
    go(SCR_RUN, 0);                 shot("07-run");
    scr_run_demo_keyboard();        shot("08-run-teclado");
    nav_home(); pump(50);
    go(SCR_RUN, 0);
    scr_run_demo_command("uname -a; echo; id; echo; ls / | head -n 8; echo 'olá, ação, ñandú'");
    wait_runner();                  shot("09-run-saida");
    go(SCR_CALIBRATE, 0);           shot("10-calibrar");
    scr_calibrate_demo_step(1);     shot("11-calibrar-alvo");
    scr_calibrate_demo_step(5);     shot("12-calibrar-resultado");
    nav_back(); pump(50);
    go(SCR_BACKLIGHT, 0);           shot("13-luz-posterior");
    go(SCR_VOLUME, 0);              shot("14-volume");
    go(SCR_LANGUAGE, 0);            shot("15-lingua");
    go(SCR_DATE, 0);                shot("16-data");
    scroll_content_end();           shot("16b-data-fim");
    go(SCR_DATE, 1);                shot("16c-data-ajustar");
    go(SCR_DATE, 2);                shot("16d-data-fuso");
    go(SCR_POWER, 0);               shot("17-potencia");
    go(SCR_RESET, 0);               shot("18-reset-settings");
    go(SCR_SSH, 0);                 shot("19-ssh");
    go(SCR_ADVANCED, 0);            shot("20-avancado");
    go(SCR_MUSIC, 0);               shot("21-multimidia");
    scr_music_demo(0); pump(2500);  shot("21b-multimidia-tocando");
    scr_music_demo(1); pump(100);   shot("21c-multimidia-lista");
    scr_music_demo(2); pump(800);   shot("21d-multimidia-tela-cheia");
    scr_music_demo(3); pump(100);
    go(SCR_FEATURE, FEAT_FM);       shot("22-fm-transmitir");
    go(SCR_TERM, 0); pump(800);
    scr_term_demo_type("echo 'olá, ação, ñandú'; printf 'normal \\033[7minverso\\033[0m fim\\n'; ls /\r");
    pump(1200);                     shot("23-terminal");
    go(SCR_BOOK, 0);                shot("24-livro");
    {
        char f[600];
        scr_book_demo("+covers"); pump(50); shot("24-livro");
        snprintf(f, sizeof f, "%s/books/com-capa.epub", data_dir());
        scr_book_demo(f); scr_book_demo("+paginate"); pump(50); shot("24f-livro-pagina");
        scr_book_demo("+controls"); pump(50); shot("24g-livro-controles");
        scr_book_demo("+toc"); pump(50);        shot("24h-livro-sumario");
        scr_book_demo("+close");
        snprintf(f, sizeof f, "%s/books/documento.pdf", data_dir());
        scr_book_demo(f); scr_book_demo("+paginate"); pump(50); shot("24i-livro-pdf");
        snprintf(f, sizeof f, "%s/books/desenho.svg", data_dir());
        scr_book_demo(f); pump(50);              shot("24j-livro-svg");
        imgview_close();
        scr_book_demo(NULL);
    }
    go(SCR_FILES, 0);               shot("24b-arquivos-locais");
    scr_files_demo(data_dir());      shot("24c-arquivos-pasta");
    {
        char f[600];
        snprintf(f, sizeof f, "%s/notas.txt", data_dir());
        scr_files_demo(f);           shot("24d-arquivos-texto");
        snprintf(f, sizeof f, "%s/cover.jpg", data_dir());
        scr_files_demo(f);           shot("24e-arquivos-imagem");
        scr_files_demo(data_dir());
        scr_files_demo("+nova-pasta"); pump(50); shot("24g-arquivos-nova-pasta");
        scr_files_demo("+fechar");
    }
    nav_home(); pump(50);
    bottombar_open_power();         shot("25-dialogo-desligar");
    dialog_close_all(); pump(30);
    bottombar_open_recents();       shot("26-recentes");
    dialog_close_all(); pump(30);
    devwatch_demo(0, 1); pump(300); shot("26d-aviso-fone");
    devwatch_demo(1, 1); pump(300); shot("26e-aviso-carregador");
    pump(3000);
    {
        lv_obj_t *b = app_bye_screen(true); pump(30); shot("26b-reiniciando");
        lv_obj_delete(b);
        b = app_bye_screen(false); pump(30);          shot("26c-desligando");
        lv_obj_delete(b); pump(30);
    }

    /* bateria/brilho "reais" (provedor linux): mostra o estado honesto */
    status_select(&bf_status_linux);
    app_rebuild_now(); pump(50);
    go(SCR_POWER, 0);               shot("27-potencia-real");
    go(SCR_BACKLIGHT, 0);           shot("28-luz-posterior-real");
    nav_home(); pump(450);          shot("29-inicio-status-real");
    status_select(&bf_status_sim);

    s->mode = BF_MODE_LIGHT;
    app_rebuild_now(); pump(50);
    nav_home(); pump(400);          shot("30-tema-claro-inicio");
    go(SCR_SETTINGS, 0);            shot("31-tema-claro-ajustes");
    go(SCR_SYSINFO, 0);             shot("32-tema-claro-sistema");
    go(SCR_APPEARANCE, 0);          shot("32b-tema-claro-aparencia");
    /* temas de cores, escuro e claro */
    for(int i = 1; i < theme_preset_count(); i++) {
        char n[48];
        s->palette = i;
        s->mode = BF_MODE_DARK;
        app_rebuild_now(); pump(50);
        nav_home(); pump(400);
        snprintf(n, sizeof n, "38-tema-%d-escuro", i);  shot(n);
        s->mode = BF_MODE_LIGHT;
        app_rebuild_now(); pump(450);
        snprintf(n, sizeof n, "38-tema-%d-claro", i);   shot(n);
    }
    s->palette = 0;
    s->wallpaper = BF_WALL_PLAIN;
    s->mode = BF_MODE_DARK;
    app_rebuild_now(); pump(50);
    nav_home(); pump(400);          shot("39-papel-liso");
    s->wallpaper = BF_WALL_GRADIENT;
    app_rebuild_now(); pump(50);
    nav_home(); pump(400);          shot("39b-papel-degrade");
    s->wallpaper = BF_WALL_LOGO;
    s->lang = LANG_EN;
    app_rebuild_now(); pump(50);
    nav_home(); pump(450);           shot("33-en-inicio");
    go(SCR_SETTINGS, 0);            shot("34-en-ajustes");
    s->lang = LANG_ES;
    app_rebuild_now(); pump(50);
    go(SCR_FEATURE, FEAT_GPS);      shot("35-es-gps");
    go(SCR_MONITOR, 0); pump(2500); shot("36-es-monitor");

    printf("screenshot: %d imagens em %s\n", n_ok, dir);
    return n_ok > 0 ? 0 : 1;
}
#endif /* BIGFOSTON_SCREENSHOT */
