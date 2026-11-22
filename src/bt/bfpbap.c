/* bfpbap — agenda e histórico de chamadas do celular por PBAP (OBEX sobre RFCOMM), sem o obexd.
 *
 *   bfpbap ENDERECO CANAL REPOSITORIO SAIDA.tsv
 *
 * REPOSITORIO: pb (agenda), ich (recebidas), och (feitas), mch (perdidas), cch (todas);
 * "sim1/pb" etc. para o chip. CANAL: canal RFCOMM do PBAP do celular (registro SDP 0x112F).
 *
 * Pede vCard 3.0 só com os campos usados (sem fotos) e grava um TSV (UTF-8, uma linha por
 * contato/chamada; tabulações e quebras dos valores viram espaço):
 *   agenda:    nome \t telefones (tipo:número separados por ;) \t e-mail \t empresa
 *   histórico: tipo (R/F/P) \t nome \t número \t data e hora (AAAAMMDDTHHMMSS)
 * Grava em SAIDA.tsv.tmp e renomeia no fim. Imprime "ok N" (registros) ou "erro motivo".
 * O celular pede autorização ao usuário na primeira vez (Android: "Permitir acesso aos
 * contatos"); sem ela o GET é recusado (0xC3 Forbidden).
 *
 * Limites: cada vCard até 16 KB, até 20000 registros, linha do TSV até 1 KB.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#define _GNU_SOURCE
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define AF_BT 31
#define BTPROTO_RFCOMM 3
struct sockaddr_rc { unsigned short family; uint8_t bdaddr[6]; uint8_t channel; };

#define MAXPKT 32767
static const uint8_t PBAP_TARGET[16] = { 0x79, 0x61, 0x35, 0xf0, 0xf0, 0xc5, 0x11, 0xd8,
                                         0x09, 0x66, 0x08, 0x00, 0x20, 0x0c, 0x9a, 0x66 };
static int sk = -1;
static uint8_t cid[4];
static int have_cid;
static uint8_t *pkt;

/* um pacote OBEX inteiro (o RFCOMM é um fluxo: lê o cabeçalho e depois o resto) */
static int read_full(uint8_t *b, int n, int ms)
{
    int got = 0;
    while(got < n) {
        struct pollfd p = { sk, POLLIN, 0 };
        if(poll(&p, 1, ms) <= 0) return -1;
        ssize_t r = read(sk, b + got, (size_t)(n - got));
        if(r <= 0) return -1;
        got += (int)r;
    }
    return got;
}

static int recv_pkt(int ms)
{
    if(read_full(pkt, 3, ms) < 0) return -1;
    int len = pkt[1] << 8 | pkt[2];
    if(len < 3 || len > MAXPKT) return -1;
    if(len > 3 && read_full(pkt + 3, len - 3, ms) < 0) return -1;
    return len;
}

static int send_all(const uint8_t *b, int n)
{
    while(n > 0) {
        ssize_t w = write(sk, b, (size_t)n);
        if(w <= 0) return 0;
        b += w;
        n -= (int)w;
    }
    return 1;
}

static int hdr(uint8_t *p, uint8_t id, const void *data, int n)
{
    p[0] = id;
    p[1] = (uint8_t)((n + 3) >> 8);
    p[2] = (uint8_t)(n + 3);
    memcpy(p + 3, data, (size_t)n);
    return n + 3;
}

static int obex_connect(void)
{
    uint8_t c[64];
    int n = 7;
    c[0] = 0x80; c[3] = 0x10; c[4] = 0; c[5] = 0x7f; c[6] = 0xff;
    n += hdr(c + n, 0x46, PBAP_TARGET, 16);
    c[1] = (uint8_t)(n >> 8); c[2] = (uint8_t)n;
    if(!send_all(c, n)) return 0;
    int len = recv_pkt(15000);
    if(len < 7 || pkt[0] != 0xa0) return 0;
    for(int i = 7; i < len;) {
        uint8_t id = pkt[i];
        if(id == 0xcb && i + 5 <= len) { memcpy(cid, pkt + i + 1, 4); have_cid = 1; i += 5; }
        else if((id & 0xc0) == 0xc0) i += 5;
        else if((id & 0xc0) == 0x80) i += 2;
        else { int l = pkt[i + 1] << 8 | pkt[i + 2]; if(l < 3) break; i += l; }
    }
    return 1;
}

/* ---------------------------------------------------------- vCard -> TSV */

static FILE *out;
static int is_history;
static long records;
static char card[16384];
static int card_len, card_over;

static void clean(char *s)                     /* sem tabulação/quebra: o TSV fica íntegro */
{
    for(; *s; s++) if(*s == '\t' || *s == '\r' || *s == '\n') *s = ' ';
}

