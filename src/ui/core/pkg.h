/* pkg.h — pacotes de aplicativo .bfapp: instalar e desinstalar.
 *
 * Um .bfapp é um arquivo tar (ustar), cru ou com gzip, com uma pasta só, a do app:
 *   sc-controller/sc-controller.desktop   o .desktop (Categories= decide o grupo)
 *   sc-controller/icon.svg                o ícone (Icon=icon.svg)
 *   sc-controller/...                     o resto do app
 * O nome da pasta é o id do app ([a-z0-9._-], até 40). A instalação extrai em
 * <apps>/.<id>.novo, confere o .desktop e só então troca pela versão anterior (atualizar =
 * instalar de novo). Recusa caminho absoluto, "..", links, aparelhos e mais de PKG_MAX_FILES
 * arquivos ou PKG_MAX_BYTES bytes.
 *
 * Sem LVGL: testado em tests/test_pkg.c.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef BF_PKG_H
#define BF_PKG_H
#include <stdbool.h>
#include <stddef.h>

#define PKG_MAX_FILES 512
#define PKG_MAX_BYTES (24u * 1024 * 1024)

typedef enum {
    PKG_OK, PKG_E_READ, PKG_E_FORMAT, PKG_E_UNSAFE, PKG_E_BIG, PKG_E_NODESKTOP, PKG_E_WRITE, PKG_E_NOTFOUND,
} pkg_err_t;

typedef struct {
    char id[48];
    char name[64], comment[96], categories[128];
    size_t bytes;                 /* tamanho instalado */
    int files;
    bool installed;               /* já há um app com este id (instalar = atualizar) */
} pkg_info_t;

/* lê o pacote sem instalar (para o "Instalar?"); lang ("pt_BR", NULL) escolhe Name[..] e
 * Comment[..] do .desktop como o shell faz */
pkg_err_t pkg_info(const char *file, const char *apps_dir, const char *lang, pkg_info_t *out);
/* instala em apps_dir/<id>; out (opcional) recebe as informações */
pkg_err_t pkg_install(const char *file, const char *apps_dir, const char *lang, pkg_info_t *out);
/* apaga apps_dir/<id> */
pkg_err_t pkg_uninstall(const char *apps_dir, const char *id);
bool pkg_valid_id(const char *id);
const char *pkg_err_name(pkg_err_t e);   /* texto curto, em inglês, para o log */
#endif
