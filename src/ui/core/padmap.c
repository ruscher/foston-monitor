/* padmap.c — ver padmap.h.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "padmap.h"
#include <linux/input.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static const char *const names[PC__N] = {
    "a", "b", "x", "y", "back", "guide", "start", "leftstick", "rightstick", "leftshoulder",
    "rightshoulder", "dpup", "dpdown", "dpleft", "dpright", "misc1", "touchpad",
    "leftx", "lefty", "rightx", "righty", "lefttrigger", "righttrigger",
};

const char *padmap_ctl_name(int c) { return c >= 0 && c < PC__N ? names[c] : "?"; }

void padmap_guid(uint16_t bus, uint16_t vendor, uint16_t product, uint16_t version, char out[33])
{
    const uint16_t w[8] = { bus, 0, vendor, 0, product, 0, version, 0 };
    for(int i = 0; i < 8; i++)
        snprintf(out + 4 * i, 5, "%02x%02x", w[i] & 0xff, w[i] >> 8);
}

/* ------------------------------------------------------------ linha */

static bool is_hex32(const char *s)
{
    for(int i = 0; i < 32; i++)
        if(!((s[i] >= '0' && s[i] <= '9') || (s[i] >= 'a' && s[i] <= 'f') || (s[i] >= 'A' && s[i] <= 'F')))
            return false;
    return s[32] == ',';
}

static bool parse_src(const char *v, pm_src_t *s)
{
    memset(s, 0, sizeof *s);
    if(*v == '+') { s->half = 1; v++; }
    else if(*v == '-') { s->half = -1; v++; }
    char *end;
    if(v[0] == 'b' || v[0] == 'a') {
        long n = strtol(v + 1, &end, 10);
        if(end == v + 1 || n < 0 || n > 255) return false;
        s->kind = v[0] == 'b' ? PS_BUTTON : PS_AXIS;
        s->idx = (uint8_t)n;
        if(*end == '~') { s->invert = true; end++; }
        return *end == 0 && (s->kind == PS_AXIS || !s->half);
    }
    if(v[0] == 'h') {
        long n = strtol(v + 1, &end, 10);
        if(end == v + 1 || *end != '.' || n < 0 || n > 3) return false;
        char *e2;
        long m = strtol(end + 1, &e2, 10);
        if(e2 == end + 1 || *e2 || (m != 1 && m != 2 && m != 4 && m != 8)) return false;
        s->kind = PS_HAT;
        s->idx = (uint8_t)n;
        s->mask = (uint8_t)m;
        return !s->half;
    }
    return false;
}

bool padmap_parse(const char *line, padmap_t *m)
{
    memset(m, 0, sizeof *m);
    while(*line == ' ' || *line == '\t') line++;
    if(!is_hex32(line)) return false;
    for(int i = 0; i < 32; i++) m->guid[i] = (char)(line[i] | 0x20);      /* minúsculas */
    const char *p = line + 33, *c = strchr(p, ',');
    if(!c) return false;
    size_t nl = (size_t)(c - p);
    if(nl >= sizeof m->name) nl = sizeof m->name - 1;
    memcpy(m->name, p, nl);
    m->name[nl] = 0;
    bool linux_ = true, any = false;
    for(p = c + 1; *p && *p != '\n' && *p != '\r';) {
        char f[64];
        size_t k = 0;
        while(*p && *p != ',' && *p != '\n' && *p != '\r') {
            if(k < sizeof f - 1) f[k++] = *p;
            p++;
        }
        f[k] = 0;
        if(*p == ',') p++;
        char *colon = strchr(f, ':');
        if(!colon) continue;
        *colon = 0;
        const char *key = f, *val = colon + 1;
        if(!strcmp(key, "platform")) { linux_ = !strcasecmp(val, "Linux"); continue; }
        int th = 0;
        if(*key == '+') { th = 1; key++; }
        else if(*key == '-') { th = -1; key++; }
        int t = -1;
        for(int i = 0; i < PC__N; i++) if(!strcmp(key, names[i])) t = i;
        if(t < 0) continue;                           /* paddle1, misc2...: não usados aqui */
        pm_src_t s;
        if(!parse_src(val, &s)) continue;
        if(th) {
            if(t < PC__NBTN) continue;
            m->half[t - PC__NBTN][th > 0 ? 0 : 1] = s;
        } else m->src[t] = s;
        any = true;
    }
    return linux_ && any;
}

static int put_src(char *o, int n, const pm_src_t *s)
{
    switch(s->kind) {
    case PS_BUTTON: return snprintf(o, (size_t)n, "b%d", s->idx);
    case PS_AXIS: return snprintf(o, (size_t)n, "%sa%d%s", s->half > 0 ? "+" : s->half < 0 ? "-" : "", s->idx,
                                  s->invert ? "~" : "");
    case PS_HAT: return snprintf(o, (size_t)n, "h%d.%d", s->idx, s->mask);
    default: return 0;
    }
}