/* desfaz o escape do vCard 3.0 (\, \; \n) no lugar */
static void unescape(char *s)
{
    char *w = s;
    for(; *s; s++) {
        if(*s == '\\' && s[1]) { s++; *w++ = (*s == 'n' || *s == 'N') ? ' ' : *s; }
        else *w++ = *s;
    }
    *w = 0;
}

static void emit_card(void)
{
    if(card_over || !card_len) return;
    card[card_len] = 0;
    char fn[256] = "", n_[256] = "", tels[700] = "", email[160] = "", org[160] = "", when[32] = "", kind = 0;
    size_t tl = 0;
    /* linhas desdobradas: uma linha que começa com espaço continua a anterior */
    char *save = NULL;
    for(char *line = strtok_r(card, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        size_t L = strlen(line);
        if(L && line[L - 1] == '\r') line[--L] = 0;
        char *colon = strchr(line, ':');
        if(!colon) continue;
        *colon = 0;
        char *val = colon + 1, *name = line;
        char *semi = strchr(name, ';');
        char *params = semi ? semi + 1 : "";
        if(semi) *semi = 0;
        unescape(val);
        if(!strcasecmp(name, "FN")) snprintf(fn, sizeof fn, "%s", val);
        else if(!strcasecmp(name, "N")) {
            /* N:Sobrenome;Nome;... -> "Nome Sobrenome" */
            char *f2 = strchr(val, ';');
            if(f2) { *f2++ = 0; char *f3 = strchr(f2, ';'); if(f3) *f3 = 0; snprintf(n_, sizeof n_, "%s%s%s", f2, *f2 && *val ? " " : "", val); }
            else snprintf(n_, sizeof n_, "%s", val);
        }
        else if(!strcasecmp(name, "TEL") && *val && tl < sizeof tels - 40) {
            const char *t = strcasestr(params, "CELL") ? "cel" : strcasestr(params, "HOME") ? "casa" :
                            strcasestr(params, "WORK") ? "trabalho" : "outro";
            tl += (size_t)snprintf(tels + tl, sizeof tels - tl, "%s%s:%.40s", tl ? ";" : "", t, val);
        }
        else if(!strcasecmp(name, "EMAIL") && !email[0]) snprintf(email, sizeof email, "%s", val);
        else if(!strcasecmp(name, "ORG") && !org[0]) { snprintf(org, sizeof org, "%s", val); for(char *c = org; *c; c++) if(*c == ';') *c = ' '; }
        else if(!strcasecmp(name, "X-IRMC-CALL-DATETIME")) {
            snprintf(when, sizeof when, "%.20s", val);
            kind = strcasestr(params, "MISSED") ? 'P' : strcasestr(params, "RECEIVED") ? 'R' :
                   strcasestr(params, "DIALED") ? 'F' : 0;
        }
    }
    const char *nm = fn[0] ? fn : n_;
    clean((char *)nm); clean(tels); clean(email); clean(org); clean(when);
    if(is_history) {
        char num[48] = "";
        const char *c = strchr(tels, ':');
        if(c) { snprintf(num, sizeof num, "%s", c + 1); char *s = strchr(num, ';'); if(s) *s = 0; }
        if(!num[0] && !nm[0]) return;
        fprintf(out, "%c\t%s\t%s\t%s\n", kind ? kind : '?', nm, num, when);
    } else {
        if(!nm[0] && !tels[0]) return;
        fprintf(out, "%s\t%s\t%s\t%s\n", nm, tels, email, org);
    }
    records++;
}

/* corpo do GET em pedaços: junta as linhas e fecha cada vCard em END:VCARD */
static char linebuf[2048];
static int line_len;

static void feed(const uint8_t *b, int n)
{
    for(int i = 0; i < n; i++) {
        char ch = (char)b[i];
        if(line_len < (int)sizeof linebuf - 1) linebuf[line_len++] = ch;
        if(ch != '\n') continue;
        linebuf[line_len] = 0;
        if(!strncasecmp(linebuf, "BEGIN:VCARD", 11)) { card_len = 0; card_over = 0; }
        else if(!strncasecmp(linebuf, "END:VCARD", 9)) { if(records < 20000) emit_card(); card_len = 0; }
        else if((linebuf[0] == ' ' || linebuf[0] == '\t') && card_len > 0) {   /* continuação */
            if(card[card_len - 1] == '\n') card_len--;
            if(card_len > 0 && card[card_len - 1] == '\r') card_len--;
            int k = line_len - 1;
            if(card_len + k < (int)sizeof card - 1) { memcpy(card + card_len, linebuf + 1, (size_t)k); card_len += k; }
            else card_over = 1;
        }
        else if(card_len + line_len < (int)sizeof card - 1) { memcpy(card + card_len, linebuf, (size_t)line_len); card_len += line_len; }
        else card_over = 1;
        line_len = 0;
    }
}

static int pull(const char *repo)
{
    char name[96];
    snprintf(name, sizeof name, "%s%s.vcf", strchr(repo, '/') ? "" : "telecom/", repo);
    if(!strncmp(repo, "sim1/", 5)) snprintf(name, sizeof name, "SIM1/telecom/%s.vcf", repo + 5);
    uint8_t r[512];
    int n = 3;
    r[0] = 0x83;
    if(have_cid) { r[n++] = 0xcb; memcpy(r + n, cid, 4); n += 4; }
    uint8_t u16[200];
    int ul = 0;
    for(const char *c = name; *c && ul < 190; c++) { u16[ul++] = 0; u16[ul++] = (uint8_t)*c; }
    u16[ul++] = 0; u16[ul++] = 0;
    n += hdr(r + n, 0x01, u16, ul);                                   /* Name */
    static const char type[] = "x-bt/phonebook";
    n += hdr(r + n, 0x42, type, sizeof type);                          /* Type */
    /* parâmetros: formato vCard 3.0; campos VERSION FN N TEL EMAIL ORG X-IRMC-CALL-DATETIME;
     * até 65535 registros a partir do 0 */
    uint8_t ap[] = { 0x07, 1, 0x01,
                     0x06, 8, 0, 0, 0, 0, 0x10, 0x01, 0x01, 0x87,
                     0x04, 2, 0xff, 0xff,
                     0x05, 2, 0x00, 0x00 };
    n += hdr(r + n, 0x4c, ap, sizeof ap);
    r[1] = (uint8_t)(n >> 8); r[2] = (uint8_t)n;
    for(int round = 0; round < 100000; round++) {
        if(!send_all(r, n)) return -1;
        int len = recv_pkt(30000);
        if(len < 3) return -1;
        uint8_t code = pkt[0];
        if(code != 0x90 && code != 0xa0) return code;
        for(int i = 3; i + 2 < len;) {
            uint8_t id = pkt[i];
            if((id & 0xc0) == 0xc0) { i += 5; continue; }
            if((id & 0xc0) == 0x80) { i += 2; continue; }
            int l = pkt[i + 1] << 8 | pkt[i + 2];
            if(l < 3 || i + l > len) break;
            if(id == 0x48 || id == 0x49) feed(pkt + i + 3, l - 3);
            i += l;
        }
        if(code == 0xa0) return 0;
        n = 3;
        r[0] = 0x83;
        if(have_cid) { r[n++] = 0xcb; memcpy(r + n, cid, 4); n += 4; }
        r[1] = (uint8_t)(n >> 8); r[2] = (uint8_t)n;
    }
    return -1;
}

int main(int argc, char **argv)
{
    if(argc < 5) { fprintf(stderr, "uso: bfpbap ENDERECO CANAL pb|ich|och|mch|cch SAIDA.tsv\n"); return 2; }
    unsigned v[6];
    if(sscanf(argv[1], "%x:%x:%x:%x:%x:%x", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) != 6) return 2;
    signal(SIGPIPE, SIG_IGN);
    pkt = malloc(MAXPKT + 1);
    if(!pkt) return 1;
    const char *repo = argv[3];
    is_history = strcmp(repo, "pb") && strcmp(repo, "sim1/pb");
    sk = socket(AF_BT, SOCK_STREAM | SOCK_CLOEXEC, BTPROTO_RFCOMM);
    if(sk < 0) { printf("erro socket\n"); return 1; }
    struct sockaddr_rc a = { AF_BT, { 0 }, (uint8_t)atoi(argv[2]) };
    for(int i = 0; i < 6; i++) a.bdaddr[i] = (uint8_t)v[5 - i];
    if(connect(sk, (struct sockaddr *)&a, sizeof a) < 0) { printf("erro conexao %d\n", errno); return 1; }
    if(!obex_connect()) { printf("erro obex\n"); return 1; }
    char tmp[512];
    snprintf(tmp, sizeof tmp, "%s.tmp", argv[4]);
    out = fopen(tmp, "w");
    if(!out) { printf("erro arquivo %d\n", errno); return 1; }
    int rc = pull(repo);
    fclose(out);
    close(sk);
    if(rc != 0) {
        unlink(tmp);
        if(rc == 0xc3) printf("erro negado\n");             /* Forbidden: sem autorização */
        else printf("erro %d\n", rc);
        return 1;
    }
    if(rename(tmp, argv[4]) != 0) { printf("erro gravar %d\n", errno); unlink(tmp); return 1; }
    printf("ok %ld\n", records);
    return 0;
}
