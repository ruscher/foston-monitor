/* test_webdata.c — core/webdata: barra de endereço (URL x pesquisa, esquemas recusados) e
 * persistência de favoritos, histórico e WebApps. make test (no PC, com ASan/UBSan).
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "core/webdata.h"
#include <stdio.h>
#include <string.h>
void data_path(char *o, size_t n, const char *name) { snprintf(o, n, "/nonexistent/%s", name); }
static int fails;
static void norm(const char *in, const char *want) {
    char out[1100]; bool ok = web_normalize(in, 0, out, sizeof out);
    if(!want) { if(ok) { printf("FALHA %-30s -> %s (devia recusar)\n", in, out); fails++; } return; }
    if(!ok || strcmp(out, want)) { printf("FALHA %-30s -> '%s' (esperado '%s')\n", in, out, want); fails++; }
}
int main(int argc, char **argv) {
    if(argc < 2) { fprintf(stderr, "uso: %s PASTA_TEMPORARIA\n", argv[0]); return 2; }
    norm("github.com", "https://github.com");
    norm("  www.wikipedia.org/wiki/GPS ", "https://www.wikipedia.org/wiki/GPS");
    norm("https://example.com/a?b=1", "https://example.com/a?b=1");
    norm("http://192.168.7.1:8000/", "http://192.168.7.1:8000/");
    norm("192.168.0.1", "http://192.168.0.1");
    norm("localhost:8080/x", "http://localhost:8080/x");
    norm("exemplo.com.br:8443", "https://exemplo.com.br:8443");
    norm("previsão do tempo", "https://html.duckduckgo.com/html/?q=previs%C3%A3o+do+tempo");
    norm("netsurf", "https://html.duckduckgo.com/html/?q=netsurf");
    norm("v1.2", "https://html.duckduckgo.com/html/?q=v1.2");
    norm("javascript:alert(1)", NULL);
    norm("JaVaScRiPt:alert(1)", NULL);
    norm("mailto:a@b.c", NULL);
    norm("", NULL);
    norm("   ", NULL);
    norm("about:blank", "about:blank");
    norm("file:///mnt/sd/a.html", "file:///mnt/sd/a.html");
    char h[100]; web_host("https://www.exemplo.com:443/x", h, sizeof h);
    if(strcmp(h, "exemplo.com")) { printf("FALHA host %s\n", h); fails++; }
    /* persistência */
    web_data_set_dir(argv[1]);
    web_data_load();
    if(!web_mark_add("Exemplo\tcom tab\n", "https://example.com")) { puts("FALHA add"); fails++; }
    if(web_mark_add("dup", "https://example.com")) { puts("FALHA duplicado aceito"); fails++; }
    if(web_mark_add("js", "javascript:x")) { puts("FALHA js aceito"); fails++; }
    for(int i = 0; i < 350; i++) { char u[64]; snprintf(u, sizeof u, "https://s%d.com", i); web_history_add("t", u); }
    web_history_add("Nova", "https://s10.com");
    web_data_flush();
    web_app_add("Wiki", "https://pt.m.wikipedia.org");
    web_data_load();
    if(web_marks_count() != 1 || strchr(web_mark(0)->title, '\t')) { puts("FALHA favorito relido"); fails++; }
    if(web_history_count() != WEB_HIST_MAX) { printf("FALHA hist n=%d\n", web_history_count()); fails++; }
    if(strcmp(web_history(0)->url, "https://s10.com") || strcmp(web_history(0)->title, "Nova")) { printf("FALHA hist topo %s\n", web_history(0)->url); fails++; }
    if(strcmp(web_history(1)->url, "https://s349.com")) { printf("FALHA hist 2o %s\n", web_history(1)->url); fails++; }
    if(web_apps_count() != 1) { puts("FALHA app"); fails++; }
    /* recarga sem título (o Chrome reporta uma carga curta da página de busca ao sair
     * dela): sobe para o topo mantendo o título conhecido; sem título nunca visto, o endereço */
    web_history_add("", "https://s300.com");
    if(strcmp(web_history(0)->url, "https://s300.com") || strcmp(web_history(0)->title, "t")) { printf("FALHA hist título mantido: %s\n", web_history(0)->title); fails++; }
    web_history_add(NULL, "https://novo.com");
    if(strcmp(web_history(0)->title, "https://novo.com")) { printf("FALHA hist sem título: %s\n", web_history(0)->title); fails++; }
    /* WebApps: duplicado recusado, inicial UTF-8, nome que é endereço vira o domínio */
    if(web_app_add("Outra", "https://pt.m.wikipedia.org")) { puts("FALHA app duplicado aceito"); fails++; }
    if(web_app_find("https://pt.m.wikipedia.org") != 0) { puts("FALHA app_find"); fails++; }
    char ini[8], host[64];
    web_initial("ética", ini, sizeof ini);
    if(strcmp(ini, "é")) { printf("FALHA inicial UTF-8: %s\n", ini); fails++; }
    web_initial("wiki", ini, sizeof ini);
    if(strcmp(ini, "W")) { printf("FALHA inicial: %s\n", ini); fails++; }
    if(strcmp(web_display_name("https://x.com/a", "https://www.exemplo.com/a", host, sizeof host), "exemplo.com"))
        { printf("FALHA nome-endereço: %s\n", host); fails++; }
    if(strcmp(web_display_name("Meu app", "https://a.com", host, sizeof host), "Meu app")) { puts("FALHA nome"); fails++; }
    web_apps_load();
    if(web_apps_count() != 1) { printf("FALHA apps_load n=%d\n", web_apps_count()); fails++; }
    web_history_set_enabled(false); web_history_add("x", "https://nao.com");
    if(!strcmp(web_history(0)->url, "https://nao.com")) { puts("FALHA hist desligado gravou"); fails++; }
    printf("%s\n", fails ? "ERROS" : "webdata OK");
    return fails != 0;
}