int padmap_format(const padmap_t *m, char *out, int n)
{
    int k = snprintf(out, (size_t)n, "%s,", m->guid);
    for(const char *c = m->name; *c && k < n - 1; c++) out[k++] = *c == ',' ? ' ' : *c;   /* o nome não leva vírgula */
    if(k < n) out[k] = 0;
    k += snprintf(out + k, k < n ? (size_t)(n - k) : 0, ",");
    for(int t = 0; t < PC__N && k < n; t++) {
        if(m->src[t].kind) {
            k += snprintf(out + k, (size_t)(n - k), "%s:", names[t]);
            if(k < n) k += put_src(out + k, n - k, &m->src[t]);
            if(k < n) k += snprintf(out + k, (size_t)(n - k), ",");
        }
        for(int h = 0; t >= PC__NBTN && h < 2 && k < n; h++) {
            const pm_src_t *s = &m->half[t - PC__NBTN][h];
            if(!s->kind) continue;
            k += snprintf(out + k, (size_t)(n - k), "%c%s:", h ? '-' : '+', names[t]);
            if(k < n) k += put_src(out + k, n - k, s);
            if(k < n) k += snprintf(out + k, (size_t)(n - k), ",");
        }
    }
    if(k < n) k += snprintf(out + k, (size_t)(n - k), "platform:Linux,");
    return k < n ? k : -1;
}

/* o CRC (bytes 2 e 3) não conta na comparação */
static bool guid_eq(const char *a, const char *b, bool no_version)
{
    for(int i = 0; i < 32; i++) {
        if(i >= 4 && i < 8) continue;
        if(no_version && i >= 24 && i < 28) continue;
        if((a[i] | 0x20) != (b[i] | 0x20)) return false;
    }
    return true;
}

bool padmap_find(const char *const *files, int nfiles, const char *guid, padmap_t *out)
{
    /* sem versão: a do aparelho é ignorada só contra linhas com a versão zerada (o SDL) */
    char zver[33];
    memcpy(zver, guid, 33);
    memcpy(zver + 24, "0000", 4);
    for(int pass = 0; pass < 2; pass++) {
        for(int f = 0; f < nfiles; f++) {
            FILE *fp = files[f] ? fopen(files[f], "r") : NULL;
            if(!fp) continue;
            char line[1024];
            bool found = false;
            while(!found && fgets(line, sizeof line, fp)) {
                if(!is_hex32(line)) continue;
                if(pass == 0 ? !guid_eq(line, guid, false) : !guid_eq(line, zver, false)) continue;
                found = padmap_parse(line, out);
            }
            fclose(fp);
            if(found) {
                memcpy(out->guid, guid, 33);
                return true;
            }
        }
    }
    return false;
}

/* ------------------------------------------------------------ aparelho */

#define LBITS (8 * sizeof(unsigned long))

void pm_bits_set(pm_bits_t *b, bool key, unsigned code)
{
    unsigned long *a = key ? b->key : b->abs;
    unsigned max = key ? 0x2ff : 0x3f;
    if(code <= max) a[code / LBITS] |= 1UL << (code % LBITS);
}

bool pm_bits_test(const pm_bits_t *b, bool key, unsigned code)
{
    const unsigned long *a = key ? b->key : b->abs;
    unsigned max = key ? 0x2ff : 0x3f;
    return code <= max && ((a[code / LBITS] >> (code % LBITS)) & 1);
}

/* a ordem dos botões no SDL: BTN_JOYSTICK..KEY_MAX, depois 0..BTN_JOYSTICK */
static int button_nth(const pm_bits_t *b, int idx, unsigned want)
{
    int n = 0;
    for(int pass = 0; pass < 2; pass++) {
        unsigned from = pass ? 0 : BTN_JOYSTICK, to = pass ? BTN_JOYSTICK : 0x2ff;
        for(unsigned c = from; c < to; c++) {
            if(!pm_bits_test(b, true, c)) continue;
            if(want != 0xffff ? c == want : n == idx) return want != 0xffff ? n : (int)c;
            n++;
        }
    }
    return -1;
}

int pm_button_code(const pm_bits_t *b, int idx) { return idx < 0 ? -1 : button_nth(b, idx, 0xffff); }
int pm_button_index(const pm_bits_t *b, unsigned code) { return pm_bits_test(b, true, code) ? button_nth(b, 0, code) : -1; }

static int axis_nth(const pm_bits_t *b, int idx, unsigned want)
{
    int n = 0;
    for(unsigned c = 0; c < 0x3f; c++) {
        if(c >= ABS_HAT0X && c <= ABS_HAT3Y) continue;
        if(!pm_bits_test(b, false, c)) continue;
        if(want != 0xffff ? c == want : n == idx) return want != 0xffff ? n : (int)c;
        n++;
    }
    return -1;
}

