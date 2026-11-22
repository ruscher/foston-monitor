/* bgnav.c - see bgnav.h.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "bgnav.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAXP 4096

typedef struct { uint32_t *v; int n, cap; } vec_t;

static int vpush(vec_t *v, uint32_t a)
{
    if (v->n == v->cap) {
        int nc = v->cap ? v->cap * 2 : 1024;
        uint32_t *nv = realloc(v->v, (size_t)nc * sizeof *nv);
        if (!nv) return -1;
        v->v = nv;
        v->cap = nc;
    }
    v->v[v->n++] = a;
    return 0;
}

static double seglen(const uint32_t *a, const uint32_t *b)
{
    double dx = (double)(int32_t)(b[0] - a[0]), dy = (double)(int32_t)(b[1] - a[1]);
    return sqrt(dx * dx + dy * dy);
}

/* bearing of a -> b, degrees clockwise from north (world y grows southwards) */
static double bearing(const uint32_t *a, const uint32_t *b)
{
    double dx = (double)(int32_t)(b[0] - a[0]), dy = (double)(int32_t)(b[1] - a[1]);
    double t = atan2(dx, -dy) * 180.0 / M_PI;
    return t < 0 ? t + 360 : t;
}

static double norm180(double a)
{
    while (a > 180) a -= 360;
    while (a < -180) a += 360;
    return a;
}

/* out-bearing of an arc (first segment) and in-bearing (last segment) */
static double arc_out(bgr_pack_t *p, uint32_t arc, uint32_t *buf)
{
    int n = bgr_arc_points(p, arc, buf, MAXP);
    return n >= 2 ? bearing(&buf[0], &buf[2]) : 0;
}

static double arc_in(bgr_pack_t *p, uint32_t arc, uint32_t *buf)
{
    int n = bgr_arc_points(p, arc, buf, MAXP);
    return n >= 2 ? bearing(&buf[2 * (n - 2)], &buf[2 * (n - 1)]) : 0;
}

static void way_name(bgr_pack_t *p, uint32_t arc, char *buf, size_t n)
{
    bgr_way_name(p, bgr_arc_way(p, arc), buf, n);
}

static int is_round(bgr_pack_t *p, uint32_t arc) { return (bgr_way_info(p, bgr_arc_way(p, arc)) >> 8) & BGR_WF_ROUNDABOUT; }
static int is_link(bgr_pack_t *p, uint32_t arc) { return (bgr_way_info(p, bgr_arc_way(p, arc)) >> 8) & BGR_WF_LINK; }

static int push_man(bgn_route_t *r, int *cap, const bgn_man_t *m)
{
    if (r->nman == *cap) {
        int nc = *cap ? *cap * 2 : 64;
        bgn_man_t *nm = realloc(r->man, (size_t)nc * sizeof *nm);
        if (!nm) return -1;
        r->man = nm;
        *cap = nc;
    }
    r->man[r->nman++] = *m;
    return 0;
}

static uint32_t first_out_arc(bgr_pack_t *p, uint32_t node, uint32_t *end);

