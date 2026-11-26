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
#include "webremote.h"
#include "hwkbd.h"
#include "gamepad.h"
#include "apps.h"
#include "launcher.h"
#include "appgroups.h"
#include "../screens/launcher_ui.h"
#include "../widgets/ctxmenu.h"
#include "../widgets/namebox.h"
#include "padmap.h"
#include <linux/input.h>
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

/* ---------------------------------------------- teclado físico (core/hwkbd.c) */
static int kb_fail;

static void kfeed(uint16_t code, uint16_t mod)
{
    if(mod) hwkbd_feed(mod, 1);
    hwkbd_feed(code, 1);
    hwkbd_feed(code, 0);
    if(mod) hwkbd_feed(mod, 0);
    pump(15);
}

/* digita como num ABNT2 (só o que os testes usam) */
static void ktype(const char *s)
{
    static const char row1[] = "qwertyuiop", row2[] = "asdfghjkl", row3[] = "zxcvbnm";
    for(; *s; s++) {
        char c = *s, l = (char)(c | 0x20);
        const char *p;
        uint16_t sh = (c >= 'A' && c <= 'Z') ? KEY_LEFTSHIFT : 0;
        if((p = strchr(row1, l)) && l >= 'a') kfeed((uint16_t)(KEY_Q + (p - row1)), sh);
        else if((p = strchr(row2, l)) && l >= 'a') kfeed((uint16_t)(KEY_A + (p - row2)), sh);
        else if((p = strchr(row3, l)) && l >= 'a') kfeed((uint16_t)(KEY_Z + (p - row3)), sh);
        else if(c >= '1' && c <= '9') kfeed((uint16_t)(KEY_1 + (c - '1')), 0);
        else if(c == '0') kfeed(KEY_0, 0);
        else if(c == ' ') kfeed(KEY_SPACE, 0);
        else if(c == '-') kfeed(KEY_MINUS, 0);
        else if(c == '.') kfeed(KEY_DOT, 0);
        else if(c == ',') kfeed(KEY_COMMA, 0);
    }
}

static void kcheck(const char *what, bool ok)
{
    printf("teclado: %s %s\n", ok ? "ok" : "FALHOU", what);
    if(!ok) kb_fail++;
}

static lv_obj_t *find_class(lv_obj_t *o, const lv_obj_class_t *c)
{
    if(lv_obj_check_type(o, c)) return o;
    for(uint32_t i = 0; i < lv_obj_get_child_count(o); i++) {
        lv_obj_t *r = find_class(lv_obj_get_child(o, (int32_t)i), c);
        if(r) return r;
    }
    return NULL;
}

static int dlg_answer = -9;
static void dlg_cb(int b, void *ud) { (void)ud; dlg_answer = b; }

static void keyboard_tests(void)
{
    /* Ajustes > Teclado físico: o campo de teste já recebe o texto, com acentos ABNT2 */
    go(SCR_SETTINGS, 0);
    nav_go(SCR_KEYBOARD, 0); pump(300);
    ktype("Ola");
    kfeed(KEY_COMMA, 0);
    kfeed(KEY_SPACE, 0);
    ktype("a");
    kfeed(KEY_SEMICOLON, 0);                          /* ç */
    kfeed(KEY_APOSTROPHE, 0); ktype("a");              /* ~a = ã */
    ktype("o ");
    kfeed(KEY_LEFTBRACE, 0); ktype("E");               /* ´E = É */
    kfeed(KEY_SEMICOLON, KEY_LEFTSHIFT);              /* Ç */
    lv_obj_t *ta = find_class(lv_screen_active(), &lv_textarea_class);
    const char *t = ta ? lv_textarea_get_text(ta) : "";
    kcheck("campo de teste ABNT2", !strcmp(t, "Ola, a\xC3\xA7\xC3\xA3o \xC3\x89\xC3\x87"));
    kfeed(KEY_BACKSPACE, 0);
    kfeed(KEY_BACKSPACE, KEY_LEFTCTRL);               /* apaga a palavra */
    t = ta ? lv_textarea_get_text(ta) : "";
    kcheck("Backspace e Ctrl+Backspace", !strcmp(t, "Ola, a\xC3\xA7\xC3\xA3o "));
    kfeed(KEY_HOME, 0); ktype("X");
    t = ta ? lv_textarea_get_text(ta) : "";
    kcheck("Home move o cursor", t[0] == 'X');
    shot("23b-teclado-fisico");

    /* Esc com o campo em foco: sai do campo; o segundo volta para Ajustes */
    kfeed(KEY_ESC, 0); pump(50);
    kcheck("Esc sai do campo", ta && !lv_obj_has_state(ta, LV_STATE_FOCUSED));
    kfeed(KEY_ESC, 0); pump(300);
    kcheck("Esc volta", nav_current().scr == SCR_SETTINGS);
    kfeed(KEY_PAGEDOWN, 0); pump(400);
    lv_obj_t *pg = lv_obj_get_child(app_content(), 0);
    kcheck("Page Down rola Ajustes", pg && lv_obj_get_scroll_y(pg) > 0);

    /* diálogo: Enter = botão principal, Esc = fechar (-1) */
    static const char *b2[] = { "Não", "Sim" };
    dialog_open(NULL, lv_color_white(), "Teste", NULL, b2, 2, 1, -1, dlg_cb, NULL); pump(50);
    kfeed(KEY_ENTER, 0); pump(50);
    kcheck("Enter no diálogo", dlg_answer == 1 && !dialog_is_open());
    dialog_open(NULL, lv_color_white(), "Teste", NULL, b2, 2, 1, -1, dlg_cb, NULL); pump(50);
    kfeed(KEY_ESC, 0); pump(50);
    kcheck("Esc no diálogo", dlg_answer == -1 && !dialog_is_open() && nav_current().scr == SCR_SETTINGS);

    /* Arquivos/Run: o teclado da tela com campo; Enter = OK dele */
    go(SCR_RUN, RUN_ARG_NORMAL); pump(300);
    lv_obj_t *rta = find_class(lv_screen_active(), &lv_textarea_class);
    if(rta) lv_obj_send_event(rta, LV_EVENT_CLICKED, NULL);
    pump(100);
    ktype("echo teclado-ok");
    t = rta ? lv_textarea_get_text(rta) : "";
    kcheck("Run recebe o texto", strstr(t, "echo teclado-ok") != NULL);
    shot("23c-run-teclado");

    /* Terminal: tudo vai para o shell; o teclado da tela some na primeira tecla */
    go(SCR_TERM, 0); pump(500);
    ktype("echo TECLADO ");
    kfeed(KEY_LEFTBRACE, 0); ktype("a");               /* á pelo shell */
    kfeed(KEY_ENTER, 0);
    pump(800);
    kfeed(KEY_L, KEY_LEFTCTRL);                       /* Ctrl+L limpa a tela */
    pump(400);
    ktype("echo fim");
    kfeed(KEY_ENTER, 0);
    pump(800);
    shot("23d-terminal-teclado");
    kfeed(KEY_ESC, 0); pump(300);
    kcheck("Esc no Terminal vai para o shell", nav_current().scr == SCR_TERM);

    /* Internet: na página inicial, digitar abre o endereço com a letra; Esc fecha */
    go(SCR_WEB, 0); pump(400);
    ktype("w");
    pump(100);
    lv_obj_t *wta = find_class(lv_screen_active(), &lv_textarea_class);
    t = wta ? lv_textarea_get_text(wta) : "";
    kcheck("Internet: digitar abre o endereço", !strcmp(t, "w"));
    ktype("ikipedia");
    shot("23e-internet-teclado");
    kfeed(KEY_ESC, 0); pump(100);
    kcheck("Internet: Esc fecha o endereço", !find_class(lv_screen_active(), &lv_textarea_class));
    kfeed(KEY_L, KEY_LEFTCTRL); pump(100);
    kcheck("Internet: Ctrl+L abre o endereço", find_class(lv_screen_active(), &lv_textarea_class) != NULL);
    kfeed(KEY_ESC, 0); pump(100);
    printf("teclado: %d falha(s)\n", kb_fail);
}

