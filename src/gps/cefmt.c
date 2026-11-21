/* cefmt.c — printf do Windows CE sobre um vetor de palavras (ver cefmt.h).
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "cefmt.h"
#include <stdio.h>
#include <string.h>

size_t w16_len(const uint16_t *s)
{
    size_t n = 0;
    if(s) while(s[n]) n++;
    return n;
}

/* UTF-16 -> UTF-8 (BMP; substitutos viram '?') */
size_t w16_to_utf8(char *out, size_t n, const uint16_t *s)
{
    size_t o = 0;
    if(!n) return 0;
    for(; s && *s; s++) {
        uint16_t c = *s;
        char b[3];
        size_t k;
        if(c < 0x80) { b[0] = (char)c; k = 1; }
        else if(c < 0x800) { b[0] = (char)(0xC0 | c >> 6); b[1] = (char)(0x80 | (c & 0x3F)); k = 2; }
        else if(c >= 0xD800 && c < 0xE000) { b[0] = '?'; k = 1; }
        else { b[0] = (char)(0xE0 | c >> 12); b[1] = (char)(0x80 | ((c >> 6) & 0x3F)); b[2] = (char)(0x80 | (c & 0x3F)); k = 3; }
        if(o + k >= n) break;
        memcpy(out + o, b, k);
        o += k;
    }
    out[o] = 0;
    return o;
}

size_t utf8_to_w16(uint16_t *out, size_t n, const char *s)
{
    size_t o = 0;
    const unsigned char *p = (const unsigned char *)s;
    if(!n) return 0;
    while(*p && o + 1 < n) {
        uint32_t c = *p++;
        if(c >= 0xE0 && p[0] && p[1]) { c = (c & 0x0F) << 12 | (p[0] & 0x3F) << 6 | (p[1] & 0x3F); p += 2; }
        else if(c >= 0xC0 && p[0]) { c = (c & 0x1F) << 6 | (p[0] & 0x3F); p += 1; }
        out[o++] = (uint16_t)c;
    }
    out[o] = 0;
    return o;
}

static void put(char *out, size_t n, size_t *o, const char *s, size_t len)
{
    while(len--) {
        if(*o + 1 < n) out[*o] = *s;
        (*o)++;
        s++;
    }
}

size_t ce_vformat(char *out, size_t n, const char *fmt, int wide, const uint32_t **argp)
{
    const uint32_t *a = *argp;
    size_t o = 0;
    char spec[32], tmp[512];
    while(*fmt) {
        if(*fmt != '%') { put(out, n, &o, fmt++, 1); continue; }
        const char *start = fmt++;
        if(*fmt == '%') { put(out, n, &o, fmt++, 1); continue; }
        size_t sl = 0;
        spec[sl++] = '%';
        while(*fmt && strchr("-+ #0", *fmt) && sl < 20) spec[sl++] = *fmt++;
        if(*fmt == '*') { sl += (size_t)snprintf(spec + sl, sizeof spec - sl, "%d", (int)*a++); fmt++; }
        while(*fmt >= '0' && *fmt <= '9' && sl < 24) spec[sl++] = *fmt++;
        if(*fmt == '.') {
            spec[sl++] = *fmt++;
            if(*fmt == '*') { sl += (size_t)snprintf(spec + sl, sizeof spec - sl, "%d", (int)*a++); fmt++; }
            while(*fmt >= '0' && *fmt <= '9' && sl < 28) spec[sl++] = *fmt++;
        }
        int is64 = 0, lng = 0, shrt = 0;
        for(;;) {
            if(*fmt == 'l') { lng++; fmt++; }
            else if(*fmt == 'h') { shrt = 1; fmt++; }
            else if(!strncmp(fmt, "I64", 3)) { is64 = 1; fmt += 3; }
            else if(*fmt == 'L' || *fmt == 'z' || *fmt == 't' || *fmt == 'j') fmt++;
            else break;
        }
        if(lng >= 2) is64 = 1;
        char conv = *fmt ? *fmt++ : 0;
        int len = 0;
        switch(conv) {
        case 'd': case 'i': case 'u': case 'x': case 'X': case 'o': {
            spec[sl++] = 'l'; spec[sl++] = 'l'; spec[sl++] = conv; spec[sl] = 0;
            long long v;
            if(is64) { v = (long long)((uint64_t)a[0] | (uint64_t)a[1] << 32); a += 2; }
            else if(conv == 'd' || conv == 'i') v = shrt ? (short)*a++ : (int32_t)*a++;
            else v = shrt ? (unsigned short)*a++ : (uint32_t)*a++;
            len = snprintf(tmp, sizeof tmp, spec, v);
            break;
        }
        case 'f': case 'F': case 'e': case 'E': case 'g': case 'G': {
            spec[sl++] = conv; spec[sl] = 0;
            union { uint64_t u; double d; } v;
            v.u = (uint64_t)a[0] | (uint64_t)a[1] << 32;    /* palavra baixa primeiro */
            a += 2;
            len = snprintf(tmp, sizeof tmp, spec, v.d);
            break;
        }
        case 'c': case 'C': {
            spec[sl++] = 'c'; spec[sl] = 0;
            len = snprintf(tmp, sizeof tmp, spec, (int)(*a++ & 0xFF));
            break;
        }
        case 'p': {
            len = snprintf(tmp, sizeof tmp, "%08x", (unsigned)*a++);
            break;
        }
        case 's': case 'S': {
            /* %s: largura do formato (wsprintfW = UTF-16); %S: a outra; %hs / %ls forçam */
            int w = conv == 's' ? wide : !wide;
            if(shrt) w = 0;
            if(lng == 1) w = 1;
            const void *p = (const void *)(uintptr_t)*a++;
            char sbuf[400];
            if(!p) strcpy(sbuf, "(null)");
            else if(w) w16_to_utf8(sbuf, sizeof sbuf, p);
            else snprintf(sbuf, sizeof sbuf, "%s", (const char *)p);
            spec[sl++] = 's'; spec[sl] = 0;
            len = snprintf(tmp, sizeof tmp, spec, sbuf);
            break;
        }
        default:   /* desconhecido: copia literal */
            len = snprintf(tmp, sizeof tmp, "%.*s", (int)(fmt - start), start);
            break;
        }
        if(len > 0) put(out, n, &o, tmp, (size_t)len < sizeof tmp ? (size_t)len : sizeof tmp - 1);
    }
    if(n) out[o < n ? o : n - 1] = 0;
    *argp = a;
    return o;
}
