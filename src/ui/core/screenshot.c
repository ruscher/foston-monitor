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
#include "powerkey.h"
#include "nav.h"
#include "settings.h"
#include "fmstat.h"
#include "../widgets/imgview.h"
#include "status.h"
#include "../widgets/volpop.h"
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
    /* ícone da bateria em cada estado (barra superior + Painel) */
    sim_set_battery(64, 'c'); status_refresh(); pump(1100); shot("01d-bateria-carregando");
    sim_set_battery(100, 'f'); status_refresh(); pump(1100); shot("01e-bateria-carga-completa");
    sim_set_battery(88, 'n'); status_refresh(); pump(1100); shot("01f-bateria-ligado-sem-carregar");
    sim_set_battery(8, 'd'); status_refresh(); pump(1100); shot("01g-bateria-fraca");
    sim_set_battery(64, 'c'); status_refresh();
    go(SCR_POWER, 0);               shot("27b-potencia-carregando");
    sim_set_battery(-2, 0); status_refresh();      /* volta ao padrão (ou BIGFOSTON_SIM_BATTERY) */
    nav_home(); pump(450);
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
    s->home_mode = BF_HOME_PANEL;
    nav_home(); pump(1200);         shot("01h-inicio-painel-tocando");
    /* Foston Glass: tela inicial com a música tocando, a grade Apps e uma tela comum */
    s->home_mode = BF_HOME_GLASS;
    app_rebuild_now(); nav_home(); pump(1500); shot("44-glass-inicio");
    go(SCR_HOME, HOME_ARG_APPS); pump(600); shot("44b-glass-apps");
    go(SCR_APPEARANCE, 0); pump(200); shot("44c-glass-aparencia");
    {   /* Rádio FM: transmissor ligado (estado de demonstração; o simulador não tem o bffm) */
        char fp[300], fc[300];
        data_path(fp, sizeof fp, "fm-demo-status");
        data_path(fc, sizeof fc, "fm.conf");
        FILE *o = fopen(fc, "w");
        if(o) { fputs("freq=995\nmode=0\ntimer=30\nhelp=1\nfav=995|Carro\nfav=1013|Rádio de casa\nfav=881|Reserva\n", o); fclose(o); }
        o = fopen(fp, "w");
        if(o) { fprintf(o, "state=desligado\nfreq=0.0\n"); fclose(o); }
        setenv("BIGFOSTON_FM_STATUS", fp, 1);
        go(SCR_FM, 0); pump(500); shot("44d-glass-radio-desligado");
        fm_on(995, 0); pump(900); shot("44f-glass-radio-transmitindo");
        scr_fm_demo(1); pump(300); shot("44g-glass-radio-digitar");
        dialog_close_all(); pump(100);
        scr_fm_demo(2); pump(300); shot("44h-glass-radio-diagnostico");
        dialog_close_all(); pump(100);
    }
    s->home_mode = BF_HOME_GRID;
    app_rebuild_now(); pump(50);
    go(SCR_MUSIC, 0); pump(200);
    scr_music_demo(1); pump(100);   shot("21c-multimidia-lista");
    scr_music_demo(2); pump(800);   shot("21d-multimidia-tela-cheia");
    scr_music_demo(4); pump(200);   shot("21e-multimidia-opcoes");
    scr_music_demo(5); pump(800);   shot("21f-multimidia-led-fogo");
    scr_music_demo(6); pump(800);   shot("21g-multimidia-espelho-arco-iris");
    scr_music_demo(7); pump(800);   shot("21h-multimidia-linha-claro");
    volpop_open(560, 120); pump(200); shot("21i-volume");
    volpop_close();
    scr_music_demo(8); pump(100);
    scr_music_demo(3); pump(100);
    go(SCR_FM, 0); pump(500);       shot("22-fm-transmitir");
    fm_off(); pump(100);
    unsetenv("BIGFOSTON_FM_STATUS");
    go(SCR_TERM, 0); pump(800);
    scr_term_demo_type("echo 'olá, ação, ñandú'; printf 'normal \\033[7minverso\\033[0m fim\\n'; ls /\r");
    pump(1200);                     shot("23-terminal");
    {   /* GPS com um estado de demonstração (posição 3D e procurando) */
        char gp[300];
        data_path(gp, sizeof gp, "gps-demo-status");
        FILE *gf = fopen(gp, "w");
        if(gf) {
            fprintf(gf, "updated=0\nepochs=120\ntime=145230.000\ndate=211126\nvalid=1\nmode=3\nquality=1\n"
                        "lat=-23.550520\nlon=-46.633308\nalt=760.0\nspeed_kmh=42.5\ncourse=87.0\nhdop=0.9\n"
                        "used=8\nview=11\nsats=2:62:40:44,5:35:310:38,7:18:120:31,9:71:200:46,12:25:80:27,"
                        "13:48:150:41,15:9:20:-1,17:55:260:40,19:30:350:35,24:12:220:18,28:5:100:-1\n");
            fclose(gf);
            setenv("BIGFOSTON_GPS_STATUS", gp, 1);
            go(SCR_GPS, 0); pump(200);  shot("38-gps-posicao");
            gf = fopen(gp, "w");
            if(gf) {
                fprintf(gf, "updated=0\nepochs=12\ntime=145002.000\ndate=211126\nvalid=0\nmode=1\n"
                            "hdop=99.99\nused=0\nview=3\nsats=5:40:120:22,9:15:300:-1,17:60:30:19\n");
                fclose(gf);
            }
            go(SCR_GPS, 0); pump(1300); shot("38b-gps-procurando");
            /* Big GPS: só com um mapa em $BIGFOSTON_BIGGPS (o DF de docs/big-gps) */
            if(getenv("BIGFOSTON_BIGGPS")) {
                gf = fopen(gp, "w");
                if(gf) {
                    fprintf(gf, "updated=0\nepochs=300\ntime=150010.000\ndate=211126\nvalid=1\nmode=3\nquality=1\n"
                                "lat=-15.793900\nlon=-47.882800\nalt=1100.0\nspeed_kmh=38.0\ncourse=62.0\nhdop=0.8\n"
                                "used=9\nview=12\nsats=2:62:40:44\n");
                    fclose(gf);
                }
                go(SCR_MAP, 0); pump(3000); shot("42-biggps-mapa");
                scr_map_demo(3); pump(300); shot("42e-biggps-busca");
                scr_map_demo(4); pump(300); shot("42f-biggps-busca-hospital");
                scr_map_demo(5); pump(100);
                scr_map_demo(0); pump(4000); shot("42b-biggps-rota");
                scr_map_demo(1); pump(2500); shot("42c-biggps-navegacao");
                scr_map_demo(2); pump(2500); shot("42d-biggps-navegacao-adiante");
                go(SCR_HOME, 0); pump(100);
            }
            unsetenv("BIGFOSTON_GPS_STATUS");
        }
    }
    {   /* Bluetooth com um estado de demonstração (endereços inventados) */
        char bp[300];
        data_path(bp, sizeof bp, "bt-demo-status");
        FILE *bf = fopen(bp, "w");
        if(bf) {
            fprintf(bf, "updated=0\nenabled=1\nagent=1\nadapter=1\npowered=1\ndiscoverable=0\ndiscovering=0\n"
                        "name=BigFoston\ndev=02:00:00:00:00:01|1|1|1|0|phone|Celular da Ana\n"
                        "dev=02:00:00:00:00:02|1|0|1|0|audio-headphones|Fone JBL\n"
                        "dev=02:00:00:00:00:03|0|0|0|-62|audio-card|Caixa de som\n"
                        "player=playing|Aquarela|Toquinho|Celular da Ana|Casa de Brinquedos|258000|61000|0\n"
                        "stream=active\nresult=connect|ok|\n");
            fclose(bf);
            setenv("BIGFOSTON_BT_STATUS", bp, 1);
            go(SCR_BLUETOOTH, 0); pump(300); shot("43-bluetooth");
            go(SCR_MUSIC, 0); pump(200); scr_music_demo(9); pump(400); shot("43c-multimidia-bluetooth");
            s->home_mode = BF_HOME_GLASS;                          /* Glass com a música do celular */
            app_rebuild_now(); nav_home(); pump(1200); shot("44e-glass-musica-bluetooth");
            s->home_mode = BF_HOME_GRID;
            app_rebuild_now(); pump(50);
            bf = fopen(bp, "w");
            if(bf) {
                fprintf(bf, "updated=0\nenabled=1\nagent=1\nadapter=1\npowered=1\ndiscoverable=1\ndiscovering=0\n"
                            "name=BigFoston\nreq=confirm|482913|02:00:00:00:00:04|Celular novo\n");
                fclose(bf);
            }
            pump(1300); shot("43b-bluetooth-parear");
            dialog_close_all();
            unlink(bp);
            unsetenv("BIGFOSTON_BT_STATUS");
            go(SCR_HOME, 0); pump(100);
        }
    }
    {   /* Telefone com dados inventados (nomes e números fictícios) */
        char pp[300], dd[300], f[400];
        data_path(pp, sizeof pp, "phone-demo-status");
        data_path(dd, sizeof dd, "phone-demo");
        mkdir(dd, 0700);
        static const char *const files[][2] = {
            { "contatos.tsv", "Ana Souza\tcel:+55 11 90000-0001;casa:+55 11 3000-0001\tana@exemplo.org\t\n"
                              "Bruno Lima\tcel:+55 21 90000-0002\t\tOficina\n"
                              "Carla Mendes\tcel:+55 31 90000-0003\t\t\n"
                              "Diego Rocha\ttrabalho:+55 41 3000-0004\t\tTransportes\n"
                              "Elisa Martins\tcel:+55 51 90000-0005\t\t\n" },
            { "historico.tsv", "R\tAna Souza\t+55 11 90000-0001\t20261122T141200\n"
                               "P\t\t+55 61 90000-0009\t20261122T120500\n"
                               "F\tBruno Lima\t+55 21 90000-0002\t20261121T183000\n" },
            { "mensagens.tsv", "0\t20261122T143000\tAna Souza\t+55 11 90000-0001\tChego em 20 minutos, pode deixar o portão aberto?\t1\n"
                               "1\t20261122T090000\tCarla Mendes\t+55 31 90000-0003\tBom dia! A reunião foi para as 15h.\t2\n" },
            { "enviadas.tsv", "1\t20261122T143500\tAna Souza\t+55 11 90000-0001\tCombinado!\t3\n" },
            { "sincronizado", "1795350000\n" },
        };
        for(size_t i = 0; i < sizeof files / sizeof files[0]; i++) {
            snprintf(f, sizeof f, "%s/%s", dd, files[i][0]);
            FILE *o = fopen(f, "w");
            if(o) { fputs(files[i][1], o); fclose(o); }
        }
        FILE *pf = fopen(pp, "w");
        if(pf) {
            fprintf(pf, "updated=0\nlink=1\ndev=Celular da Ana\ndata=%s\noperator=Operadora\nsignal=4\nbattery=3\n"
                        "service=1\nvoice=phone\nsco=0\nsync=ok\n", dd);
            fclose(pf);
            setenv("BIGFOSTON_PHONE_STATUS", pp, 1);
            go(SCR_PHONE, 0); pump(300); shot("45-telefone-discador");
            scr_phone_demo(1); pump(300); shot("45b-telefone-contatos");
            scr_phone_demo(2); pump(300); shot("45c-telefone-recentes");
            scr_phone_demo(3); pump(300); shot("45d-telefone-mensagens");
            pf = fopen(pp, "a");
            if(pf) { fputs("msg_auto=1\nnotify=1\n", pf); fclose(pf); }
            pump(600); shot("45k-telefone-mensagens-auto");
            scr_phone_demo(15); pump(300); shot("45l-telefone-importacao-automatica");
            dialog_close_all(); pump(100);
            scr_phone_demo(14); pump(300); shot("45m-telefone-conversa");
            scr_phone_demo(16); pump(300); shot("45n-telefone-responder");
            scr_phone_demo(3); pump(100);
            pf = fopen(pp, "a");
            if(pf) { fprintf(pf, "call=1|0|0|+55 11 90000-0001|Ana Souza\nsince=%ld\nvoice=foston\nvoice_foston=1\n"
                                 "voice_on=1\nsco=1\nmute=0\nvol=5\nrec=1\n", (long)time(NULL) - 83); fclose(pf); }
            pump(800); shot("45e-telefone-ligacao");
            scr_phone_demo(11); pump(300); shot("45h-telefone-volume");
            scr_phone_demo(12); pump(300); shot("45i-telefone-teclado");
            scr_phone_demo(13); pump(100);
            pf = fopen(pp, "w");
            if(pf) {
                fprintf(pf, "updated=0\nlink=1\ndev=Celular da Ana\ndata=%s\nsignal=4\nbattery=3\nservice=1\n"
                            "call=1|0|1|+55 11 90000-0001|Ana Souza\ncall=2|0|0|+55 21 90000-0002|Bruno Lima\n"
                            "since=%ld\nvoice=foston\nvoice_foston=1\nvoice_on=1\nsco=1\nmute=0\nvol=5\nrec=0\nsync=ok\n",
                        dd, (long)time(NULL) - 12);
                fclose(pf);
            }
            pump(800); shot("45j-telefone-duas-chamadas");
            scr_phone_demo(10); pump(300); shot("45g-telefone-falar-pelo-foston");
            dialog_close_all();
            go(SCR_HOME, 0); pump(100);
            pf = fopen(pp, "w");
            if(pf) {
                fprintf(pf, "updated=0\nlink=1\ndev=Celular da Ana\ndata=%s\nsignal=4\nbattery=3\nservice=1\n"
                            "call=1|1|4|+55 21 90000-0002|Bruno Lima\nsync=ok\n", dd);
                fclose(pf);
            }
            pump(1300); shot("45f-telefone-chamada-recebida");
            dialog_close_all();
            unlink(pp);
            unsetenv("BIGFOSTON_PHONE_STATUS");
            go(SCR_HOME, 0); pump(100);
        }
    }
    {   /* TV com canais e uma gravação inventados (o simulador não roda o bftvd) */
        char td[300], f[400], sp[300];
        data_path(td, sizeof td, "tv-demo");
        mkdir(td, 0755);
        snprintf(f, sizeof f, "%s/gravacoes", td);
        mkdir(f, 0755);
        static const char *const tf[][2] = {
            { "bftvd", "#!/bin/sh\n" }, { "isdbt_nova_12mhz_b0.inp", "x" },
            { "canais.tsv", "15\t479.143\tTV Brasil 1seg\t19\t-62\n18\t497.143\tRede Vida\t15\t-70\n"
                            "20\t509.143\tCultura\t21\t-58\n27\t551.143\t\t9\t-80\n" },
            { "gravacoes/tv-20261122-203015-c15.ts", "" },
        };
        for(size_t i = 0; i < sizeof tf / sizeof tf[0]; i++) {
            snprintf(f, sizeof f, "%s/%s", td, tf[i][0]);
            FILE *o = fopen(f, "w");
            if(o) { fputs(tf[i][1], o); fclose(o); }
            if(i == 0) chmod(f, 0755);
        }
        data_path(sp, sizeof sp, "tv-demo-status");
        FILE *o = fopen(sp, "w");
        if(o) { fputs("state=tocando\nfreq=479.143\nlocked=1\nsnr=19\nrssi=-62\nfps=13.9\n", o); fclose(o); }
        setenv("BIGFOSTON_TV_DIR", td, 1);
        setenv("BIGFOSTON_TV_STATUS", sp, 1);
        bf_settings_t *ts = settings();              /* o FOSTON usa o Foston Glass */
        int was = ts->home_mode;
        ts->home_mode = BF_HOME_GLASS;
        app_rebuild_now(); pump(50);
        go(SCR_TV, 0); pump(300); shot("46-tv");
        scr_tv_demo(5); pump(300); shot("46f-tv-volume");
        scr_tv_demo(1); pump(300); shot("46b-tv-canais");
        scr_tv_demo(2); pump(300); shot("46c-tv-gravacoes");
        scr_tv_demo(3); pump(300); shot("46d-tv-busca");
        scr_tv_demo(4); pump(300); shot("46e-tv-entrada-av");
        go(SCR_HOME, 0); pump(600); shot("46h-inicio-depois-da-tv");     /* papel de parede e barra voltam */
        ts->home_mode = was;
        app_rebuild_now(); pump(50);
        go(SCR_TV, 0); pump(300); shot("46g-tv-classico");
        go(SCR_HOME, 0); pump(100);
        unsetenv("BIGFOSTON_TV_DIR");
        unsetenv("BIGFOSTON_TV_STATUS");
    }
    go(SCR_GALLERY, 0);             scr_gallery_demo(0); pump(200); shot("37-galeria");
    scr_gallery_demo(1); pump(300); shot("37b-galeria-foto");
    scr_gallery_demo(2); pump(50);
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
    powerkey_press();               shot("25-dialogo-desligar");   /* botão físico abre */
    bottombar_open_power();         /* tocar em Desligar com ele aberto não duplica */
    powerkey_press(); pump(30);     /* segundo toque no botão fecha */
    if(bottombar_power_is_open())
        fprintf(stderr, "screenshot: ERRO: o diálogo de energia não fechou (%u na camada)\n",
                (unsigned)lv_obj_get_child_count(lv_layer_top()));
    powerkey_press(); pump(30);     /* abre de novo e toca em Hibernar */
    {
        lv_obj_t *bd = lv_obj_get_child(lv_layer_top(), -1);   /* o diálogo é o mais novo */
        lv_obj_t *card = bd ? lv_obj_get_child(bd, 0) : NULL;
        lv_obj_t *row = card ? lv_obj_get_child(card, -1) : NULL;
        lv_obj_t *hib = row ? lv_obj_get_child(row, 1) : NULL;
        if(hib) lv_obj_send_event(hib, LV_EVENT_CLICKED, NULL);
        pump(30);
    }
    if(!powerkey_sleeping() || bottombar_power_is_open())
        fprintf(stderr, "screenshot: ERRO: hibernar (dormindo %d, aberto %d)\n",
                powerkey_sleeping(), bottombar_power_is_open());
    powerkey_press(); pump(30);     /* o botão acorda, sem abrir o diálogo */
    if(powerkey_sleeping() || bottombar_power_is_open())
        fprintf(stderr, "screenshot: ERRO: acordar\n");
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