/* ---------------------------------------------- controle de jogo (core/gamepad.c) */
static void pev(int slot, uint16_t type, uint16_t code, int32_t v) { gamepad_event(slot, type, code, v); pump(20); }
static void pbtn(int slot, uint16_t code) { pev(slot, EV_KEY, code, 1); pev(slot, EV_KEY, code, 0); pump(30); }

static int pad_clicks;
static void pad_click(lv_event_t *e) { (void)e; pad_clicks++; }

static void gamepad_tests(void)
{
    /* DualShock 4 sem driver (hid-generic): □=0x130 ✕=0x131 ○=0x132 △=0x133 ... PS=0x13c,
     * analógicos 0..255, direcional no HAT */
    static gp_axis_t ax[GP_NABS];
    static pm_bits_t bits, sbits;
    for(unsigned c = ABS_X; c <= ABS_RZ; c++) { ax[c] = (gp_axis_t){ 0, 255, true }; pm_bits_set(&bits, false, c); }
    ax[ABS_HAT0X] = ax[ABS_HAT0Y] = (gp_axis_t){ -1, 1, true };
    pm_bits_set(&bits, false, ABS_HAT0X);
    pm_bits_set(&bits, false, ABS_HAT0Y);
    sbits = bits;
    for(unsigned c = 0x130; c <= 0x13d; c++) pm_bits_set(&bits, true, c);
    static const unsigned skeys[] = { BTN_SOUTH, BTN_EAST, BTN_NORTH, BTN_WEST, BTN_TL, BTN_TR, BTN_TL2, BTN_TR2,
                                      BTN_SELECT, BTN_START, BTN_MODE, BTN_THUMBL, BTN_THUMBR };
    for(size_t i = 0; i < sizeof skeys / sizeof skeys[0]; i++) pm_bits_set(&sbits, true, skeys[i]);
    const gp_id_t ds4id = { BUS_BLUETOOTH, 0x054c, 0x09cc, 0x0100 }, stdid = { BUS_BLUETOOTH, 0x1234, 0x5678, 1 };
    int ds4 = gamepad_attach("Wireless Controller", &ds4id, &bits, ax, "");
    gp_info_t gi;
    kcheck("controle: DS4 sem driver usa o mapa embutido",
           gamepad_info(ds4, &gi) && gi.map_from == GPM_BUILTIN && !strcmp(gi.guid, "050000004c050000cc09000000010000"));
    kcheck("controle: conectado", ds4 >= 0 && gamepad_count() == 1);
    for(int a = ABS_X; a <= ABS_RZ; a++) pev(ds4, EV_ABS, (uint16_t)a, 128);

    go(SCR_SETTINGS, 0);
    int32_t x0, y0, x1, y1;
    kcheck("controle: cursor escondido no início", !gamepad_cursor(&x0, &y0));
    pbtn(ds4, 0x131);                                  /* ✕ com o cursor escondido: só mostra */
    kcheck("controle: ✕ mostra o cursor", gamepad_cursor(&x0, &y0));

    /* ✕ toca onde o cursor está */
    lv_obj_t *b = lv_button_create(lv_layer_top());
    lv_obj_set_size(b, 120, 80);
    lv_obj_set_pos(b, x0 - 60, y0 - 40);
    lv_obj_add_event_cb(b, pad_click, LV_EVENT_CLICKED, NULL);
    pump(50);
    pbtn(ds4, 0x131);
    kcheck("controle: ✕ toca", pad_clicks == 1);
    lv_obj_delete(b);

    /* analógico esquerdo para a direita e para baixo: o cursor anda */
    pev(ds4, EV_ABS, ABS_X, 255);
    pev(ds4, EV_ABS, ABS_Y, 255);
    pump(200);
    pev(ds4, EV_ABS, ABS_X, 128);
    pev(ds4, EV_ABS, ABS_Y, 128);
    gamepad_cursor(&x1, &y1);
    kcheck("controle: analógico move o cursor", x1 > x0 + 100 && y1 > y0 + 60);
    shot("23f-controle-cursor");

    /* ✕ segurado + analógico para cima: arrasta (rola a lista de Ajustes); antes volta o
     * cursor para o meio da lista (a corrida acima o deixou na borda de baixo) */
    pev(ds4, EV_ABS, ABS_Y, 0);
    pump(250);
    pev(ds4, EV_ABS, ABS_Y, 128);
    lv_obj_t *pg = lv_obj_get_child(app_content(), 0);
    int32_t s0 = pg ? lv_obj_get_scroll_y(pg) : 0;
    pev(ds4, EV_KEY, 0x131, 1);
    pev(ds4, EV_ABS, ABS_Y, 0);
    pump(350);
    pev(ds4, EV_ABS, ABS_Y, 128);
    pev(ds4, EV_KEY, 0x131, 0);
    pump(300);
    kcheck("controle: ✕ segurado arrasta", pg && lv_obj_get_scroll_y(pg) > s0 + 40);

    /* direcional para baixo (HAT): rola */
    go(SCR_SETTINGS, 0);
    pg = lv_obj_get_child(app_content(), 0);
    pev(ds4, EV_ABS, ABS_HAT0Y, 1);
    pev(ds4, EV_ABS, ABS_HAT0Y, 0);
    pump(400);
    kcheck("controle: direcional rola", pg && lv_obj_get_scroll_y(pg) > 0);

    /* ○ volta, PS vai para o início */
    nav_go(SCR_KEYBOARD, 0); pump(300);
    shot("23g-teclado-controle");
    pbtn(ds4, 0x132);
    pump(300);
    kcheck("controle: ○ volta", nav_current().scr == SCR_SETTINGS);
    pbtn(ds4, 0x13c);
    pump(300);
    kcheck("controle: PS vai para o início", nav_current().scr == SCR_HOME);

    /* direcional → / ←: as páginas de Apps */
    go(SCR_HOME, HOME_ARG_APPS); pump(200);
    lv_obj_t *tv = find_class(lv_screen_active(), &lv_tileview_class);
    pev(ds4, EV_ABS, ABS_HAT0X, 1);
    pev(ds4, EV_ABS, ABS_HAT0X, 0);
    pump(600);
    lv_obj_t *t1 = tv ? lv_tileview_get_tile_active(tv) : NULL;
    kcheck("controle: direcional → passa a página de Apps", t1 && lv_obj_get_x(t1) == lv_obj_get_width(tv));
    pev(ds4, EV_ABS, ABS_HAT0X, -1);
    pev(ds4, EV_ABS, ABS_HAT0X, 0);
    pump(600);
    t1 = tv ? lv_tileview_get_tile_active(tv) : NULL;
    kcheck("controle: direcional ← volta a página", t1 && lv_obj_get_x(t1) == 0);

    /* △ = Enter num diálogo */
    static const char *b2[] = { "Não", "Sim" };
    dlg_answer = -9;
    dialog_open(NULL, lv_color_white(), "Teste", NULL, b2, 2, 1, -1, dlg_cb, NULL); pump(50);
    pbtn(ds4, 0x133);
    kcheck("controle: △ confirma o diálogo", dlg_answer == 1 && !dialog_is_open());

    /* controle com o mapa padrão (hid-playstation, xpad...): BTN_EAST (0x131) volta */
    int std = gamepad_attach("Controle Padrão", &stdid, &sbits, ax, "");
    kcheck("controle padrão: mapa do Linux", gamepad_info(std, &gi) && gi.map_from == GPM_DEFAULT);
    go(SCR_SETTINGS, 0);
    nav_go(SCR_KEYBOARD, 0); pump(300);
    pbtn(std, BTN_EAST);
    pump(300);
    kcheck("controle padrão: B volta", nav_current().scr == SCR_SETTINGS && gamepad_count() == 2);
    gamepad_detach(std);
    gamepad_detach(ds4);
    pump(50);
    kcheck("controle: cursor some ao desconectar", !gamepad_cursor(NULL, NULL) && gamepad_count() == 0);
    printf("controle: %d falha(s) no total com o teclado\n", kb_fail);
}