int bgn_build(bgr_pack_t *p, const bgr_route_t *rt, bgn_route_t *out)
{
    memset(out, 0, sizeof *out);
    out->profile = (int)bgr_info(p)->profile;
    if (rt->narcs < 1) return -1;
    uint32_t *buf = malloc(2 * MAXP * sizeof *buf), *buf2 = malloc(2 * MAXP * sizeof *buf2);
    vec_t xy = {0}, cd = {0}, ct = {0};
    int *arc_start = malloc((size_t)rt->narcs * sizeof *arc_start);   /* polyline index where each arc starts */
    if (!buf || !buf2 || !arc_start) goto fail;

    /* polyline with cumulative distance and time (from the arcs' own length and time) */
    double dist = 0, tim = 0;
    for (int i = 0; i < rt->narcs; i++) {
        uint32_t a = rt->arcs[i];
        int n = bgr_arc_points(p, a, buf, MAXP);
        if (n < 2) { arc_start[i] = xy.n ? xy.n / 2 - 1 : 0; continue; }
        double total = 0;
        for (int k = 0; k + 1 < n; k++) total += seglen(&buf[2 * k], &buf[2 * k + 2]);
        double f0 = i == 0 ? rt->src_frac / 65536.0 : 0, f1 = i == rt->narcs - 1 ? rt->dst_frac / 65536.0 : 1;
        if (rt->narcs == 1 && f1 < f0) f1 = f0;
        double s0 = f0 * total, s1 = f1 * total, alen = bgr_arc_len_dm(p, a), atim = bgr_arc_time_ds(p, a);
        double run = 0;
        int started = 0;
        arc_start[i] = xy.n ? xy.n / 2 - 1 : 0;
        for (int k = 0; k + 1 < n; k++) {
            double l = seglen(&buf[2 * k], &buf[2 * k + 2]);
            double a0 = run, a1 = run + l;
            run = a1;
            if (a1 < s0 || a0 > s1 || l <= 0) continue;
            double t0 = (fmax(a0, s0) - a0) / l, t1 = (fmin(a1, s1) - a0) / l;
            uint32_t px[2][2];
            for (int e = 0; e < 2; e++) {
                double t = e ? t1 : t0;
                px[e][0] = buf[2 * k] + (uint32_t)(int32_t)lround(t * (double)(int32_t)(buf[2 * k + 2] - buf[2 * k]));
                px[e][1] = buf[2 * k + 1] + (uint32_t)(int32_t)lround(t * (double)(int32_t)(buf[2 * k + 3] - buf[2 * k + 1]));
            }
            if (!started) {
                started = 1;
                if (!xy.n) {
                    if (vpush(&xy, px[0][0]) || vpush(&xy, px[0][1]) || vpush(&cd, (uint32_t)lround(dist)) || vpush(&ct, (uint32_t)lround(tim))) goto fail;
                }
            }
            double frac = total > 0 ? (fmin(a1, s1) - fmax(a0, s0)) / total : 0;
            dist += frac * alen;
            tim += frac * atim;
            if (vpush(&xy, px[1][0]) || vpush(&xy, px[1][1]) || vpush(&cd, (uint32_t)lround(dist)) || vpush(&ct, (uint32_t)lround(tim))) goto fail;
        }
    }
    if (xy.n < 4) {                                   /* degenerate: start = end */
        while (xy.n < 4) {
            if (vpush(&xy, xy.n >= 2 ? xy.v[0] : 0) || vpush(&xy, xy.n >= 2 ? xy.v[1] : 0)) goto fail;
        }
        while (cd.n < 2) { if (vpush(&cd, 0) || vpush(&ct, 0)) goto fail; }
    }
    out->xy = xy.v;
    out->cum_dm = cd.v;
    out->cum_ds = ct.v;
    out->npts = xy.n / 2;
    out->total_dm = cd.v[cd.n - 1];
    out->total_ds = rt->time_ds;
    xy.v = cd.v = ct.v = NULL;

    /* maneuvers */
    int cap = 0;
    bgn_man_t m;
    memset(&m, 0, sizeof m);
    m.type = BGN_DEPART;
    m.x = out->xy[0];
    m.y = out->xy[1];
    way_name(p, rt->arcs[0], m.name, sizeof m.name);
    if (push_man(out, &cap, &m)) goto fail;
    for (int i = 0; i + 1 < rt->narcs; i++) {
        uint32_t a = rt->arcs[i], b = rt->arcs[i + 1];
        uint32_t v = bgr_arc_head(p, a), u = bgr_arc_tail(p, a);
        int idx = arc_start[i + 1];
        if (idx < 0 || idx >= out->npts) continue;
        memset(&m, 0, sizeof m);
        m.at_dm = out->cum_dm[idx];
        m.x = out->xy[2 * idx];
        m.y = out->xy[2 * idx + 1];
        double in = arc_in(p, a, buf), outb = arc_out(p, b, buf);
        double turn = norm180(outb - in);
        /* other ways out of v (no U-turn back along a) */
        uint32_t end, alt[16];
        int nalt = 0;
        for (uint32_t c = first_out_arc(p, v, &end); c < end && nalt < 16; c++) {
            if (c == b) continue;
            if (bgr_arc_head(p, c) == u && bgr_arc_way(p, c) == bgr_arc_way(p, a)) continue;
            alt[nalt++] = c;
        }
        if (is_round(p, b) && !is_round(p, a)) {
            /* count exits until the route leaves the roundabout */
            int exits = 0, j = i + 1;
            while (j < rt->narcs && is_round(p, rt->arcs[j])) {
                uint32_t node = bgr_arc_head(p, rt->arcs[j]), e2, c;
                int has_exit = 0;
                for (c = first_out_arc(p, node, &e2); c < e2; c++)
                    if (!is_round(p, c)) has_exit = 1;
                if (has_exit) exits++;
                if (j + 1 < rt->narcs && !is_round(p, rt->arcs[j + 1])) break;
                j++;
            }
            m.type = BGN_ROUNDABOUT;
            m.exit = (uint8_t)(exits ? exits : 1);
            if (j + 1 < rt->narcs) way_name(p, rt->arcs[j + 1], m.name, sizeof m.name);
            if (push_man(out, &cap, &m)) goto fail;
            i = j;                                    /* resume after the exit */
            continue;
        }
        if (is_round(p, a) || nalt == 0) continue;    /* inside a roundabout, or just a bend */
        /* straightest alternative */
        double best_alt = 999;
        uint32_t best_arc = alt[0];
        for (int k = 0; k < nalt; k++) {
            double t = norm180(arc_out(p, alt[k], buf2) - in);
            if (fabs(t) < fabs(best_alt)) { best_alt = t; best_arc = alt[k]; }
        }
        double at = fabs(turn);
        if (is_link(p, b) && !is_link(p, a) && at < 60) m.type = turn - best_alt >= 0 ? BGN_RAMP_RIGHT : BGN_RAMP_LEFT;
        else if (at < 25) {
            /* a fork only when both branches are comparable: staying on the main road while
             * an exit ramp leaves (or the reverse) needs no instruction */
            if (fabs(best_alt) >= 40 || is_link(p, best_arc) != is_link(p, b)) continue;
            m.type = turn >= best_alt ? BGN_KEEP_RIGHT : BGN_KEEP_LEFT;
        } else if (at > 170) m.type = BGN_UTURN;
        else if (turn > 0) m.type = at < 60 ? BGN_SLIGHT_RIGHT : at < 140 ? BGN_RIGHT : BGN_SHARP_RIGHT;
        else m.type = at < 60 ? BGN_SLIGHT_LEFT : at < 140 ? BGN_LEFT : BGN_SHARP_LEFT;
        way_name(p, b, m.name, sizeof m.name);
        /* two turns to the same side a few metres apart (dual carriageway): one maneuver */
        bgn_man_t *prev = &out->man[out->nman - 1];
        int same_side = (m.type >= BGN_SLIGHT_RIGHT && m.type <= BGN_SHARP_RIGHT && prev->type >= BGN_SLIGHT_RIGHT && prev->type <= BGN_SHARP_RIGHT)
                        || (m.type >= BGN_SHARP_LEFT && m.type <= BGN_SLIGHT_LEFT && prev->type >= BGN_SHARP_LEFT && prev->type <= BGN_SLIGHT_LEFT);
        /* "mantenha-se" repetido para o mesmo lado logo em seguida (trevos): um aviso só */
        if (out->nman > 1 && m.type == prev->type && (m.type == BGN_KEEP_LEFT || m.type == BGN_KEEP_RIGHT)
            && m.at_dm - prev->at_dm < 2000) {
            memcpy(prev->name, m.name, sizeof prev->name);
            continue;
        }
        if (out->nman > 1 && same_side && m.at_dm - prev->at_dm < 300) {
            if (prev->type == BGN_LEFT && m.type == BGN_LEFT) prev->type = BGN_UTURN;   /* left + left across a median */
            memcpy(prev->name, m.name, sizeof prev->name);
            continue;
        }
        if (push_man(out, &cap, &m)) goto fail;
    }
    memset(&m, 0, sizeof m);
    m.type = BGN_ARRIVE;
    m.at_dm = out->total_dm;
    m.x = out->xy[2 * (out->npts - 1)];
    m.y = out->xy[2 * (out->npts - 1) + 1];
    if (push_man(out, &cap, &m)) goto fail;
    free(buf);
    free(buf2);
    free(arc_start);
    return 0;
fail:
    free(buf);
    free(buf2);
    free(arc_start);
    free(xy.v);
    free(cd.v);
    free(ct.v);
    bgn_free(out);
    return -1;
}

