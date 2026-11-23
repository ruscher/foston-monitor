/* bfpbap — agenda e histórico de chamadas do celular por PBAP (OBEX sobre RFCOMM), sem o obexd.
 *
 *   bfpbap ENDERECO CANAL REPOSITORIO SAIDA.tsv [MAX]
 *   bfpbap ENDERECO CANAL push NUMERO TEXTO.txt      (responde/envia um SMS pelo MAP)
 *   bfpbap ENDERECO CANAL notify                     (mantém o MAP aberto com avisos de mensagem nova)
 *
 * Mensagens com MAX: pede só as MAX mais recentes de cada pasta e mescla com o TSV que já
 * existe (pelo identificador), sem perder as antigas.
 *
 * REPOSITORIO: pb (agenda), ich (recebidas), och (feitas), mch (perdidas), cch (todas);
 * "sim1/pb" etc. para o chip; msg/inbox e msg/sent: lista de mensagens (SMS) pelo MAP.
 * CANAL: canal RFCOMM do serviço no celular; 0 = descobrir pelo SDP (PBAP 0x112F, MAP 0x1132).
 *
 * Pede vCard 3.0 só com os campos usados (sem fotos) e grava um TSV (UTF-8, uma linha por
 * contato/chamada; tabulações e quebras dos valores viram espaço):
 *   agenda:    nome \t telefones (tipo:número separados por ;) \t e-mail \t empresa
 *   histórico: tipo (R/F/P) \t nome \t número \t data e hora (AAAAMMDDTHHMMSS)
 *   mensagens: lida (0/1) \t data e hora \t nome \t número \t início do texto \t identificador
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
#define BTPROTO_L2CAP 0
#define BTPROTO_RFCOMM 3
struct sockaddr_rc { unsigned short family; uint8_t bdaddr[6]; uint8_t channel; };
struct sockaddr_l2 { unsigned short family; unsigned short psm; uint8_t bdaddr[6]; unsigned short cid; uint8_t type; };

#define MAXPKT 32767
static const uint8_t PBAP_TARGET[16] = { 0x79, 0x61, 0x35, 0xf0, 0xf0, 0xc5, 0x11, 0xd8,
                                         0x09, 0x66, 0x08, 0x00, 0x20, 0x0c, 0x9a, 0x66 };
static const uint8_t MAP_TARGET[16] = { 0xbb, 0x58, 0x2b, 0x40, 0x42, 0x0c, 0x11, 0xdb,
                                        0xb0, 0xde, 0x08, 0x00, 0x20, 0x0c, 0x9a, 0x66 };
static const uint8_t *target = PBAP_TARGET;
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

static int obex_code;

static int obex_connect(void)
{
    uint8_t c[64];
    int n = 7;
    c[0] = 0x80; c[3] = 0x10; c[4] = 0; c[5] = 0x7f; c[6] = 0xff;
    n += hdr(c + n, 0x46, target, 16);
    if(target == MAP_TARGET) {                     /* MAP 1.2+: MapSupportedFeatures do cliente */
        static const uint8_t feat[] = { 0x29, 4, 0x00, 0x00, 0x00, 0x1f };
        n += hdr(c + n, 0x4c, feat, sizeof feat);
    }
    c[1] = (uint8_t)(n >> 8); c[2] = (uint8_t)n;
    if(!send_all(c, n)) return 0;
    int len = recv_pkt(45000);                     /* o celular pode estar pedindo autorização ao usuário */
    if(len < 7 || pkt[0] != 0xa0) { obex_code = len >= 1 ? pkt[0] : -1; return 0; }
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

/* ------------------------------------------------------------------ SDP */

/* canal RFCOMM do serviço uuid16 no celular: ServiceSearchAttributeRequest pela
 * ProtocolDescriptorList (0x0004) e procura "UUID16 0x0003 (RFCOMM), uint8 canal" */