int pm_axis_code(const pm_bits_t *b, int idx) { return idx < 0 ? -1 : axis_nth(b, idx, 0xffff); }
int pm_axis_index(const pm_bits_t *b, unsigned code)
{
    if(code >= ABS_HAT0X && code <= ABS_HAT3Y) return -1;
    return pm_bits_test(b, false, code) ? axis_nth(b, 0, code) : -1;
}

int pm_hat_code(const pm_bits_t *b, int idx)
{
    int n = 0;
    for(unsigned c = ABS_HAT0X; c <= ABS_HAT3Y; c += 2)
        if(pm_bits_test(b, false, c) || pm_bits_test(b, false, c + 1)) {
            if(n == idx) return (int)c;
            n++;
        }
    return -1;
}

int pm_hat_index(const pm_bits_t *b, unsigned code)
{
    if(code < ABS_HAT0X || code > ABS_HAT3Y) return -1;
    unsigned x = ABS_HAT0X + (code - ABS_HAT0X) / 2 * 2;
    for(int i = 0; i < 4; i++) if(pm_hat_code(b, i) == (int)x) return i;
    return -1;
}

static void btn(const pm_bits_t *b, padmap_t *m, int t, unsigned code)
{
    if(m->src[t].kind) return;
    int i = pm_button_index(b, code);
    if(i >= 0) m->src[t] = (pm_src_t){ .kind = PS_BUTTON, .idx = (uint8_t)i };
}

static void axis(const pm_bits_t *b, padmap_t *m, int t, unsigned code)
{
    if(m->src[t].kind) return;
    int i = pm_axis_index(b, code);
    if(i >= 0) m->src[t] = (pm_src_t){ .kind = PS_AXIS, .idx = (uint8_t)i };
}

void padmap_linux_default(const pm_bits_t *b, padmap_t *m)
{
    char guid[33];
    memcpy(guid, m->guid, sizeof guid);
    memset(m, 0, sizeof *m);
    memcpy(m->guid, guid, sizeof guid);
    snprintf(m->name, sizeof m->name, "Linux");
    btn(b, m, PC_A, BTN_SOUTH);
    btn(b, m, PC_B, BTN_EAST);
    btn(b, m, PC_X, BTN_WEST);
    btn(b, m, PC_Y, BTN_NORTH);
    /* joystick genérico (botão 1, 2, 3, 4) */
    btn(b, m, PC_A, BTN_TRIGGER);
    btn(b, m, PC_B, BTN_THUMB);
    btn(b, m, PC_X, BTN_THUMB2);
    btn(b, m, PC_Y, BTN_TOP);
    btn(b, m, PC_LSHOULDER, BTN_TL);
    btn(b, m, PC_RSHOULDER, BTN_TR);
    btn(b, m, PC_BACK, BTN_SELECT);
    btn(b, m, PC_START, BTN_START);
    btn(b, m, PC_GUIDE, BTN_MODE);
    btn(b, m, PC_LSTICK, BTN_THUMBL);
    btn(b, m, PC_RSTICK, BTN_THUMBR);
    axis(b, m, PC_LEFTX, ABS_X);
    axis(b, m, PC_LEFTY, ABS_Y);
    if(pm_bits_test(b, false, ABS_RX) && pm_bits_test(b, false, ABS_RY)) {
        axis(b, m, PC_RIGHTX, ABS_RX);
        axis(b, m, PC_RIGHTY, ABS_RY);
        axis(b, m, PC_LTRIGGER, ABS_Z);
        axis(b, m, PC_RTRIGGER, ABS_RZ);
    } else {                                          /* muitos controles HID: Z/RZ = analógico direito */
        axis(b, m, PC_RIGHTX, ABS_Z);
        axis(b, m, PC_RIGHTY, ABS_RZ);
    }
    axis(b, m, PC_LTRIGGER, ABS_BRAKE);
    axis(b, m, PC_RTRIGGER, ABS_GAS);
    btn(b, m, PC_LTRIGGER, BTN_TL2);
    btn(b, m, PC_RTRIGGER, BTN_TR2);
    int h = pm_hat_index(b, ABS_HAT0X);
    if(h >= 0) {
        static const uint8_t mask[4] = { 1, 4, 8, 2 };
        for(int d = 0; d < 4; d++) m->src[PC_DPUP + d] = (pm_src_t){ .kind = PS_HAT, .idx = (uint8_t)h, .mask = mask[d] };
    }
    btn(b, m, PC_DPUP, BTN_DPAD_UP);
    btn(b, m, PC_DPDOWN, BTN_DPAD_DOWN);
    btn(b, m, PC_DPLEFT, BTN_DPAD_LEFT);
    btn(b, m, PC_DPRIGHT, BTN_DPAD_RIGHT);
}