/* arcs leaving a node: [first, *end) */
static uint32_t first_out_arc(bgr_pack_t *p, uint32_t node, uint32_t *end)
{
    *end = bgr_node_first_out(p, node + 1);
    return bgr_node_first_out(p, node);
}

void bgn_free(bgn_route_t *r)
{
    free(r->xy);
    free(r->cum_dm);
    free(r->cum_ds);
    free(r->man);
    memset(r, 0, sizeof *r);
}

void bgn_update(bgn_route_t *r, uint32_t x, uint32_t y, double speed_kmh, double heading, bgn_state_t *st)
{
    memset(st, 0, sizeof *st);
    if (r->npts < 2) return;
    double lat = atan(sinh(M_PI * (1.0 - 2.0 * y / 4294967296.0)));
    double mpu = 40075016.686 / 4294967296.0 * cos(lat);          /* metres per world unit */
    int s0 = r->seg - 3 < 0 ? 0 : r->seg - 3, s1 = r->seg + 80 > r->npts - 2 ? r->npts - 2 : r->seg + 80;
    double best = 1e30, bt = 0, bd = 0;
    int bs = r->seg;
    for (int s = s0; s <= s1; s++) {
        const uint32_t *a = &r->xy[2 * s], *b = &r->xy[2 * s + 2];
        double ax = (double)(int32_t)(a[0] - x), ay = (double)(int32_t)(a[1] - y);
        double dx = (double)(int32_t)(b[0] - a[0]), dy = (double)(int32_t)(b[1] - a[1]);
        double l2 = dx * dx + dy * dy, t = l2 > 0 ? -(ax * dx + ay * dy) / l2 : 0;
        if (t < 0) t = 0;
        if (t > 1) t = 1;
        double px = ax + t * dx, py = ay + t * dy, d = sqrt(px * px + py * py) * mpu, score = d;
        if (heading >= 0 && speed_kmh > 5 && l2 > 0) {
            double diff = fabs(norm180(bearing(a, b) - heading));
            if (diff > 90) score += 30;
        }
        score += (s < r->seg ? (r->seg - s) * 2.0 : 0);            /* do not jump backwards */
        if (score < best) { best = score; bs = s; bt = t; bd = d; }
    }
    double lim = r->profile == BGR_FOOT ? 25 : r->profile == BGR_BIKE ? 30 : 40;
    st->cross_m = bd;
    if (bd > lim) {
        r->off_count++;
        st->off_route = r->off_count >= 3;
    } else {
        r->off_count = 0;
        r->seg = bs;
    }
    int s = r->seg;
    double t = bd > lim ? 0 : bt;
    st->valid = bd <= lim;
    const uint32_t *a = &r->xy[2 * s], *b = &r->xy[2 * s + 2];
    st->x = a[0] + (uint32_t)(int32_t)lround(t * (double)(int32_t)(b[0] - a[0]));
    st->y = a[1] + (uint32_t)(int32_t)lround(t * (double)(int32_t)(b[1] - a[1]));
    st->progress_dm = r->cum_dm[s] + (uint32_t)lround(t * (r->cum_dm[s + 1] - r->cum_dm[s]));
    st->remain_dm = r->total_dm > st->progress_dm ? r->total_dm - st->progress_dm : 0;
    uint32_t tdone = r->cum_ds[s] + (uint32_t)lround(t * (r->cum_ds[s + 1] - r->cum_ds[s]));
    uint32_t tsum = r->cum_ds[r->npts - 1];
    double scale = tsum ? (double)r->total_ds / tsum : 1;          /* arc times + turn costs */
    st->remain_ds = tsum > tdone ? (uint32_t)lround((tsum - tdone) * scale) : 0;
    st->next = r->nman - 1;
    for (int i = 0; i < r->nman; i++)
        if (r->man[i].at_dm > st->progress_dm + 20) { st->next = i; break; }
    st->next_dm = r->man[st->next].at_dm > st->progress_dm ? r->man[st->next].at_dm - st->progress_dm : 0;
    st->arrived = st->valid && st->remain_dm < 200;
}