static int sdp_channel(const uint8_t bd[6], unsigned uuid)
{
    int s = socket(AF_BT, SOCK_SEQPACKET | SOCK_CLOEXEC, BTPROTO_L2CAP);
    if(s < 0) return -1;
    struct sockaddr_l2 a = { AF_BT, 1, { 0 }, 0, 0 };      /* PSM 1 = SDP (little-endian) */
    memcpy(a.bdaddr, bd, 6);
    if(connect(s, (struct sockaddr *)&a, sizeof a) < 0) { close(s); return -1; }
    uint8_t q[] = { 0x06, 0x00, 0x01, 0x00, 0x0f,
                    0x35, 0x03, 0x19, (uint8_t)(uuid >> 8), (uint8_t)uuid,   /* padrão de busca */
                    0x01, 0x00,                                              /* até 256 bytes */
                    0x35, 0x05, 0x0a, 0x00, 0x04, 0x00, 0x04,                /* atributo 0x0004 */
                    0x00 };                                                  /* sem continuação */
    int ch = -1;
    if(write(s, q, sizeof q) == (ssize_t)sizeof q) {
        struct pollfd p = { s, POLLIN, 0 };
        uint8_t r[1024];
        ssize_t n = poll(&p, 1, 10000) > 0 ? read(s, r, sizeof r) : -1;
        if(n > 7 && r[0] == 0x07)
            for(ssize_t i = 7; i + 4 < n; i++)
                if(r[i] == 0x19 && r[i + 1] == 0x00 && r[i + 2] == 0x03 && r[i + 3] == 0x08) { ch = r[i + 4]; break; }
    }
    close(s);
    return ch;
}

/* ------------------------------------------------------------------ MAP */

static int setpath(const char *dir)
{
    uint8_t r[256];
    int n = 5;
    r[0] = 0x85; r[3] = 0x02; r[4] = 0x00;                 /* não criar a pasta */
    if(have_cid) { r[n++] = 0xcb; memcpy(r + n, cid, 4); n += 4; }
    uint8_t u16[128];
    int ul = 0;
    for(const char *c = dir; *c && ul < 120; c++) { u16[ul++] = 0; u16[ul++] = (uint8_t)*c; }
    u16[ul++] = 0; u16[ul++] = 0;
    n += hdr(r + n, 0x01, u16, ul);
    r[1] = (uint8_t)(n >> 8); r[2] = (uint8_t)n;
    if(!send_all(r, n)) return -1;
    int len = recv_pkt(15000);
    return len >= 3 && pkt[0] == 0xa0 ? 0 : (len >= 3 ? pkt[0] : -1);
}

static char *xml;
static size_t xml_len, xml_cap;

static void xml_feed(const uint8_t *b, int n)
{
    if(xml_len + (size_t)n + 1 > xml_cap) {
        size_t c = xml_cap ? xml_cap * 2 : 65536;
        while(c < xml_len + (size_t)n + 1) c *= 2;
        if(c > 4u << 20) return;                             /* lista absurda: ignora o resto */
        char *x = realloc(xml, c);
        if(!x) return;
        xml = x; xml_cap = c;
    }
    memcpy(xml + xml_len, b, (size_t)n);
    xml_len += (size_t)n;
    xml[xml_len] = 0;
}

/* valor do atributo name="..." de um elemento, com as entidades XML desfeitas */
static void attr(const char *el, const char *name, char *out_, size_t on)
{
    out_[0] = 0;
    char pat[40];
    snprintf(pat, sizeof pat, " %s=\"", name);
    const char *p = strstr(el, pat);
    if(!p) return;
    p += strlen(pat);
    size_t o = 0;
    for(; *p && *p != '"' && o + 4 < on; p++) {
        if(*p != '&') { out_[o++] = *p; continue; }
        const char *semi = strchr(p, ';');
        if(!semi || semi - p > 8) { out_[o++] = *p; continue; }
        unsigned cp = 0;
        if(!strncmp(p, "&amp;", 5)) cp = '&';
        else if(!strncmp(p, "&lt;", 4)) cp = '<';
        else if(!strncmp(p, "&gt;", 4)) cp = '>';
        else if(!strncmp(p, "&quot;", 6)) cp = '"';
        else if(!strncmp(p, "&apos;", 6)) cp = '\'';
        else if(p[1] == '#') cp = (unsigned)(p[2] == 'x' ? strtoul(p + 3, NULL, 16) : strtoul(p + 2, NULL, 10));
        if(!cp) { out_[o++] = *p; continue; }
        if(cp < 0x80) out_[o++] = (char)cp;                  /* UTF-8 */
        else if(cp < 0x800) { out_[o++] = (char)(0xc0 | cp >> 6); out_[o++] = (char)(0x80 | (cp & 0x3f)); }
        else { out_[o++] = (char)(0xe0 | (cp >> 12 & 0x0f)); out_[o++] = (char)(0x80 | (cp >> 6 & 0x3f)); out_[o++] = (char)(0x80 | (cp & 0x3f)); }
        p = semi;
    }
    out_[o] = 0;
    clean(out_);
}

