/* preview.c — a tela Dual Boot (ui.c) desenhada no PC, sem o aparelho: compila com o
 * winegcc e grava cada estado em RGB565 cru (800x480), que tools/preview.py vira PNG.
 *
 *   make preview          (usa a Tahoma da ROM do FOSTON se estiver no prefixo do wine)
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "ui.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PW 800
#define PH 480

static ui_line_t lines[UI_LINES];
static int nlines;

static void add(int kind, const WCHAR *t)
{
    if (nlines == UI_LINES) {
        memmove(lines, lines + 1, sizeof(lines[0]) * (UI_LINES - 1));
        nlines--;
    }
    lines[nlines].kind = kind;
    wcsncpy(lines[nlines].text, t, UI_LINE_CHARS - 1);
    lines[nlines].text[UI_LINE_CHARS - 1] = 0;
    nlines++;
}

static void shot(HDC mem, unsigned short *px, const char *dir, const char *name, ui_state_t *s)
{
    char path[512];
    FILE *f;
    int y;
    s->nlines = nlines;
    s->lines = lines;
    ui_paint(mem, s, NULL);
    GdiFlush();
    snprintf(path, sizeof path, "%s/%s.565", dir, name);
    f = fopen(path, "wb");
    if (!f)
        return;
    for (y = 0; y < PH; y++)
        fwrite(px + y * PW, 2, PW, f);
    fclose(f);
    printf("%s\n", path);
}

int main(int argc, char **argv)
{
    /* a pasta vem do ambiente: com a saída redirecionada o wine perde os argumentos */
    const char *dir = getenv("BF_PREVIEW_DIR") ? getenv("BF_PREVIEW_DIR") : argc > 1 ? argv[1] : NULL;
    struct { BITMAPINFOHEADER h; DWORD masks[3]; } bi;
    HDC mem;
    HBITMAP b;
    void *bits = NULL;
    ui_state_t s;

    if (!dir) {
        fprintf(stderr, "uso: BF_PREVIEW_DIR=pasta preview.exe\n");
        return 2;
    }
    memset(&bi, 0, sizeof bi);
    bi.h.biSize = sizeof bi.h;
    bi.h.biWidth = PW;
    bi.h.biHeight = -PH;
    bi.h.biPlanes = 1;
    bi.h.biBitCount = 16;
    bi.h.biCompression = BI_BITFIELDS;
    bi.masks[0] = 0xF800;
    bi.masks[1] = 0x07E0;
    bi.masks[2] = 0x001F;
    mem = CreateCompatibleDC(NULL);
    b = CreateDIBSection(mem, (BITMAPINFO *)&bi, DIB_RGB_COLORS, &bits, NULL, 0);
    SelectObject(mem, b);
    if (!ui_init(mem, PW, PH)) {
        fprintf(stderr, "ui_init falhou\n");
        return 1;
    }
    memset(&s, 0, sizeof s);
    s.version = L"bfagent 0.8.0";
    s.port = L"COM9";
    s.linux_ok = 1;

    /* início do aparelho: conferindo o cartão, contagem em andamento */
    add(UI_L_INFO, L"bfagent 0.7.0 iniciado da memória interna");
    add(UI_L_INFO, L"Atualização do BigFoston encontrada no cartão SD: conferindo...");
    add(UI_L_INFO, L"Gravando zImage na memória interna...");
    s.autoboot = 1;                        /* contagem ainda não começou: gravando o cartão */
    shot(mem, (unsigned short *)bits, dir, "00-preparando", &s);
    add(UI_L_INFO, L"Gravando initrd.img na memória interna...");
    add(UI_L_OK, L"Atualização do BigFoston instalada e conferida.");
    add(UI_L_INFO, L"Pronto na porta COM9. No PC: tools/bigfoston/bfagent.py info");
    s.autoboot = 1;
    s.cd_total_ms = 12000;
    s.cd_elapsed_ms = 4300;
    shot(mem, (unsigned short *)bits, dir, "01-contagem", &s);
    s.pressed = UI_HIT_LINUX;
    s.cd_elapsed_ms = 9800;
    shot(mem, (unsigned short *)bits, dir, "02-contagem-tocando", &s);
    s.pressed = UI_HIT_NONE;

    /* ficou no Windows CE */
    add(UI_L_WARN, L"Início automático cancelado: ficando no Windows CE.");
    s.autoboot = 2;
    shot(mem, (unsigned short *)bits, dir, "03-cancelado", &s);

    /* PC conectado (diagnóstico) */
    add(UI_L_PC, L"#128 read (24 bytes)");
    s.autoboot = 0;
    s.requests = 128;
    shot(mem, (unsigned short *)bits, dir, "04-pc", &s);

    /* iniciando */
    s.autoboot = 3;
    add(UI_L_INFO, L"Iniciando o BigFoston (Linux)...");
    shot(mem, (unsigned short *)bits, dir, "05-iniciando", &s);

    /* sem Linux: mensagem longa em várias linhas */
    nlines = 0;
    add(UI_L_INFO, L"bfagent 0.7.0 iniciado da memória interna");
    add(UI_L_ERR, L"Linux da memória interna incompleto: ficando no Windows CE. Desligue e ligue para continuar a "
                  L"atualização do cartão, ou copie BigFoston\\recuperacao\\linux-0.2.0 para BigFoston\\update.");
    s.autoboot = 2;
    s.linux_ok = 0;
    s.requests = 0;
    shot(mem, (unsigned short *)bits, dir, "06-sem-linux", &s);
    /* aberto pelo cartão, sem o Dual Boot: instalar no toque */
    nlines = 0;
    add(UI_L_INFO, L"bfagent 0.8.0 iniciado (\\SDMMC\\BigFoston).");
    add(UI_L_INFO, L"Dual Boot não instalado: toque em Instalar o Dual Boot para gravar o BigFoston OS.");
    add(UI_L_OK, L"Pronto na porta COM6. No PC: tools/bigfoston/bfagent.py info");
    s.autoboot = 0;
    s.linux_ok = 0;
    s.setup = UI_SETUP_READY;
    s.can_restore = 0;
    s.status = L"Aberto pelo cartão: Dual Boot não instalado";
    shot(mem, (unsigned short *)bits, dir, "07-pelo-cartao", &s);
    s.overlay = UI_OV_INSTALL;
    shot(mem, (unsigned short *)bits, dir, "08-confirmar-instalar", &s);
    s.overlay = UI_OV_NONE;
    s.busy = 1;
    add(UI_L_INFO, L"Guardando os valores originais...");
    add(UI_L_OK, L"Agente copiado e conferido.");
    add(UI_L_INFO, L"Gravando initrd.img na memória interna...");
    s.status = L"Gravando o BigFoston OS na memória interna...";
    shot(mem, (unsigned short *)bits, dir, "09-instalando", &s);
    s.busy = 0;
    add(UI_L_OK, L"Dual Boot instalado e conferido.");
    s.setup = UI_SETUP_NO;
    s.linux_ok = 1;
    s.can_restore = 1;
    s.status = L"Dual Boot instalado";
    s.overlay = UI_OV_DONE;
    s.ov_title = L"Dual Boot instalado";
    s.ov_body = L"Desligue e ligue o FOSTON: a tela Dual Boot aparece sozinha e inicia o BigFoston OS. O botão "
                L"Navegação ainda abre este agente; para voltar a abrir o GPS por ele: Ajustes \x00bb Caminho da "
                L"navegação \x00bb o navegador original.";
    shot(mem, (unsigned short *)bits, dir, "10-instalado", &s);
    s.overlay = UI_OV_RESTORE;
    s.status = NULL;
    shot(mem, (unsigned short *)bits, dir, "11-confirmar-restaurar", &s);
    s.overlay = UI_OV_NONE;
    s.setup = UI_SETUP_NOFILES;
    s.can_restore = 0;
    s.linux_ok = 0;
    s.status = L"O cartão não tem o BigFoston OS";
    shot(mem, (unsigned short *)bits, dir, "12-cartao-sem-sistema", &s);
    ui_free();
    return 0;
}