/* ---------------------------------------------- apps instalados e SC Controller */
static void apps_tests(void)
{
    const char *pkg = getenv("BIGFOSTON_TEST_BFAPP");      /* pkg/apps/sc-controller.bfapp */
    if(!pkg || !*pkg) { printf("apps: sem BIGFOSTON_TEST_BFAPP, testes pulados\n"); return; }
    apps_uninstall_now("sc-controller");                    /* sobra de outra rodada */
    static gp_axis_t ax[GP_NABS];
    static pm_bits_t bits;
    for(unsigned c = ABS_X; c <= ABS_RZ; c++) { ax[c] = (gp_axis_t){ 0, 255, true }; pm_bits_set(&bits, false, c); }
    ax[ABS_HAT0X] = ax[ABS_HAT0Y] = (gp_axis_t){ -1, 1, true };
    pm_bits_set(&bits, false, ABS_HAT0X);
    pm_bits_set(&bits, false, ABS_HAT0Y);
    for(unsigned c = 0x130; c <= 0x13d; c++) pm_bits_set(&bits, true, c);
    const gp_id_t ds4id = { BUS_BLUETOOTH, 0x054c, 0x09cc, 0x0100 };
    int ds4 = gamepad_attach("Wireless Controller", &ds4id, &bits, ax, "");
    for(int a = ABS_X; a <= ABS_RZ; a++) pev(ds4, EV_ABS, (uint16_t)a, 128);
    gp_info_t gi;

    /* Jogos existe mesmo vazio */
    launcher_load();
    int games = ag_group_by_key("games") ? ag_group_by_key("games")->id : -1;
    go(SCR_APPGROUP, games); pump(300);
    shot("44a-jogos-vazio");
    kcheck("apps: Jogos vazio", apps_count() == 0 && nav_current().scr == SCR_APPGROUP);

    /* Arquivos -> .bfapp: "Instalar?"; Enter = Instalar */
    apps_install_ask(pkg); pump(200);
    shot("44b-instalar");
    dialog_key(true); pump(300);
    shot("44c-instalado");
    const bf_app_entry_t *a = apps_find("sc-controller");
    kcheck("apps: instalado em Jogos", a && apps_group_of(a) == BF_GRP_GAMES && !strcmp(a->name, "SC Controller") &&
           ag_parent("pkg:sc-controller") == games);
    kcheck("apps: ícone do pacote", a && apps_icon(a) != NULL);
    dialog_key(false); pump(100);
    kcheck("controle: mapa agora do banco do SC Controller", gamepad_info(ds4, &gi) && gi.map_from == GPM_DB);

    go(SCR_HOME, HOME_ARG_APPS); pump(200);
    scr_home_show_page(1); pump(300);
    shot("44d-apps-pasta-jogos");
    go(SCR_APPGROUP, games); pump(400);
    shot("44e-grupo-jogos");

    /* abre o app pelo .desktop (X-BigFoston-Screen) */
    apps_launch(a); pump(400);
    kcheck("apps: abre o SC Controller", nav_current().scr == SCR_SCC);
    shot("44f-scc-controles");

    /* testador: o controle não mexe na interface */
    scr_scc_demo_tab(1); pump(200);
    pev(ds4, EV_KEY, 0x131, 1);                      /* ✕ */
    pev(ds4, EV_KEY, 0x134, 1);                      /* L1 */
    pev(ds4, EV_ABS, ABS_X, 40);
    pev(ds4, EV_ABS, ABS_Y, 200);
    pev(ds4, EV_ABS, ABS_RY, 230);                   /* R2 analógico */
    pev(ds4, EV_ABS, ABS_HAT0X, 1);
    pump(200);
    shot("44g-scc-testar");
    pev(ds4, EV_KEY, 0x131, 0);
    pev(ds4, EV_KEY, 0x134, 0);
    pev(ds4, EV_ABS, ABS_X, 128);
    pev(ds4, EV_ABS, ABS_Y, 128);
    pev(ds4, EV_ABS, ABS_RY, 128);
    pev(ds4, EV_ABS, ABS_HAT0X, 0);
    pbtn(ds4, 0x132);                                 /* ○: no testador não volta */
    pump(200);
    kcheck("scc: no testador o controle não navega", nav_current().scr == SCR_SCC);
    pev(ds4, EV_KEY, 0x132, 1);                       /* ○ segurado 2 s: sai do teste */
    pump(2300);
    pev(ds4, EV_KEY, 0x132, 0);
    pump(100);
    pbtn(ds4, 0x132);                                 /* fora do teste, ○ volta */
    pump(300);
    kcheck("scc: segurar ○ sai do testador", nav_current().scr == SCR_APPGROUP);

    /* registro: as 21 etapas como o DS4 sem driver, com os gatilhos em repouso no 0 */
    apps_launch(apps_find("sc-controller")); pump(400);
    pev(ds4, EV_ABS, ABS_RX, 0);
    pev(ds4, EV_ABS, ABS_RY, 0);
    scr_scc_demo_tab(2); pump(200);
    shot("44h-scc-registrar");
    scr_scc_demo_tab(3); pump(100);
    shot("44i-scc-registrar-passo");
    static const struct { uint16_t t, c; int32_t on, off; } seq[] = {
        { EV_KEY, 0x131, 1, 0 }, { EV_KEY, 0x132, 1, 0 }, { EV_KEY, 0x130, 1, 0 }, { EV_KEY, 0x133, 1, 0 },
        { EV_KEY, 0x134, 1, 0 }, { EV_KEY, 0x135, 1, 0 }, { EV_ABS, ABS_RX, 255, 0 }, { EV_ABS, ABS_RY, 255, 0 },
        { EV_KEY, 0x138, 1, 0 }, { EV_KEY, 0x139, 1, 0 }, { EV_KEY, 0x13c, 1, 0 }, { EV_KEY, 0x13a, 1, 0 },
        { EV_KEY, 0x13b, 1, 0 }, { EV_ABS, ABS_HAT0Y, -1, 0 }, { EV_ABS, ABS_HAT0Y, 1, 0 },
        { EV_ABS, ABS_HAT0X, -1, 0 }, { EV_ABS, ABS_HAT0X, 1, 0 }, { EV_ABS, ABS_X, 255, 128 },
        { EV_ABS, ABS_Y, 255, 128 }, { EV_ABS, ABS_Z, 255, 128 }, { EV_ABS, ABS_RZ, 255, 128 },
    };
    for(size_t k = 0; k < sizeof seq / sizeof seq[0]; k++) {
        pev(ds4, seq[k].t, seq[k].c, seq[k].on);
        pev(ds4, seq[k].t, seq[k].c, seq[k].off);
        pump(30);
    }
    pump(200);
    shot("44j-scc-registrar-fim");
    scr_scc_demo_tab(4); pump(300);
    kcheck("scc: registro salvo e usado", gamepad_info(ds4, &gi) && gi.map_from == GPM_USER);
    /* o mapa ensinado é o mesmo da linha oficial do banco */
    char path[320], line[1024] = "";
    gamepad_user_db(path, sizeof path);
    FILE *f = fopen(path, "r");
    if(f) { if(!fgets(line, sizeof line, f)) line[0] = 0; fclose(f); }
    padmap_t mine, off;
    padmap_parse(line, &mine);
    padmap_parse("050000004c050000cc09000000010000,PS4 Controller,a:b1,b:b2,back:b8,dpdown:h0.4,dpleft:h0.8,dpright:h0.2,dpup:h0.1,guide:b12,leftshoulder:b4,leftstick:b10,lefttrigger:a3,leftx:a0,lefty:a1,rightshoulder:b5,rightstick:b11,righttrigger:a4,rightx:a2,righty:a5,start:b9,x:b0,y:b3,platform:Linux,", &off);
    bool same = true;
    for(int c = 0; c < PC__N; c++)
        if(memcmp(&mine.src[c], &off.src[c], sizeof mine.src[c])) {
            same = false;
            printf("controle: registro difere em %s\n", padmap_ctl_name(c));
        }
    kcheck("scc: registro igual ao banco do SDL", same);
    /* o registro vale na interface: ✕ toca */
    pev(ds4, EV_ABS, ABS_RX, 128);
    pev(ds4, EV_ABS, ABS_RY, 128);
    scr_scc_demo_tab(5); pump(300);
    kcheck("scc: esquecer volta ao banco", gamepad_info(ds4, &gi) && gi.map_from == GPM_DB);

    /* desinstalar: o grupo fica vazio e o controle volta ao mapa embutido */
    kcheck("apps: desinstala", apps_uninstall_now("sc-controller") && !apps_find("sc-controller") &&
           ag_parent("pkg:sc-controller") == -1);
    kcheck("controle: sem o app, mapa embutido", gamepad_info(ds4, &gi) && gi.map_from == GPM_BUILTIN);
    gamepad_detach(ds4);
    printf("apps: %d falha(s) no total\n", kb_fail);
}

