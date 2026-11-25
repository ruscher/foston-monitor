/* test_pkg.c — pacotes .bfapp (core/pkg.c): instalar, atualizar, desinstalar e recusar
 * pacotes perigosos ou quebrados. Os tar são montados aqui mesmo (ustar), com e sem gzip. */
#include "core/pkg.h"
#include "lodepng.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int fails, total;
static char dir[256], apps[300];

static void ok(const char *what, int cond)
{
    total++;
    if(!cond) { fails++; printf("FALHOU %s\n", what); }
}

/* ------------------------------------------------ tar na memória */
static unsigned char tarbuf[1 << 20];
static size_t tarn;

static void add(const char *name, char type, const char *data)
{
    unsigned char *h = tarbuf + tarn;
    memset(h, 0, 512);
    size_t n = data ? strlen(data) : 0;
    snprintf((char *)h, 100, "%s", name);
    snprintf((char *)h + 100, 8, "%07o", type == '5' ? 0755 : 0644);
    snprintf((char *)h + 108, 8, "%07o", 0);
    snprintf((char *)h + 116, 8, "%07o", 0);
    snprintf((char *)h + 124, 12, "%011o", (unsigned)n);
    snprintf((char *)h + 136, 12, "%011o", 0);
    h[156] = (unsigned char)type;
    memcpy(h + 257, "ustar", 6);
    memcpy(h + 263, "00", 2);
    memset(h + 148, ' ', 8);
    unsigned sum = 0;
    for(int i = 0; i < 512; i++) sum += h[i];
    snprintf((char *)h + 148, 8, "%06o", sum);
    tarn += 512;
    if(n) { memcpy(tarbuf + tarn, data, n); tarn += (n + 511) / 512 * 512; }
}

static void finish(const char *file, bool gz)
{
    memset(tarbuf + tarn, 0, 1024);
    tarn += 1024;
    FILE *f = fopen(file, "wb");
    if(!gz) fwrite(tarbuf, 1, tarn, f);
    else {
        unsigned char *z = NULL;
        size_t zn = 0;
        lodepng_deflate(&z, &zn, tarbuf, tarn, &lodepng_default_compress_settings);
        static const unsigned char hdr[10] = { 0x1f, 0x8b, 8, 0, 0, 0, 0, 0, 0, 3 };
        fwrite(hdr, 1, 10, f);
        fwrite(z, 1, zn, f);
        unsigned crc = lodepng_crc32(tarbuf, tarn);
        unsigned char tail[8] = { (unsigned char)crc, (unsigned char)(crc >> 8), (unsigned char)(crc >> 16),
                                  (unsigned char)(crc >> 24), (unsigned char)tarn, (unsigned char)(tarn >> 8),
                                  (unsigned char)(tarn >> 16), (unsigned char)(tarn >> 24) };
        fwrite(tail, 1, 8, f);
        free(z);
    }
    fclose(f);
    tarn = 0;
}

static const char desk[] =
    "[Desktop Entry]\nName=Jogo Teste\nName[pt_BR]=Jogo de Teste\nComment=Um jogo\n"
    "Categories=Game;ArcadeGame;\nIcon=icon.svg\nExec=true\nX-BigFoston-Color=#7a4cd9\n";

static bool exists(const char *rel)
{
    char p[600];
    struct stat st;
    snprintf(p, sizeof p, "%s/%s", apps, rel);
    return !stat(p, &st);
}

static void good(const char *file, bool gz, const char *ver)
{
    add("jogo/", '5', NULL);
    add("jogo/jogo.desktop", '0', desk);
    add("jogo/icon.svg", '0', "<svg/>");
    add("jogo/dados/", '5', NULL);
    add("jogo/dados/versao.txt", '0', ver);
    finish(file, gz);
}