static void emit_listing(void)
{
    if(!xml) return;
    for(char *el = strstr(xml, "<msg "); el && records < 20000; el = strstr(el + 5, "<msg ")) {
        char *end = strchr(el, '>');
        if(!end) break;
        *end = 0;
        char h[40], subj[320], when[32], name[160], addr[64], rd[8];
        attr(el, "handle", h, sizeof h);
        attr(el, "subject", subj, sizeof subj);
        attr(el, "datetime", when, sizeof when);
        attr(el, "sender_name", name, sizeof name);
        attr(el, "sender_addressing", addr, sizeof addr);
        attr(el, "read", rd, sizeof rd);
        if(!addr[0]) {                                       /* enviadas: o destinatário */
            attr(el, "recipient_name", name, sizeof name);
            attr(el, "recipient_addressing", addr, sizeof addr);
        }
        fprintf(out, "%d\t%s\t%s\t%s\t%s\t%s\n", !strcasecmp(rd, "yes"), when, name, addr, subj, h);
        records++;
        *end = '>';
    }
}

static int list_max = 200;

/* lista de mensagens de uma pasta (inbox, sent) */
static int pull_msgs(const char *folder)
{
    int rc;
    if((rc = setpath("telecom")) || (rc = setpath("msg"))) return rc;
    uint8_t r[512];
    int n = 3;
    r[0] = 0x83;
    if(have_cid) { r[n++] = 0xcb; memcpy(r + n, cid, 4); n += 4; }
    uint8_t u16[64];
    int ul = 0;
    for(const char *c = folder; *c && ul < 56; c++) { u16[ul++] = 0; u16[ul++] = (uint8_t)*c; }
    u16[ul++] = 0; u16[ul++] = 0;
    n += hdr(r + n, 0x01, u16, ul);
    static const char type[] = "x-bt/MAP-msg-listing";
    n += hdr(r + n, 0x42, type, sizeof type);
    uint8_t ap[] = { 0x01, 2, (uint8_t)(list_max >> 8), (uint8_t)list_max,  /* MaxListCount */
                     0x13, 1, 0xff };                        /* SubjectLength 255 */
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
            if(id == 0x48 || id == 0x49) xml_feed(pkt + i + 3, l - 3);
            i += l;
        }
        if(code == 0xa0) { emit_listing(); return 0; }
        n = 3;
        r[0] = 0x83;
        if(have_cid) { r[n++] = 0xcb; memcpy(r + n, cid, 4); n += 4; }
        r[1] = (uint8_t)(n >> 8); r[2] = (uint8_t)n;
    }
    return -1;
}

/* PUT simples (um pacote): devolve o código de resposta */
static int put1(const char *name, const char *type, const uint8_t *ap, int apn, const void *body, int bn)
{
    int cap = 512 + bn;
    uint8_t *r = malloc((size_t)cap);
    if(!r) return -1;
    int n = 3;
    r[0] = 0x82;
    if(have_cid) { r[n++] = 0xcb; memcpy(r + n, cid, 4); n += 4; }
    if(name) {
        uint8_t u16[64];
        int ul = 0;
        for(const char *c = name; *c && ul < 56; c++) { u16[ul++] = 0; u16[ul++] = (uint8_t)*c; }
        u16[ul++] = 0; u16[ul++] = 0;
        n += hdr(r + n, 0x01, u16, ul);
    }
    n += hdr(r + n, 0x42, type, (int)strlen(type) + 1);
    if(apn) n += hdr(r + n, 0x4c, ap, apn);
    n += hdr(r + n, 0x49, body, bn);
    r[1] = (uint8_t)(n >> 8); r[2] = (uint8_t)n;
    int ok = send_all(r, n);
    free(r);
    if(!ok) return -1;
    int len = recv_pkt(30000);
    return len >= 3 ? pkt[0] : -1;
}