/* ---------------------------------------------- grupos de apps (core/appgroups.c, launcher_ui.c)
 * Um toque virtual de verdade (indev de ponteiro do LVGL), como o do aparelho: o mesmo caminho
 * de PRESSED/PRESSING/RELEASED, rolagem e clique. */
static lv_indev_t *tp;
static lv_point_t tp_pt;
static bool tp_down;
static int g_fail, g_total;

static void gcheck(const char *what, bool ok)
{
    g_total++;
    printf("grupos: %s %s\n", ok ? "ok" : "FALHOU", what);
    if(!ok) g_fail++;
}

static void tp_read(lv_indev_t *i, lv_indev_data_t *d)
{
    (void)i;
    d->point = tp_pt;
    d->state = tp_down ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

static void touch(lv_point_t p, uint32_t ms)
{
    tp_pt = p;
    tp_down = true;
    pump(ms);
    tp_down = false;
    pump(150);
}

static lv_obj_t *find_label(lv_obj_t *o, const char *txt)
{
    if(lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN)) return NULL;
    if(lv_obj_check_type(o, &lv_label_class) && !strcmp(lv_label_get_text(o), txt)) return o;
    for(uint32_t i = 0; i < lv_obj_get_child_count(o); i++) {
        lv_obj_t *r = find_label(lv_obj_get_child(o, (int32_t)i), txt);
        if(r) return r;
    }
    return NULL;
}

/* o centro do ícone (o quadrado acima do nome) do tile com esse nome, se estiver à vista */
static bool tile_pt(const char *name, lv_point_t *pt)
{
    lv_obj_t *l = find_label(lv_screen_active(), name);
    if(!l) return false;
    lv_area_t a;
    lv_obj_get_coords(lv_obj_get_parent(l), &a);
    pt->x = (a.x1 + a.x2) / 2;
    pt->y = (a.y1 + a.y2) / 2 - 16;
    return pt->x > 0 && pt->x < BF_SCREEN_W && pt->y > 0 && pt->y < BF_SCREEN_H;
}

