/* test_padmap.c — mapa de controles no formato do SDL GameControllerDB (core/padmap.c):
 * GUID, leitura e escrita da linha, busca no banco e a numeração de botões/eixos/direcional do
 * SDL no Linux, com o DualShock 4 pelo Bluetooth sem driver (hid-generic) e um controle padrão. */
#include "core/padmap.h"
#include <linux/input.h>
#include <stdio.h>
#include <string.h>

static int fails, total;

static void ok(const char *what, int cond)
{
    total++;
    if(!cond) { fails++; printf("FALHOU %s\n", what); }
}

static const char ds4_line[] =
    "050000004c050000cc09000000010000,PS4 Controller,a:b1,b:b2,back:b8,dpdown:h0.4,dpleft:h0.8,"
    "dpright:h0.2,dpup:h0.1,guide:b12,leftshoulder:b4,leftstick:b10,lefttrigger:a3,leftx:a0,lefty:a1,"
    "rightshoulder:b5,rightstick:b11,righttrigger:a4,rightx:a2,righty:a5,start:b9,touchpad:b13,x:b0,y:b3,"
    "platform:Linux,";

int main(int argc, char **argv)
{
    char g[33];
    padmap_guid(5, 0x054c, 0x09cc, 0x0100, g);
    ok("GUID do DS4 pelo Bluetooth", !strcmp(g, "050000004c050000cc09000000010000"));
    padmap_guid(3, 0x045e, 0x028e, 0x0114, g);
    ok("GUID do controle do Xbox 360 por USB", !strcmp(g, "030000005e0400008e02000014010000"));

    padmap_t m;
    ok("lê a linha do DS4", padmap_parse(ds4_line, &m));
    ok("nome", !strcmp(m.name, "PS4 Controller"));
    ok("a = b1", m.src[PC_A].kind == PS_BUTTON && m.src[PC_A].idx == 1);
    ok("dpdown = h0.4", m.src[PC_DPDOWN].kind == PS_HAT && m.src[PC_DPDOWN].mask == 4);
    ok("lefttrigger = a3", m.src[PC_LTRIGGER].kind == PS_AXIS && m.src[PC_LTRIGGER].idx == 3);
    ok("linha de outro sistema é recusada",
       !padmap_parse("030000005e0400008e02000000000000,X,a:b0,platform:Windows,", &m));
    ok("linha sem GUID é recusada", !padmap_parse("# comentário", &m));
    ok("meio eixo e invertido",
       padmap_parse("0300000000000000000000000000000a,T,+leftx:b3,-leftx:b2,lefty:a1~,righttrigger:+a5,platform:Linux,", &m)
       && m.half[PC_LEFTX - PC__NBTN][0].kind == PS_BUTTON && m.half[PC_LEFTX - PC__NBTN][0].idx == 3
       && m.half[PC_LEFTX - PC__NBTN][1].idx == 2 && m.src[PC_LEFTY].invert && m.src[PC_RTRIGGER].half == 1);

    /* ida e volta */
    char out[1024];
    padmap_t a, b;
    padmap_parse(ds4_line, &a);
    ok("escreve a linha", padmap_format(&a, out, sizeof out) > 0);
    ok("relê igual", padmap_parse(out, &b) && !memcmp(&a, &b, sizeof a));
    ok("escrita curta demais avisa", padmap_format(&a, out, 40) == -1);

    /* numeração do SDL no DS4 sem driver: botões 0x130..0x13d, eixos X Y Z RX RY RZ, HAT0 */
    pm_bits_t ds4;
    memset(&ds4, 0, sizeof ds4);
    for(unsigned c = 0x130; c <= 0x13d; c++) pm_bits_set(&ds4, true, c);
    for(unsigned c = ABS_X; c <= ABS_RZ; c++) pm_bits_set(&ds4, false, c);
    pm_bits_set(&ds4, false, ABS_HAT0X);
    pm_bits_set(&ds4, false, ABS_HAT0Y);
    ok("b1 = 0x131 (Xis)", pm_button_code(&ds4, 1) == 0x131);
    ok("b12 = 0x13c (PS)", pm_button_code(&ds4, 12) == 0x13c);
    ok("b14 não existe", pm_button_code(&ds4, 14) == -1);
    ok("0x13c = b12", pm_button_index(&ds4, 0x13c) == 12);
    ok("a3 = ABS_RX", pm_axis_code(&ds4, 3) == ABS_RX);
    ok("ABS_RZ = a5", pm_axis_index(&ds4, ABS_RZ) == 5);
    ok("o direcional não é eixo", pm_axis_index(&ds4, ABS_HAT0X) == -1);
    ok("h0 = ABS_HAT0X", pm_hat_code(&ds4, 0) == ABS_HAT0X && pm_hat_index(&ds4, ABS_HAT0Y) == 0);

    /* teclado com botões de joystick: BTN_JOYSTICK vem antes das teclas comuns */
    pm_bits_t mix;
    memset(&mix, 0, sizeof mix);
    pm_bits_set(&mix, true, KEY_A);
    pm_bits_set(&mix, true, BTN_TRIGGER);
    ok("BTN_TRIGGER = b0, KEY_A = b1", pm_button_index(&mix, BTN_TRIGGER) == 0 && pm_button_index(&mix, KEY_A) == 1);

    /* mapa padrão do Linux (hid-playstation, xpad) */
    pm_bits_t std;
    memset(&std, 0, sizeof std);
    unsigned keys[] = { BTN_SOUTH, BTN_EAST, BTN_NORTH, BTN_WEST, BTN_TL, BTN_TR, BTN_TL2, BTN_TR2,
                        BTN_SELECT, BTN_START, BTN_MODE, BTN_THUMBL, BTN_THUMBR };
    for(size_t i = 0; i < sizeof keys / sizeof keys[0]; i++) pm_bits_set(&std, true, keys[i]);
    for(unsigned c = ABS_X; c <= ABS_RZ; c++) pm_bits_set(&std, false, c);
    pm_bits_set(&std, false, ABS_HAT0X);
    pm_bits_set(&std, false, ABS_HAT0Y);
    padmap_t d;
    memset(&d, 0, sizeof d);
    padmap_linux_default(&std, &d);
    ok("padrão: A = BTN_SOUTH", pm_button_code(&std, d.src[PC_A].idx) == BTN_SOUTH);
    ok("padrão: Y = BTN_NORTH", pm_button_code(&std, d.src[PC_Y].idx) == BTN_NORTH);
    ok("padrão: analógico direito em RX/RY", pm_axis_code(&std, d.src[PC_RIGHTX].idx) == ABS_RX);
    ok("padrão: gatilho esquerdo em Z", pm_axis_code(&std, d.src[PC_LTRIGGER].idx) == ABS_Z);
    ok("padrão: direcional no HAT", d.src[PC_DPLEFT].kind == PS_HAT && d.src[PC_DPLEFT].mask == 8);

    /* busca no banco: exato, versão zerada, primeiro arquivo vence */
    const char *dir = argc > 1 ? argv[1] : "/tmp";
    char f1[300], f2[300];
    snprintf(f1, sizeof f1, "%s/user.txt", dir);
    snprintf(f2, sizeof f2, "%s/db.txt", dir);
    FILE *f = fopen(f2, "w");
    if(!f) { printf("sem pasta de teste %s\n", dir); return 1; }
    fprintf(f, "# banco\n%s\n", ds4_line);
    fprintf(f, "030000005e0400008e02000000000000,Xbox 360 Controller,a:b0,b:b1,platform:Linux,\n");
    fprintf(f, "030000005e0400008e02000014010000,Xbox 360 Controller,a:b0,b:b1,platform:Windows,\n");
    fclose(f);
    f = fopen(f1, "w");
    fprintf(f, "050000004c050000cc09000000010000,Meu DS4,a:b0,platform:Linux,\n");
    fclose(f);
    const char *both[] = { f1, f2 }, *only[] = { f2 };
    padmap_t r;
    padmap_guid(5, 0x054c, 0x09cc, 0x0100, g);
    ok("banco: acha o DS4", padmap_find(only, 1, g, &r) && r.src[PC_A].idx == 1);
    ok("banco: o do usuário vence", padmap_find(both, 2, g, &r) && !strcmp(r.name, "Meu DS4"));
    padmap_guid(5, 0x054c, 0x09cc, 0x8100, g);
    ok("banco: outra versão do driver não serve", !padmap_find(only, 1, g, &r));
    padmap_guid(3, 0x045e, 0x028e, 0x0114, g);
    ok("banco: versão zerada serve para qualquer versão", padmap_find(only, 1, g, &r) && !strcmp(r.guid, g));
    padmap_guid(3, 0x1234, 0x5678, 0x0001, g);
    ok("banco: desconhecido", !padmap_find(both, 2, g, &r));
    remove(f1);
    remove(f2);

    printf("padmap: %d/%d ok\n", total - fails, total);
    return fails ? 1 : 0;
}