/* envia um SMS (bMessage para telecom/msg/outbox) */
static int push_sms(const char *number, const char *file)
{
    FILE *f = fopen(file, "r");
    if(!f) return -1;
    char text[2048];
    size_t tl = fread(text, 1, sizeof text - 1, f);
    fclose(f);
    text[tl] = 0;
    for(char *c = text; *c; c++) if(*c == '\r') *c = ' ';
    char num[48];
    size_t o = 0;
    for(const char *c = number; *c && o + 1 < sizeof num; c++)
        if((*c >= '0' && *c <= '9') || *c == '+' || *c == '*' || *c == '#') num[o++] = *c;
    num[o] = 0;
    if(!num[0] || !text[0]) return -1;
    char msg[2200];
    int ml = snprintf(msg, sizeof msg, "BEGIN:MSG\r\n%s\r\nEND:MSG\r\n", text);
    char b[3000];
    int bn = snprintf(b, sizeof b,
        "BEGIN:BMSG\r\nVERSION:1.0\r\nSTATUS:READ\r\nTYPE:SMS_GSM\r\nFOLDER:telecom/msg/outbox\r\n"
        "BEGIN:BENV\r\nBEGIN:VCARD\r\nVERSION:2.1\r\nN:\r\nTEL:%s\r\nEND:VCARD\r\n"
        "BEGIN:BBODY\r\nCHARSET:UTF-8\r\nLENGTH:%d\r\n%sEND:BBODY\r\nEND:BENV\r\nEND:BMSG\r\n",
        num, ml, msg);
    if(bn <= 0 || bn >= (int)sizeof b) return -1;
    int rc;
    if((rc = setpath("telecom")) || (rc = setpath("msg"))) return rc;
    static const uint8_t ap[] = { 0x14, 1, 0x01 };               /* Charset UTF-8 */
    rc = put1("outbox", "x-bt/message", ap, sizeof ap, b, bn);
    return rc == 0xa0 ? 0 : rc;
}

/* pede ao celular os avisos de mensagem nova (ele conecta no nosso MNS) e fica aberto */
static int notify_session(void)
{
    static const uint8_t ap[] = { 0x0e, 1, 0x01 };               /* NotificationStatus = ligado */
    static const uint8_t fill = 0x30;
    int rc = put1(NULL, "x-bt/MAP-NotificationRegistration", ap, sizeof ap, &fill, 1);
    if(rc != 0xa0) return rc;
    printf("ok avisos\n");
    fflush(stdout);
    for(;;) {                                                    /* até o celular ou o bfphone fechar */
        struct pollfd p = { sk, POLLIN, 0 };
        if(poll(&p, 1, -1) < 0 && errno != EINTR) break;
        if(p.revents & (POLLHUP | POLLERR)) break;
        if(p.revents & POLLIN) {
            uint8_t junk[512];
            if(read(sk, junk, sizeof junk) <= 0) break;
        }
    }
    return 0;
}

/* acrescenta ao TSV novo as linhas do antigo que não vieram agora (pelo identificador, última coluna) */
static void merge_old(const char *newf, const char *oldf)
{
    FILE *o = fopen(oldf, "r");
    if(!o) return;
    FILE *n = fopen(newf, "r");
    char **seen = NULL;
    int ns = 0, cap = 0;
    char line[1100];
    while(n && fgets(line, sizeof line, n)) {
        char *h = strrchr(line, '\t');
        if(!h) continue;
        h[strcspn(h, "\n")] = 0;
        if(ns == cap) { cap = cap ? cap * 2 : 256; char **x = realloc(seen, sizeof *seen * (size_t)cap); if(!x) break; seen = x; }
        seen[ns++] = strdup(h + 1);
    }
    if(n) fclose(n);
    FILE *a = fopen(newf, "a");
    long kept = 0;
    while(a && fgets(line, sizeof line, o) && records + kept < 20000) {
        char *h = strrchr(line, '\t');
        if(!h) continue;
        char hh[64];
        snprintf(hh, sizeof hh, "%.*s", (int)strcspn(h + 1, "\n"), h + 1);
        int dup = 0;
        for(int i = 0; i < ns && !dup; i++) dup = seen[i] && !strcmp(seen[i], hh);
        if(!dup) { fputs(line, a); kept++; }
    }
    if(a) fclose(a);
    fclose(o);
    for(int i = 0; i < ns; i++) free(seen[i]);
    free(seen);
    records += kept;
}