static int gid_named(const char *n)
{
    for(int i = 0; i < ag_group_count(); i++)
        if(!strcmp(ag_group_name(ag_group_at(i)), n)) return ag_group_at(i)->id;
    return 0;
}

/* clica no botão com esse texto na camada de cima (diálogos) */
static bool click_top(const char *txt)
{
    lv_obj_t *l = find_label(lv_layer_top(), txt);
    if(!l) return false;
    lv_obj_send_event(lv_obj_get_parent(l), LV_EVENT_CLICKED, NULL);
    pump(150);
    return true;
}

static void group_tests(void)
{
    bf_settings_t *s = settings();
    s->home_mode = BF_HOME_GRID;
    app_rebuild_now(); pump(50);
    nav_home(); pump(500);
    scr_home_show_page(0); pump(100);
    if(!tp) {
        tp = lv_indev_create();
        lv_indev_set_type(tp, LV_INDEV_TYPE_POINTER);
        lv_indev_set_read_cb(tp, tp_read);
        lv_indev_set_display(tp, disp);
        lv_indev_set_scroll_limit(tp, 16);         /* como o toque calibrado do aparelho */
    }
    lv_point_t pt;
    const char *tvn = tr(S_APP_TV);

    /* toque rápido / soltar cedo / segurar 2 s */
    gcheck("TV visível na grade", tile_pt(tvn, &pt));
    touch(pt, 120); pump(300);
    gcheck("toque rápido abre a TV", nav_current().scr == SCR_TV);
    nav_home(); pump(500);
    tile_pt(tvn, &pt);
    touch(pt, 1000); pump(300);
    gcheck("soltar com 1 s: nem abre nem menu", nav_current().scr == SCR_HOME && !ctxmenu_is_open());
    tp_pt = pt; tp_down = true; pump(1100);
    shot("50a-grupos-segurando");
    gcheck("anel de progresso durante o toque", find_class(lv_screen_active(), &lv_arc_class) != NULL);
    gcheck("1,1 s: ainda sem menu", !ctxmenu_is_open());
    pump(1100);
    gcheck("2 s: menu aberto", ctxmenu_is_open());
    tp_down = false; pump(300);
    gcheck("soltar depois do menu não abre o app", nav_current().scr == SCR_HOME && ctxmenu_is_open());
    gcheck("anel some", find_class(lv_screen_active(), &lv_arc_class) == NULL);
    shot("50b-grupos-menu-app");
    kfeed(KEY_ESC, 0); pump(150);
    gcheck("Esc fecha o menu (sem voltar de tela)", !ctxmenu_is_open() && nav_current().scr == SCR_HOME);
    tile_pt(tvn, &pt);
    touch(pt, 2200); pump(100);
    touch((lv_point_t){ 12, 470 }, 100); pump(200);
    gcheck("toque fora fecha o menu", !ctxmenu_is_open() && nav_current().scr == SCR_HOME);

    /* arrastar (rolagem) não abre menu nem app */
    tile_pt(tvn, &pt);
    tp_pt = pt; tp_down = true; pump(80);
    for(int i = 0; i < 10; i++) { tp_pt.x -= 12; pump(35); }
    pump(2300);
    tp_down = false; pump(500);
    gcheck("arrastar: sem menu e sem abrir", !ctxmenu_is_open() && nav_current().scr == SCR_HOME);
    scr_home_show_page(0); pump(200);

    /* criar grupo: o app vai junto */
    tile_pt(tvn, &pt);
    touch(pt, 2200);
    lui_demo_pick(M_NEW); pump(250);
    gcheck("Novo grupo: janela aberta", namebox_is_open());
    ktype("Entretenimento"); pump(100);
    shot("50c-grupos-novo-grupo");
    gcheck("nome vazio recusado", namebox_demo_submit("   ") != NULL && namebox_is_open());
    pump(100);
    shot("50d-grupos-nome-vazio");
    gcheck("nome repetido (Jogos) recusado", namebox_demo_submit("jogos") != NULL && namebox_is_open());
    gcheck("o campo para em 24 letras", !strcmp(namebox_demo_set("Aplicativos do dia a dia!"), "Aplicativos do dia a dia"));
    gcheck("Entretenimento criado", namebox_demo_submit("Entretenimento") == NULL && !namebox_is_open());
    pump(500);
    int ent = gid_named("Entretenimento");
    gcheck("TV dentro de Entretenimento", ent > 0 && ag_parent("sys:tv") == ent);
    gcheck("pasta na grade, TV fora da grade", find_label(lv_screen_active(), "Entretenimento") &&
           !find_label(lv_screen_active(), tvn));
    shot("50e-grupos-pasta-criada");

    /* enviar para o grupo (lista) */
    lui_item_menu(NULL, "sys:music", 0); pump(100);
    lui_demo_pick(M_SEND); pump(200);
    shot("50f-grupos-enviar");
    lui_demo_pick(ent); pump(500);
    gcheck("Multimídia enviada para Entretenimento", ag_parent("sys:music") == ent);
    lui_item_menu(NULL, "sys:music", ent); lui_demo_pick(M_SEND); lui_demo_pick(ent); pump(200);
    gcheck("enviar de novo: sem duplicar", ag_parent("sys:music") == ent && ag_children(ent, NULL, 0) == 2);
    lui_item_menu(NULL, "sys:book", 0); lui_demo_pick(M_NEW); pump(100);
    gcheck("Educação (acentos) criado com o Livro", namebox_demo_submit("Educação") == NULL);
    pump(300);
    int edu = gid_named("Educação");
    gcheck("Livro em Educação", edu > 0 && ag_parent("sys:book") == edu);
    lui_item_menu(NULL, "sys:music", ent); lui_demo_pick(M_SEND); lui_demo_pick(edu); pump(300);
    gcheck("Multimídia de um grupo para outro", ag_parent("sys:music") == edu && ag_children(ent, NULL, 0) == 1);

    /* dentro do grupo: tocar na pasta abre; segurar o app; tirar do grupo */
    nav_home(); pump(500);
    gcheck("pasta à vista", tile_pt("Entretenimento", &pt));
    touch(pt, 120); pump(500);
    gcheck("tocar na pasta abre o grupo", nav_current().scr == SCR_APPGROUP && nav_current().arg == ent);
    shot("50g-grupo-aberto");
    gcheck("TV no grupo", tile_pt(tvn, &pt));
    touch(pt, 2200); pump(100);
    gcheck("segurar no grupo abre o menu", ctxmenu_is_open());
    shot("50h-grupo-menu-app");
    lui_demo_pick(M_REMOVE); pump(500);
    gcheck("TV fora do grupo; grupo vazio continua", ag_parent("sys:tv") == 0 && ag_group_get(ent) &&
           nav_current().scr == SCR_APPGROUP);
    shot("50i-grupo-vazio");
    lui_group_menu(NULL, ent); pump(100);
    shot("50j-grupo-opcoes");
    lui_demo_pick(G_RENAME); pump(100);
    gcheck("renomear para o nome de outro: recusado", namebox_demo_submit("educação") != NULL);
    gcheck("renomeado", namebox_demo_submit("Diversão") == NULL && gid_named("Diversão") == ent);
    pump(400);
    gcheck("título novo", !strcmp(nav_title(SCR_APPGROUP, ent), "Diversão"));
    lui_group_menu(NULL, ent); lui_demo_pick(G_STYLE); pump(200);
    shot("50k-grupo-cor-icone");
    click_top(tr(S_LG_SAVE));

    /* mover: tocar no novo lugar; tocar numa pasta põe dentro */
    nav_home(); pump(500);
    lui_item_menu(NULL, "sys:settings", 0); lui_demo_pick(M_MOVE); pump(400);
    shot("50l-grupos-mover");
    gcheck("faixa do modo mover", lui_moving() && find_label(lv_screen_active(), tr(S_LG_MOVE_END)));
    gcheck("Bluetooth à vista", tile_pt(tr(S_APP_BLUETOOTH), &pt));
    touch(pt, 120); pump(500);
    {
        const char *v[64];
        int n = ag_children(0, v, 64), si = -1, bi = -1;
        for(int i = 0; i < n && i < 64; i++) {
            if(!strcmp(v[i], "sys:settings")) si = i;
            if(!strcmp(v[i], "sys:bluetooth")) bi = i;
        }
        gcheck("Ajustes logo antes do Bluetooth", si >= 0 && si + 1 == bi && !lui_moving());
    }
    lui_item_menu(NULL, "sys:gps", 0); lui_demo_pick(M_MOVE); pump(400);
    scr_home_show_page(0); pump(200);
    gcheck("pasta Educação à vista", tile_pt("Educação", &pt));
    touch(pt, 120); pump(500);
    gcheck("tocar na pasta no modo mover: GPS entra", ag_parent("sys:gps") == edu && nav_current().scr == SCR_HOME);

    /* todos os temas: mesma organização */
    s->home_mode = BF_HOME_GLASS;
    app_rebuild_now(); pump(50);
    go(SCR_HOME, HOME_ARG_APPS); pump(500);
    gcheck("Foston Glass > Apps: pastas", find_label(lv_screen_active(), "Educação") &&
           find_label(lv_screen_active(), "Diversão"));
    shot("50m-glass-apps-grupos");
    tile_pt("Educação", &pt);
    touch(pt, 2200); pump(100);
    gcheck("Glass: segurar a pasta abre o menu do grupo", ctxmenu_is_open());
    shot("50n-glass-menu-grupo");
    ctxmenu_close(); pump(100);
    s->home_mode = BF_HOME_PANEL;
    app_rebuild_now(); pump(50);
    nav_home(); pump(500);
    shot("50o-painel-com-apps");
    gcheck("Painel: ícone Apps", tile_pt(tr(S_GL_APPS), &pt));
    touch(pt, 120); pump(500);
    gcheck("Painel: Apps abre a grade", nav_current().scr == SCR_HOME && nav_current().arg == HOME_ARG_APPS &&
           find_label(lv_screen_active(), "Educação"));
    shot("50p-painel-apps-grade");
    s->home_mode = BF_HOME_KIOSK;
    app_rebuild_now(); pump(50);
    nav_home(); pump(500);
    if(tile_pt(tr(S_APP_FILES), &pt)) {
        touch(pt, 2200); pump(100);
        gcheck("Quiosque: sem menu de grupos", !ctxmenu_is_open());
        nav_home(); pump(300);
    }
    shot("50v-quiosque-com-apps");
    gcheck("Quiosque: ícone Apps no fim", tile_pt(tr(S_GL_APPS), &pt));
    touch(pt, 120); pump(500);
    gcheck("Quiosque: Apps abre a grade com todos", nav_current().scr == SCR_HOME &&
           nav_current().arg == HOME_ARG_APPS && find_label(lv_screen_active(), "Educação") &&
           find_label(lv_screen_active(), tr(S_APP_TV)));
    gcheck("Quiosque: Ajustes fora da grade", !find_label(app_content(), tr(S_APP_SETTINGS)));   /* a barra tem "Ajustes" */
    shot("50w-quiosque-apps-grade");
    tile_pt(tr(S_APP_TV), &pt);
    touch(pt, 2200); pump(100);
    /* no quiosque segurar é como tocar (igual aos ícones da tela do quiosque): abre, sem menu */
    gcheck("Quiosque: segurar não abre o menu de organizar", !ctxmenu_is_open() && nav_current().scr == SCR_TV);
    go(SCR_HOME, HOME_ARG_APPS); pump(400);
    tile_pt("Educação", &pt);
    touch(pt, 120); pump(500);
    gcheck("Quiosque: pasta abre, sem Opções", nav_current().scr == SCR_APPGROUP &&
           !find_label(lv_screen_active(), tr(S_LG_OPTIONS)));
    go(SCR_HOME, HOME_ARG_APPS); pump(300);
    lui_search(); pump(100);
    gcheck("Quiosque: a busca não acha o Ajustes", namebox_demo_submit(tr(S_APP_SETTINGS)) != NULL);
    namebox_close(); pump(100);
    nav_home(); pump(300);
    s->home_mode = BF_HOME_GRID;
    app_rebuild_now(); pump(50);
    nav_home(); pump(400);
    gcheck("Grade: mesmas pastas", find_label(lv_screen_active(), "Diversão") != NULL);

    /* persistência: relê o arquivo como numa reinicialização */
    launcher_reopen();
    launcher_sync();
    gcheck("depois de reler: grupos e apps no lugar", gid_named("Educação") == edu && ag_parent("sys:book") == edu &&
           ag_parent("sys:music") == edu && ag_parent("sys:gps") == edu && gid_named("Diversão") == ent);

    /* busca: acha dentro dos grupos, sem acento nem maiúsculas */
    go(SCR_HOME, HOME_ARG_APPS); pump(400);
    lui_search(); pump(100);
    gcheck("busca sem resultado: avisa e continua aberta", namebox_demo_submit("zzz") != NULL && namebox_is_open());
    gcheck("busca \"MULTIMIDIA\" acha Multimídia (no grupo)", namebox_demo_submit("MULTIMIDIA") == NULL && ctxmenu_is_open());
    pump(150);
    shot("50t-grupos-busca");
    lui_demo_pick(0); pump(400);
    gcheck("resultado abre o app", nav_current().scr == SCR_MUSIC);
    nav_home(); pump(400);

    /* excluir: os apps voltam */
    lui_group_menu(NULL, edu); lui_demo_pick(G_DELETE); pump(200);
    shot("50q-grupos-excluir");
    gcheck("confirmar exclusão", click_top(tr(S_LG_DEL_BTN)));
    pump(500);
    gcheck("grupo excluído, apps de volta (nenhum perdido)", !ag_group_get(edu) && ag_parent("sys:book") == 0 &&
           ag_parent("sys:music") == 0 && ag_parent("sys:gps") == 0);
    shot("50r-grupos-excluido");

    /* teclado físico na janela de nome: Esc cancela, Enter confirma */
    lui_item_menu(NULL, "sys:gallery", 0); lui_demo_pick(M_NEW); pump(100);
    kfeed(KEY_ESC, 0); pump(150);
    gcheck("Esc cancela a janela de nome", !namebox_is_open() && ag_parent("sys:gallery") == 0);
    lui_item_menu(NULL, "sys:gallery", 0); lui_demo_pick(M_NEW); pump(100);
    ktype("Fotos"); kfeed(KEY_ENTER, 0); pump(300);
    gcheck("Enter do teclado físico cria o grupo", !namebox_is_open() && gid_named("Fotos") > 0 &&
           ag_parent("sys:gallery") == gid_named("Fotos"));
    ag_group_delete(gid_named("Fotos")); pump(200);

    /* tema claro e nome comprido */
    lui_item_menu(NULL, "sys:files", 0); lui_demo_pick(M_NEW);
    gcheck("nome de 24 letras", namebox_demo_submit("Ferramentas do dia a dia") == NULL);
    s->mode = BF_MODE_LIGHT;
    app_rebuild_now(); pump(50);
    nav_home(); pump(500);
    if(!tile_pt("Ferramentas do dia a dia", &pt)) tile_pt(tr(S_APP_BLUETOOTH), &pt);
    touch(pt, 2200); pump(100);
    shot("50s-grupos-claro-menu");
    ctxmenu_close(); pump(100);
    s->mode = BF_MODE_DARK;
    app_rebuild_now(); pump(50);
    nav_home(); pump(300);

    /* muitos grupos: a página à vista na hora, as outras em segundo plano */
    {
        int made[40], nm = 0;
        char nm_[32];
        for(int i = 0; i < 40; i++) {
            snprintf(nm_, sizeof nm_, "Carga %02d", i);
            if(ag_group_create(nm_, NULL, &made[nm]) == AG_OK) nm++;
        }
        gcheck("40 grupos criados", nm == 40);
        go(SCR_HOME, HOME_ARG_APPS); pump(30);
        uint32_t t0 = now_ms();
        nav_show_now();
        uint32_t dt = now_ms() - t0;
        lv_obj_t *tv = find_class(lv_screen_active(), &lv_tileview_class);
        int pages = tv ? (int)lv_obj_get_child_count(tv) : 0;
        lv_obj_t *last = pages ? lv_obj_get_child(lv_obj_get_child(tv, pages - 1), 0) : NULL;
        gcheck("várias páginas; a última ainda vazia logo depois de abrir", pages >= 6 && last &&
               lv_obj_get_child_count(last) == 0);
        printf("grupos: abrir Apps com %d páginas: %u ms no PC (só a página à vista)\n", pages, (unsigned)dt);
        scr_home_show_page(pages - 1); pump(50);
        gcheck("pular para a última: preenchida na hora", last && lv_obj_get_child_count(last) > 0);
        shot("50u-grupos-muitos");
        scr_home_show_page(0);
        pump(1500);
        int empty = 0;
        for(int i = 0; i < pages; i++) if(!lv_obj_get_child_count(lv_obj_get_child(lv_obj_get_child(tv, i), 0))) empty++;
        gcheck("em 1,5 s todas as páginas preenchidas", empty == 0);
        for(int i = 0; i < nm; i++) ag_group_delete(made[i]);
        pump(300);
    }

    lv_indev_delete(tp);
    tp = NULL;
    printf("grupos: %d/%d ok\n", g_total - g_fail, g_total);
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
    if(getenv("BIGFOSTON_TEST_GROUPS_ONLY")) {      /* só os testes de grupos (rápido) */
        group_tests();
        return g_fail ? 1 : 0;
    }

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
    go(SCR_BRP, 0);
    scr_brp_demo("HOST 192.168.7.1|0|0|0|7.1.431.-1"); pump(100); shot("40-brp-inicio");
    scr_brp_demo("PIN 4821");       pump(100); shot("40b-brp-pin");
    scr_brp_demo("PAIR ok");
    scr_brp_demo("HOST 192.168.7.1|1|0|0|7.1.431.-1");
    scr_brp_demo("APP 1|01 - Monitor"); scr_brp_demo("APP 2|Desktop"); scr_brp_demo("APP 3|Desktop Low");
    scr_brp_demo("APP 4|Steam Big Picture"); scr_brp_demo("APPS fim"); pump(100); shot("40c-brp-apps");
    scr_brp_demo("+video");         pump(100); shot("40f-brp-imagem");
    scr_brp_demo("+full");          pump(400);
    scr_brp_demo("+bar");           pump(400); shot("40g-brp-tela-cheia-barra");
    scr_brp_demo("+window");        pump(100);
    go(SCR_POWER, 0);               shot("17-potencia");
    scroll_content_end();           shot("17b-potencia-plano");
    go(SCR_PERF, 0);                shot("17c-desempenho");
    scroll_content_end();           shot("17d-desempenho-fim");
    go(SCR_PERFMODE, 0);            shot("17e-modo-performance");
    scroll_content_end();           shot("17f-modo-performance-fim");
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
    go(SCR_GALLERY, 0); scr_gallery_demo(0); scr_gallery_demo(1); scr_gallery_demo(3);
    pump(300); shot("44i-glass-galeria-foto");                      /* área de 432 px */
    scr_gallery_demo(2);
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
    keyboard_tests();
    gamepad_tests();
    apps_tests();
    group_tests();
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
                scr_map_demo(6); pump(300); shot("42g-biggps-marcador");
                scr_map_demo(7); pump(300); shot("42h-biggps-marcador-sedan");
                scr_map_demo(8); pump(1200); shot("42i-biggps-sedan-no-mapa");
                scr_map_demo(11); pump(300); shot("42l-biggps-destino-aviso");
                scr_map_demo(12); pump(1500); shot("42m-biggps-aviso-seu");
                scr_map_demo(3); pump(300); shot("42e-biggps-busca");
                scr_map_demo(4); pump(300); shot("42f-biggps-busca-hospital");
                scr_map_demo(5); pump(100);
                scr_map_demo(0); pump(4000); shot("42b-biggps-rota");
                scr_map_demo(9); pump(2500); shot("42j-biggps-rota-alternativa");
                scr_map_demo(1); pump(2500); shot("42c-biggps-navegacao");
                scr_map_demo(10); pump(300); shot("42k-biggps-faixas");
                scr_map_demo(2); pump(2500); shot("42d-biggps-navegacao-adiante");
                scr_map_demo(13); pump(200); shot("42n-biggps-radar-placa");
                scr_map_demo(14); pump(200); shot("42o-biggps-radar-acima");
                scr_map_demo(15);
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
        o = fopen(sp, "w");                             /* o PC decodificando (docs/tv/08-relay-pc.md) */
        if(o) { fputs("state=tocando\nfreq=527.143\nlocked=1\nsnr=19\nrssi=-62\nfps=30.0\nrelay=1\n", o); fclose(o); }
        pump(1200); shot("46i-tv-pc");
        o = fopen(sp, "w");
        if(o) { fputs("state=tocando\nfreq=479.143\nlocked=1\nsnr=19\nrssi=-62\nfps=13.9\n", o); fclose(o); }
        pump(1200);
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
    scr_gallery_demo(3); pump(50);  shot("37c-galeria-foto-controles");
    scr_gallery_demo(4); pump(50);  shot("37d-galeria-foto-girada");
    scr_gallery_demo(5); pump(50);  shot("37e-galeria-foto-zoom");
    scr_gallery_demo(6); pump(100); shot("37f-galeria-voltar");     /* Voltar fecha a foto, fica na Galeria */
    scr_gallery_demo(2); pump(50);
    {   /* Internet: motor do PC (firmware/build/web/host/dist, tools/bigfoston/build-web.sh host) */
        const char *wd = getenv("BIGFOSTON_WEB_DIR");
        static const char host_dist[] = "../../firmware/build/web/host/dist";
        char b[600];
        snprintf(b, sizeof b, "%s/bfweb", host_dist);
        if(!wd && access(b, X_OK) == 0) setenv("BIGFOSTON_WEB_DIR", host_dist, 1);
        char page[600], url[1100];
        snprintf(page, sizeof page, "%s/web-demo.html", data_dir());
        FILE *f = fopen(page, "w");
        if(f) {
            fputs("<!doctype html><html lang=\"pt-BR\"><head><meta charset=\"utf-8\"><title>Notícias do BigFoston</title>"
                  "<style>body{font-family:sans-serif;margin:0;background:#f4f6fb;color:#1a1a2e}"
                  "header{background:#3584e4;color:#fff;padding:14px 20px;font-size:24px;font-weight:bold}"
                  "main{padding:8px 20px} .c{background:#fff;border-radius:10px;padding:12px 16px;margin:10px 0;border:1px solid #d5dbe8}"
                  "h2{margin:0 0 6px;font-size:19px;color:#1d4f9a} a{color:#3584e4}</style></head><body>"
                  "<header>BigFoston Web</header><main>"
                  "<div class=\"c\"><h2>Navegação leve no FOSTON</h2>Páginas simples, formulários, favoritos, histórico e downloads"
                  " direto para o cartão. Motor NetSurf 3.11 num processo separado, sem privilégios.</div>"
                  "<div class=\"c\"><h2>Pesquisa</h2><form action=\"https://html.duckduckgo.com/html/\"><input name=\"q\" value=\"previsão do tempo\" size=\"30\">"
                  " <button>Pesquisar</button></form></div>"
                  "<div class=\"c\"><h2>Links</h2><a href=\"https://pt.m.wikipedia.org\">Wikipédia</a> · "
                  "<a href=\"https://github.com/ruscher/foston-monitor\">foston-monitor no GitHub</a></div>", f);
            for(int i = 1; i <= 12; i++) fprintf(f, "<div class=\"c\"><h2>Seção %d</h2>Texto para rolar a página.</div>", i);
            fputs("</main></body></html>", f);
            fclose(f);
        }
        snprintf(url, sizeof url, "file://%s", page);
        if(page[0] != '/') {                                  /* caminho relativo: absoluto para o file:// */
            char cwd[400];
            if(getcwd(cwd, sizeof cwd)) snprintf(url, sizeof url, "file://%s/%s", cwd, page);
        }
        go(SCR_WEB, 0); pump(1500);                 shot("47-internet-inicio");
        scr_web_demo("+mark Wikipédia\thttps://pt.m.wikipedia.org");
        scr_web_demo("+mark GitHub\thttps://github.com");
        scr_web_demo("+mark DuckDuckGo\thttps://html.duckduckgo.com/html/");
        scr_web_demo("+start"); pump(100);          shot("47a-internet-inicio-favoritos");
        scr_web_demo(url); pump(4000);              shot("47b-internet-pagina");
        scr_web_demo("+scroll 300"); pump(800);     shot("47c-internet-rolagem");
        scr_web_demo("+menu"); pump(200);           shot("47d-internet-menu");
        scr_web_demo("+close");
        scr_web_demo("+edit"); scr_web_demo("+type wiki"); pump(200); shot("47e-internet-endereco");
        scr_web_demo("+close");
        if(getenv("BIGFOSTON_SHOT_PC")) {            /* modo completo: tools/bigfoston/bfweb-pc.py --host 127.0.0.1 */
            for(int k = 0; k < 300 && !webremote_active(); k++) pump(100);
            shot("48-internet-pc-ligado");
            scr_web_demo("https://www.biglinux.com.br/pt-br/"); pump(9000); shot("48a-internet-pc-biglinux");
            scr_web_demo("+scroll 600"); pump(2500);                    shot("48b-internet-pc-rolagem");
            scr_web_demo("https://g1.globo.com"); pump(12000);          shot("48c-internet-pc-g1");
            scr_web_demo("https://www.gov.br"); pump(10000);            shot("48d-internet-pc-govbr");
        }
        scr_web_demo("+app BigLinux\thttps://www.biglinux.com.br");
        scr_web_demo("+start"); pump(100);          shot("47g-internet-webapps");
        go(SCR_HOME, HOME_ARG_APPS); pump(600);
        scr_home_show_page(1); pump(400);           shot("47f-apps-com-internet");
        scr_home_show_page(0);
        go(SCR_HOME, 0); pump(300);
    }
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
        snprintf(f, sizeof f, "%s/pictures/estrela.svg", data_dir());
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
        snprintf(f, sizeof f, "%s/pictures/campo.jpg", data_dir());
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
    return n_ok > 0 && !g_fail ? 0 : 1;
}
#endif /* BIGFOSTON_SCREENSHOT */