static const char *prep(const char *name)
{
    static const char *masc[] = { "Eixo", "Setor", "Acesso", "Anel", "Viaduto", "Túnel", "Balão", "Contorno", "Trevo",
                                  "Elevado", "Corredor", "Caminho", "Largo", "Beco", "Parque", "Boulevard", "Bulevar",
                                  "Ramal", "Retorno", "Complexo", "Conjunto", "Lote", "Jardim", "Passeio", "Calçadão" };
    for (unsigned i = 0; i < sizeof masc / sizeof masc[0]; i++) {
        size_t l = strlen(masc[i]);
        if (!strncmp(name, masc[i], l) && (name[l] == ' ' || !name[l])) return "no";
    }
    return "na";
}

const char *bgn_text(const bgn_man_t *m, char *buf, int n)
{
    static const char *verb[] = {
        [BGN_DEPART] = "Siga", [BGN_STRAIGHT] = "Continue em frente", [BGN_SLIGHT_RIGHT] = "Vire levemente à direita",
        [BGN_RIGHT] = "Vire à direita", [BGN_SHARP_RIGHT] = "Vire acentuadamente à direita", [BGN_UTURN] = "Faça o retorno",
        [BGN_SHARP_LEFT] = "Vire acentuadamente à esquerda", [BGN_LEFT] = "Vire à esquerda",
        [BGN_SLIGHT_LEFT] = "Vire levemente à esquerda", [BGN_KEEP_RIGHT] = "Mantenha-se à direita",
        [BGN_KEEP_LEFT] = "Mantenha-se à esquerda", [BGN_RAMP_RIGHT] = "Pegue o acesso à direita",
        [BGN_RAMP_LEFT] = "Pegue o acesso à esquerda", [BGN_ROUNDABOUT] = "Na rotatória", [BGN_ARRIVE] = "Você chegou ao destino",
    };
    const char *v = m->type <= BGN_ARRIVE ? verb[m->type] : "";
    if (m->type == BGN_ROUNDABOUT) {
        if (m->name[0]) snprintf(buf, (size_t)n, "Na rotatória, pegue a %dª saída para %c %s", m->exit, prep(m->name)[1], m->name);
        else snprintf(buf, (size_t)n, "Na rotatória, pegue a %dª saída", m->exit);
    } else if (m->type == BGN_ARRIVE || !m->name[0]) {
        snprintf(buf, (size_t)n, "%s", m->type == BGN_DEPART ? "Siga em frente" : v);
    } else if (m->type == BGN_DEPART) {
        snprintf(buf, (size_t)n, "Siga pel%c %s", prep(m->name)[1], m->name);
    } else {
        snprintf(buf, (size_t)n, "%s %s %s", v, prep(m->name), m->name);
    }
    return buf;
}

const char *bgn_dist_text(uint32_t dm, char *buf, int n)
{
    uint32_t m = dm / 10;
    uint32_t step = m < 100 ? 10 : m < 500 ? 50 : 100;
    uint32_t r = (m + step / 2) / step * step;
    if (r < 1000) {
        snprintf(buf, (size_t)n, "%u m", r);
    } else if (m < 10000) {
        uint32_t d = (m + 50) / 100;
        snprintf(buf, (size_t)n, "%u,%u km", d / 10, d % 10);
    } else {
        snprintf(buf, (size_t)n, "%u km", (m + 500) / 1000);
    }
    return buf;
}