int main(int argc, char **argv)
{
    snprintf(dir, sizeof dir, "%s", argc > 1 ? argv[1] : "/tmp/test_pkg");
    snprintf(apps, sizeof apps, "%s/apps", dir);
    mkdir(dir, 0755);
    mkdir(apps, 0755);
    char f[400];
    pkg_info_t in;

    ok("ids válidos", pkg_valid_id("sc-controller") && pkg_valid_id("a.b_c") && !pkg_valid_id("..")
       && !pkg_valid_id(".oculto") && !pkg_valid_id("Maiuscula") && !pkg_valid_id("a/b") && !pkg_valid_id(""));

    snprintf(f, sizeof f, "%s/jogo.bfapp", dir);
    good(f, false, "1");
    ok("info", pkg_info(f, apps, NULL, &in) == PKG_OK && !strcmp(in.id, "jogo") && !strcmp(in.name, "Jogo Teste")
       && !strcmp(in.categories, "Game;ArcadeGame;") && !in.installed && in.files == 4);
    ok("instala", pkg_install(f, apps, NULL, &in) == PKG_OK && !in.installed);
    ok("arquivos no lugar", exists("jogo/jogo.desktop") && exists("jogo/icon.svg") && exists("jogo/dados/versao.txt"));
    ok("sem sobras", !exists(".jogo.novo") && !exists(".jogo.velho"));
    ok("agora consta instalado", pkg_info(f, apps, NULL, &in) == PKG_OK && in.installed);
    ok("nome no idioma", pkg_info(f, apps, "pt_BR", &in) == PKG_OK && !strcmp(in.name, "Jogo de Teste")
       && pkg_info(f, apps, "pt", &in) == PKG_OK && !strcmp(in.name, "Jogo Teste")
       && pkg_info(f, apps, "es", &in) == PKG_OK && !strcmp(in.name, "Jogo Teste"));

    snprintf(f, sizeof f, "%s/jogo2.bfapp", dir);
    good(f, true, "2");
    ok("atualiza com gzip", pkg_install(f, apps, NULL, &in) == PKG_OK && in.installed);
    char p[600], v[8] = "";
    snprintf(p, sizeof p, "%s/jogo/dados/versao.txt", apps);
    FILE *vf = fopen(p, "r");
    if(vf) { if(!fgets(v, sizeof v, vf)) v[0] = 0; fclose(vf); }
    ok("a versão nova ficou", !strcmp(v, "2"));

    /* perigosos */
    snprintf(f, sizeof f, "%s/mau.bfapp", dir);
    add("jogo/", '5', NULL); add("jogo/jogo.desktop", '0', desk); add("jogo/../../fora.txt", '0', "x");
    finish(f, false);
    ok("recusa ..", pkg_install(f, apps, NULL, NULL) == PKG_E_UNSAFE);
    add("/etc/passwd", '0', "x");
    finish(f, false);
    ok("recusa caminho absoluto", pkg_install(f, apps, NULL, NULL) == PKG_E_UNSAFE);
    add("jogo/", '5', NULL); add("jogo/jogo.desktop", '0', desk); add("jogo/link", '2', NULL);
    finish(f, false);
    ok("recusa link", pkg_install(f, apps, NULL, NULL) == PKG_E_UNSAFE);
    add("jogo/jogo.desktop", '0', desk); add("outro/x.txt", '0', "x");
    finish(f, false);
    ok("recusa duas pastas", pkg_install(f, apps, NULL, NULL) == PKG_E_UNSAFE);
    add("Jogo/jogo.desktop", '0', desk);
    finish(f, false);
    ok("recusa id inválido", pkg_install(f, apps, NULL, NULL) == PKG_E_UNSAFE);
    add("jogo/leiame.txt", '0', "x");
    finish(f, false);
    ok("recusa sem .desktop", pkg_install(f, apps, NULL, NULL) == PKG_E_NODESKTOP);
    FILE *bad = fopen(f, "wb");
    fwrite("isto nao e um tar", 1, 17, bad);
    fclose(bad);
    ok("recusa lixo", pkg_install(f, apps, NULL, NULL) != PKG_OK);
    ok("arquivo que não existe", pkg_info("/nao/existe.bfapp", apps, NULL, &in) == PKG_E_READ);
    ok("o app bom continua lá", exists("jogo/jogo.desktop"));
    ok("nada escapou", access("/fora.txt", F_OK) && !exists("../fora.txt"));

    ok("desinstala", pkg_uninstall(apps, "jogo") == PKG_OK && !exists("jogo"));
    ok("desinstalar de novo", pkg_uninstall(apps, "jogo") == PKG_E_NOTFOUND);
    ok("desinstalar id perigoso", pkg_uninstall(apps, "..") == PKG_E_UNSAFE);

    printf("pkg: %d/%d ok\n", total - fails, total);
    return fails ? 1 : 0;
}