int main(int argc, char **argv)
{
    if(argc < 4 || (argc < 5 && strcmp(argv[3], "notify"))) {
        fprintf(stderr, "uso: bfpbap ENDERECO CANAL(0=SDP) pb|ich|och|mch|cch|msg/inbox|msg/sent SAIDA.tsv [MAX]\n"
                        "     bfpbap ENDERECO CANAL push NUMERO TEXTO.txt | notify\n");
        return 2;
    }
    unsigned v[6];
    if(sscanf(argv[1], "%x:%x:%x:%x:%x:%x", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) != 6) return 2;
    signal(SIGPIPE, SIG_IGN);
    pkt = malloc(MAXPKT + 1);
    if(!pkt) return 1;
    const char *repo = argv[3];
    int is_push = !strcmp(repo, "push"), is_notify = !strcmp(repo, "notify");
    if(is_push && argc < 6) return 2;
    int is_msg = !strncmp(repo, "msg/", 4) || is_push || is_notify;
    is_history = !is_msg && strcmp(repo, "pb") && strcmp(repo, "sim1/pb");
    if(is_msg) target = MAP_TARGET;
    uint8_t bd[6];
    for(int i = 0; i < 6; i++) bd[i] = (uint8_t)v[5 - i];
    int ch = atoi(argv[2]);
    if(ch <= 0) ch = sdp_channel(bd, is_msg ? 0x1132 : 0x112f);
    if(ch <= 0 || ch > 30) { printf("erro sdp\n"); return 1; }
    sk = socket(AF_BT, SOCK_STREAM | SOCK_CLOEXEC, BTPROTO_RFCOMM);
    if(sk < 0) { printf("erro socket\n"); return 1; }
    struct sockaddr_rc a = { AF_BT, { 0 }, (uint8_t)ch };
    memcpy(a.bdaddr, bd, 6);
    if(connect(sk, (struct sockaddr *)&a, sizeof a) < 0) { printf("erro conexao %d\n", errno); return 1; }
    if(!obex_connect()) {
        if(obex_code == 0xc3) printf("erro negado\n");
        else if(obex_code == 0xd3) printf("erro indisponivel\n");   /* Android: acesso não permitido */
        else printf("erro obex %d\n", obex_code);
        return 1;
    }
    if(is_push || is_notify) {
        int rc = is_push ? push_sms(argv[4], argv[5]) : notify_session();
        close(sk);
        if(rc == 0) { if(is_push) printf("ok 1\n"); return 0; }
        if(rc == 0xc3) printf("erro negado\n");
        else printf("erro %d\n", rc);
        return 1;
    }
    int merge = 0;
    if(argc > 5 && is_msg) {
        list_max = atoi(argv[5]);
        if(list_max < 1) list_max = 1;
        if(list_max > 1024) list_max = 1024;
        merge = 1;
    }
    char tmp[512];
    snprintf(tmp, sizeof tmp, "%s.tmp", argv[4]);
    out = fopen(tmp, "w");
    if(!out) { printf("erro arquivo %d\n", errno); return 1; }
    int rc = is_msg ? pull_msgs(repo + 4) : pull(repo);
    fclose(out);
    close(sk);
    if(rc != 0) {
        unlink(tmp);
        if(rc == 0xc3) printf("erro negado\n");             /* Forbidden: sem autorização */
        else printf("erro %d\n", rc);
        return 1;
    }
    if(merge) merge_old(tmp, argv[4]);
    if(rename(tmp, argv[4]) != 0) { printf("erro gravar %d\n", errno); unlink(tmp); return 1; }
    printf("ok %ld\n", records);
    return 0;
}
