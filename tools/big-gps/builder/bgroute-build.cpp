// bgroute-build.cpp - Big GPS route pack builder (runs on the PC).
//
//   bgroute-build IN.osm.pbf OUT.bgr --profile car|moto|bike|foot [--tests FILE]
//
// --tests writes 300 random queries (source arc, target arc, distance in deciseconds)
// computed with a plain Dijkstra on the expanded graph: an independent reference for the
// contraction hierarchy query on the device (bgroute -c).
//
// Builds the road graph of one profile with RoutingKit (OSM loading, turn restrictions,
// contraction hierarchy), expands it by turns (turn restrictions and turn costs become
// part of the graph), and writes the .bgr pack described in src/ui/biggps/bgroute_format.h.
//
// SPDX-License-Identifier: GPL-3.0-or-later
#include <routingkit/contraction_hierarchy.h>
#include <routingkit/dijkstra.h>
#include <routingkit/osm_graph_builder.h>
#include <routingkit/osm_profile.h>
#include <routingkit/strongly_connected_component.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <random>
#include <string>
#include <unordered_map>
#include <vector>

extern "C" {
#include "../../../src/ui/biggps/bgroute_format.h"
}

using namespace RoutingKit;

namespace {

struct WayInfo {
    unsigned speed = 0;        // km/h
    uint32_t info = 0;         // class | flags << 8
    std::string name, ref;
};

std::vector<WayInfo> ways;
int profile = -1;

bool is(const char *v, const char *s) { return v && !std::strcmp(v, s); }

uint32_t way_class(const TagMap &t, uint32_t *flags)
{
    const char *h = t["highway"];
    if (is(t["route"], "ferry")) return BGR_W_FERRY;
    if (!h) return BGR_W_OTHER;
    std::string s(h);
    if (s.size() > 5 && s.compare(s.size() - 5, 5, "_link") == 0) { s.resize(s.size() - 5); *flags |= BGR_WF_LINK; }
    if (s == "motorway") return BGR_W_MOTORWAY;
    if (s == "trunk") return BGR_W_TRUNK;
    if (s == "primary") return BGR_W_PRIMARY;
    if (s == "secondary") return BGR_W_SECONDARY;
    if (s == "tertiary") return BGR_W_TERTIARY;
    if (s == "residential" || s == "unclassified" || s == "living_street" || s == "road") return BGR_W_RESIDENTIAL;
    if (s == "service") return BGR_W_SERVICE;
    if (s == "track") return BGR_W_TRACK;
    if (s == "cycleway") return BGR_W_CYCLEWAY;
    if (s == "footway" || s == "pedestrian") return BGR_W_FOOTWAY;
    if (s == "steps") return BGR_W_STEPS;
    if (s == "path" || s == "bridleway") return BGR_W_PATH;
    return BGR_W_OTHER;
}

bool used(uint64_t id, const TagMap &t)
{
    switch (profile) {
    case BGR_CAR: return is_osm_way_used_by_cars(id, t);
    case BGR_MOTO:
        if (is(t["motorcycle"], "no") || (is(t["motor_vehicle"], "no") && !is(t["motorcycle"], "yes"))) return false;
        return is_osm_way_used_by_cars(id, t) || is(t["motorcycle"], "yes") || is(t["motorcycle"], "designated");
    case BGR_BIKE: return is_osm_way_used_by_bicycles(id, t);
    case BGR_FOOT: return is_osm_way_used_by_pedestrians(id, t);
    }
    return false;
}

unsigned speed_of(uint64_t id, const TagMap &t, uint32_t cls)
{
    switch (profile) {
    case BGR_CAR:
    case BGR_MOTO: {
        unsigned s = get_osm_way_speed(id, t);
        return std::max(5u, std::min(s, 120u));
    }
    case BGR_BIKE:
        if (cls == BGR_W_STEPS) return 3;                 // carrying the bicycle
        if (cls == BGR_W_FOOTWAY) return 10;
        if (cls == BGR_W_TRACK || cls == BGR_W_PATH) return 12;
        if (cls == BGR_W_TRUNK || cls == BGR_W_PRIMARY) return 15;
        return 16;
    case BGR_FOOT:
        return cls == BGR_W_STEPS ? 2 : 5;
    }
    return 5;
}

OSMWayDirectionCategory way_cb(uint64_t id, unsigned rid, const TagMap &t)
{
    if (rid >= ways.size()) ways.resize(rid + 1);
    WayInfo &w = ways[rid];
    uint32_t flags = 0;
    uint32_t cls = way_class(t, &flags);
    if (is(t["junction"], "roundabout") || is(t["junction"], "circular")) flags |= BGR_WF_ROUNDABOUT;
    if (is(t["toll"], "yes")) flags |= BGR_WF_TOLL;
    if (t["tunnel"] && !is(t["tunnel"], "no")) flags |= BGR_WF_TUNNEL;
    if (t["bridge"] && !is(t["bridge"], "no")) flags |= BGR_WF_BRIDGE;
    w.info = cls | flags << 8;
    w.speed = speed_of(id, t, cls);
    if (const char *n = t["name"]) w.name = n;
    if (const char *r = t["ref"]) w.ref = r;
    switch (profile) {
    case BGR_CAR:
    case BGR_MOTO: return get_osm_car_direction_category(id, t);
    case BGR_BIKE: return get_osm_bicycle_direction_category(id, t);
    default: return OSMWayDirectionCategory::open_in_both;
    }
}

void put_var(std::string &o, uint64_t v)
{
    while (v >= 0x80) { o.push_back((char)(v | 0x80)); v >>= 7; }
    o.push_back((char)v);
}
void put_zz32(std::string &o, int64_t v)
{
    int32_t x = (int32_t)v;
    put_var(o, (uint32_t)((uint32_t)x << 1) ^ (uint32_t)(x >> 31));
}

uint32_t world_x(double lon) { return (uint32_t)std::llround(std::min(std::max((lon + 180.0) / 360.0 * 4294967296.0, 0.0), 4294967295.0)); }
uint32_t world_y(double lat)
{
    double r = lat * M_PI / 180.0;
    double y = (1.0 - std::log(std::tan(r) + 1.0 / std::cos(r)) / M_PI) / 2.0 * 4294967296.0;
    return (uint32_t)std::llround(std::min(std::max(y, 0.0), 4294967295.0));
}

} // namespace

int main(int argc, char **argv)
{
    const char *tests = argc == 7 && !std::strcmp(argv[5], "--tests") ? argv[6] : nullptr;
    if ((argc != 5 && !tests) || std::strcmp(argv[3], "--profile")) {
        std::fprintf(stderr, "uso: %s IN.osm.pbf OUT.bgr --profile car|moto|bike|foot [--tests ARQ]\n", argv[0]);
        return 2;
    }
    const char *names[] = {"car", "moto", "bike", "foot"};
    for (int i = 0; i < BGR_PROFILES; i++)
        if (!std::strcmp(argv[4], names[i])) profile = i;
    if (profile < 0) { std::fprintf(stderr, "perfil desconhecido: %s\n", argv[4]); return 2; }
    time_t t0 = time(nullptr);
    auto log = [](const std::string &) {};

    OSMRoutingIDMapping mapping = load_osm_id_mapping_from_pbf(
        argv[1], nullptr, [](uint64_t id, const TagMap &t) { return used(id, t); }, log);
    auto restrictions = [&](uint64_t rid, const std::vector<OSMRelationMember> &m, const TagMap &t, std::function<void(OSMTurnRestriction)> cb) {
        if (profile == BGR_CAR || profile == BGR_MOTO) decode_osm_car_turn_restrictions(rid, m, t, cb, log);
    };
    OSMRoutingGraph g = load_osm_routing_graph_from_pbf(argv[1], mapping, way_cb, restrictions, log, false, OSMRoadGeometry::uncompressed);
    mapping = OSMRoutingIDMapping();
    unsigned n = g.node_count(), m = g.arc_count();
    std::fprintf(stderr, "grafo: %u nós, %u arcos, %zu vias, %zu proibições de conversão (%ld s)\n",
                 n, m, ways.size(), g.forbidden_turn_from_arc.size(), (long)(time(nullptr) - t0));
    if (ways.size() < 1) ways.resize(1);

    // renumber nodes along a Hilbert curve; arcs follow their tail (locality on the SD card)
    {
        auto hilbert = [](uint32_t x, uint32_t y) {
            uint64_t d = 0;
            for (uint32_t s = 1u << 15; s > 0; s >>= 1) {
                uint32_t rx = (x & s) > 0, ry = (y & s) > 0;
                d += (uint64_t)s * s * ((3 * rx) ^ ry);
                if (!ry) {
                    if (rx) { x = s - 1 - x; y = s - 1 - y; }
                    std::swap(x, y);
                }
            }
            return d;
        };
        std::vector<uint64_t> key(n);
        for (unsigned u = 0; u < n; u++) key[u] = hilbert(world_x(g.longitude[u]) >> 16, world_y(g.latitude[u]) >> 16);
        std::vector<unsigned> perm(n), inv(n);
        for (unsigned u = 0; u < n; u++) perm[u] = u;
        std::stable_sort(perm.begin(), perm.end(), [&](unsigned a, unsigned b) { return key[a] < key[b]; });
        for (unsigned i = 0; i < n; i++) inv[perm[i]] = i;
        OSMRoutingGraph r;
        r.first_out.assign(n + 1, 0);
        r.latitude.resize(n);
        r.longitude.resize(n);
        std::vector<unsigned> arc_map(m);
        r.first_modelling_node.push_back(0);
        for (unsigned i = 0; i < n; i++) {
            unsigned u = perm[i];
            r.latitude[i] = g.latitude[u];
            r.longitude[i] = g.longitude[u];
            for (unsigned a = g.first_out[u]; a < g.first_out[u + 1]; a++) {
                arc_map[a] = (unsigned)r.head.size();
                r.head.push_back(inv[g.head[a]]);
                r.way.push_back(g.way[a]);
                r.geo_distance.push_back(g.geo_distance[a]);
                for (unsigned k = g.first_modelling_node[a]; k < g.first_modelling_node[a + 1]; k++) {
                    r.modelling_node_latitude.push_back(g.modelling_node_latitude[k]);
                    r.modelling_node_longitude.push_back(g.modelling_node_longitude[k]);
                }
                r.first_modelling_node.push_back((unsigned)r.modelling_node_latitude.size());
            }
            r.first_out[i + 1] = (unsigned)r.head.size();
        }
        for (size_t i = 0; i < g.forbidden_turn_from_arc.size(); i++) {
            r.forbidden_turn_from_arc.push_back(arc_map[g.forbidden_turn_from_arc[i]]);
            r.forbidden_turn_to_arc.push_back(arc_map[g.forbidden_turn_to_arc[i]]);
        }
        g = std::move(r);
    }

    // tail of each arc, travel time
    std::vector<unsigned> tail(m);
    for (unsigned u = 0; u < n; u++)
        for (unsigned a = g.first_out[u]; a < g.first_out[u + 1]; a++) tail[a] = u;
    std::vector<uint32_t> len_dm(m), time_ds(m);
    for (unsigned a = 0; a < m; a++) {
        unsigned sp = ways[g.way[a]].speed ? ways[g.way[a]].speed : 5;
        len_dm[a] = g.geo_distance[a] * 10;
        time_ds[a] = std::max(1u, (unsigned)((uint64_t)g.geo_distance[a] * 36 / sp));
    }

    // geometry: modelling nodes between tail and head, in arc direction
    auto lat_of = [&](unsigned k) { return (double)g.modelling_node_latitude[k]; };
    auto lon_of = [&](unsigned k) { return (double)g.modelling_node_longitude[k]; };
    // incoming / outgoing direction at the head / tail (local metres)
    auto bearing = [&](double la1, double lo1, double la2, double lo2) {
        double dx = (lo2 - lo1) * std::cos(la1 * M_PI / 180.0), dy = la2 - la1;
        return std::atan2(dx, dy) * 180.0 / M_PI;   // 0 = north, clockwise
    };
    auto in_bearing = [&](unsigned a) {
        unsigned v = g.head[a];
        unsigned f = g.first_modelling_node[a], l = g.first_modelling_node[a + 1];
        double la = l > f ? lat_of(l - 1) : g.latitude[tail[a]], lo = l > f ? lon_of(l - 1) : g.longitude[tail[a]];
        return bearing(la, lo, g.latitude[v], g.longitude[v]);
    };
    auto out_bearing = [&](unsigned b) {
        unsigned u = tail[b];
        unsigned f = g.first_modelling_node[b], l = g.first_modelling_node[b + 1];
        double la = l > f ? lat_of(f) : g.latitude[g.head[b]], lo = l > f ? lon_of(f) : g.longitude[g.head[b]];
        return bearing(g.latitude[u], g.longitude[u], la, lo);
    };

    // forbidden turns: sorted (from, to) pairs
    std::vector<std::pair<unsigned, unsigned>> forbidden(g.forbidden_turn_from_arc.size());
    for (size_t i = 0; i < forbidden.size(); i++) forbidden[i] = {g.forbidden_turn_from_arc[i], g.forbidden_turn_to_arc[i]};
    std::sort(forbidden.begin(), forbidden.end());

    // turn-expanded graph
    std::vector<unsigned> ex_tail, ex_head, ex_w;
    unsigned uturns = 0, banned = 0;
    for (unsigned a = 0; a < m; a++) {
        unsigned u = tail[a], v = g.head[a];
        unsigned deg = g.first_out[v + 1] - g.first_out[v];
        double bin = in_bearing(a);
        for (unsigned b = g.first_out[v]; b < g.first_out[v + 1]; b++) {
            bool uturn = g.head[b] == u && (g.first_modelling_node[b + 1] - g.first_modelling_node[b]) == (g.first_modelling_node[a + 1] - g.first_modelling_node[a]) && g.way[a] == g.way[b];
            if (uturn && deg > 1 && profile != BGR_FOOT) { uturns++; continue; }
            if (std::binary_search(forbidden.begin(), forbidden.end(), std::make_pair(a, b))) { banned++; continue; }
            double turn = out_bearing(b) - bin;
            while (turn > 180) turn -= 360;
            while (turn < -180) turn += 360;
            unsigned cost = 0;                          // deciseconds
            if (profile == BGR_CAR || profile == BGR_MOTO) {
                if (uturn) cost = 300;
                else if (turn < -30) cost = turn < -120 ? 130 : 80;   // left: crosses traffic (right-hand traffic)
                else if (turn > 30) cost = turn > 120 ? 80 : 30;
            } else if (profile == BGR_BIKE) {
                if (uturn) cost = 100;
                else if (turn < -30) cost = 30;
                else if (turn > 30) cost = 10;
            }
            ex_tail.push_back(a);
            ex_head.push_back(b);
            ex_w.push_back(cost + time_ds[b]);
        }
    }
    std::fprintf(stderr, "grafo expandido: %zu arestas (%u retornos e %u conversões proibidas removidos)\n", ex_head.size(), uturns, banned);

    /* bicycle and foot have no turn restrictions: hierarchy over nodes and arcs (much smaller) */
    const bool node_mode = profile == BGR_BIKE || profile == BGR_FOOT;
    const unsigned cn = node_mode ? n : m;              // CH nodes
    if (node_mode) {
        ex_tail = tail;
        ex_head.assign(g.head.begin(), g.head.end());
        ex_w = time_ds;
        std::fprintf(stderr, "modo por nós: CH sobre %u nós e %u arcos\n", n, m);
    }
    ContractionHierarchy ch = ContractionHierarchy::build(cn, ex_tail, ex_head, ex_w,
        [](const std::string &s) { (void)s; });
    std::fprintf(stderr, "CH: %zu arcos para frente, %zu para trás (%ld s)\n", ch.forward.head.size(), ch.backward.head.size(), (long)(time(nullptr) - t0));
    // the section table uses 32-bit offsets: the whole file must stay below 4 GB (FAT32 too)

    std::vector<uint32_t> chf_first, chb_first;
    std::string chf_data, chb_data;
    uint32_t ch_arcs[2] = {0, 0};
    // CH node ids: the lower part keeps the spatial order of the arcs, the top of the
    // hierarchy goes to the end in rank order (BGR_TOP_PERMILLE, default 20 = 2 %)
    std::vector<unsigned> ch_id(cn), ch_node(cn);        // CH input node -> id, id -> node
    {
        const char *e = getenv("BGR_TOP_PERMILLE");
        unsigned permille = e ? (unsigned)atoi(e) : 20;
        unsigned top = (unsigned)((uint64_t)cn * permille / 1000);
        std::vector<unsigned> ids(cn);
        for (unsigned a = 0; a < cn; a++) ids[a] = a;
        std::stable_sort(ids.begin(), ids.end(), [&](unsigned a, unsigned b) {
            bool ta = ch.rank[a] >= cn - top, tb = ch.rank[b] >= cn - top;
            if (ta != tb) return tb;
            return ta ? ch.rank[a] < ch.rank[b] : a < b;
        });
        for (unsigned i = 0; i < cn; i++) { ch_node[i] = ids[i]; ch_id[ids[i]] = i; }
    }
    {
        ContractionHierarchy::Side *side[2] = {&ch.forward, &ch.backward};
        std::vector<unsigned> amap[2];
        ContractionHierarchy::Side out[2];   /* new layout: first_out, head, weight */
        for (int sd = 0; sd < 2; sd++) {
            ContractionHierarchy::Side &S = *side[sd];
            amap[sd].assign(S.head.size(), 0);
            out[sd].first_out.assign(cn + 1, 0);
            for (unsigned id = 0; id < cn; id++) {
                unsigned r = ch.rank[ch_node[id]];
                for (unsigned arc = S.first_out[r]; arc < S.first_out[r + 1]; arc++) {
                    amap[sd][arc] = (unsigned)out[sd].head.size();
                    out[sd].head.push_back(ch_id[ch.order[S.head[arc]]]);
                    out[sd].weight.push_back(S.weight[arc]);
                }
                out[sd].first_out[id + 1] = (unsigned)out[sd].head.size();
            }
        }
        /* format 2: varint blocks; shortcuts keep only their middle node (the CH node that
         * owns the shortcut's backward part) */
        std::vector<unsigned> owner_b(ch.backward.head.size());
        for (unsigned r = 0; r < cn; r++)
            for (unsigned arc = ch.backward.first_out[r]; arc < ch.backward.first_out[r + 1]; arc++) owner_b[arc] = ch_id[ch.order[r]];
        for (int sd = 0; sd < 2; sd++) {
            ContractionHierarchy::Side &S = *side[sd];
            size_t na = S.head.size();
            std::vector<uint32_t> &first = sd ? chb_first : chf_first;
            std::string &data = sd ? chb_data : chf_data;
            first.assign(cn + 1, 0);
            /* entries in the order of out[sd] (already per new id); aux from the old arc */
            std::vector<unsigned> old_of_new(na);
            for (size_t arc = 0; arc < na; arc++) old_of_new[amap[sd][arc]] = (unsigned)arc;
            for (unsigned id = 0; id < cn; id++) {
                first[id] = (uint32_t)data.size();
                for (unsigned na_ = out[sd].first_out[id]; na_ < out[sd].first_out[id + 1]; na_++) {
                    unsigned arc = old_of_new[na_];
                    bool orig = S.is_shortcut_an_original_arc.is_set(arc);
                    put_zz32(data, (int64_t)out[sd].head[na_] - id);
                    put_var(data, (uint64_t)out[sd].weight[na_] << 1 | (orig ? 1 : 0));
                    if (orig) put_var(data, S.shortcut_first_arc[arc]);          // expanded edge id
                    else put_zz32(data, (int64_t)owner_b[S.shortcut_first_arc[arc]] - id);   // middle node
                }
            }
            first[cn] = (uint32_t)data.size();
            if (data.size() > 0xFFFFFFF0ull) { std::fprintf(stderr, "CH grande demais\n"); return 1; }
        }
        ch_arcs[0] = (uint32_t)ch.forward.head.size();
        ch_arcs[1] = (uint32_t)ch.backward.head.size();
        ch = ContractionHierarchy();                                 // frees the RoutingKit arrays
        (void)out;
    }

    // largest strongly connected component of the expanded graph: only its arcs are snappable
    std::vector<unsigned> ex_first(cn + 1, 0);
    for (unsigned e = 0; e < ex_tail.size(); e++) ex_first[ex_tail[e] + 1]++;
    for (unsigned a = 0; a < cn; a++) ex_first[a + 1] += ex_first[a];
    std::vector<bool> big_ch = compute_largest_strongly_connected_component(ex_first, ex_head);   // ex_tail is sorted
    std::vector<bool> in_big(m);
    for (unsigned a = 0; a < m; a++) in_big[a] = node_mode ? big_ch[tail[a]] && big_ch[g.head[a]] : big_ch[a];
    std::fprintf(stderr, "maior componente: %ld de %u arcos\n", (long)std::count(in_big.begin(), in_big.end(), true), m);
    if (tests) {
        FILE *tf = std::fopen(tests, "w");
        if (!tf) { std::perror(tests); return 1; }
        std::vector<unsigned> big_arcs;                  // CH input nodes of the big component
        for (unsigned a = 0; a < cn; a++) if (big_ch[a]) big_arcs.push_back(a);
        std::mt19937 rng(20261121);
        Dijkstra dij(ex_first, ex_tail, ex_head);
        for (int q = 0; q < 300; q++) {
            unsigned s0 = big_arcs[rng() % big_arcs.size()], t0 = big_arcs[rng() % big_arcs.size()];
            dij.reset().add_source(s0);
            unsigned d = inf_weight;
            while (!dij.is_finished()) {
                auto r = dij.settle(ScalarGetWeight(ex_w));
                if (r.node == t0) { d = r.distance; break; }
            }
            std::fprintf(tf, "%u %u %u\n", s0, t0, d);
        }
        std::fclose(tf);
        std::fprintf(stderr, "%s: 300 consultas de referência (Dijkstra)\n", tests);
    }

    // coordinates
    std::vector<uint32_t> nx(n), ny(n), gx, gy;
    for (unsigned u = 0; u < n; u++) { nx[u] = world_x(g.longitude[u]); ny[u] = world_y(g.latitude[u]); }
    std::vector<uint32_t> geom_first(m + 1);
    for (unsigned a = 0; a < m; a++) {
        geom_first[a] = (uint32_t)gx.size();
        for (unsigned k = g.first_modelling_node[a]; k < g.first_modelling_node[a + 1]; k++) {
            gx.push_back(world_x(lon_of(k)));
            gy.push_back(world_y(lat_of(k)));
        }
    }
    geom_first[m] = (uint32_t)gx.size();

    // snapping grid (~300 m cells)
    const uint32_t shift = 15;
    uint32_t x0 = UINT32_MAX, y0 = UINT32_MAX, x1 = 0, y1 = 0;
    for (unsigned a = 0; a < m; a++) {
        if (!in_big[a]) continue;
        for (unsigned u : {tail[a], g.head[a]}) { x0 = std::min(x0, nx[u]); y0 = std::min(y0, ny[u]); x1 = std::max(x1, nx[u]); y1 = std::max(y1, ny[u]); }
    }
    x0 &= ~((1u << shift) - 1);
    y0 &= ~((1u << shift) - 1);
    uint32_t gw = ((x1 - x0) >> shift) + 1, gh = ((y1 - y0) >> shift) + 1;
    if ((uint64_t)gw * gh >= 0xFFFFFFFFull) { std::fprintf(stderr, "região grande demais para a grade\n"); return 1; }
    std::vector<std::pair<uint32_t, uint32_t>> cell_arc;          // (cell, arc), sorted later
    for (unsigned a = 0; a < m; a++) {
        if (!in_big[a]) continue;
        uint32_t ax0 = std::min(nx[tail[a]], nx[g.head[a]]), ax1 = std::max(nx[tail[a]], nx[g.head[a]]);
        uint32_t ay0 = std::min(ny[tail[a]], ny[g.head[a]]), ay1 = std::max(ny[tail[a]], ny[g.head[a]]);
        for (uint32_t k = geom_first[a]; k < geom_first[a + 1]; k++) {
            ax0 = std::min(ax0, gx[k]); ax1 = std::max(ax1, gx[k]);
            ay0 = std::min(ay0, gy[k]); ay1 = std::max(ay1, gy[k]);
        }
        for (uint32_t cy = (ay0 - y0) >> shift; cy <= (ay1 - y0) >> shift && cy < gh; cy++)
            for (uint32_t cx = (ax0 - x0) >> shift; cx <= (ax1 - x0) >> shift && cx < gw; cx++)
                cell_arc.push_back({cy * gw + cx, a});
    }
    std::sort(cell_arc.begin(), cell_arc.end());
    std::vector<uint32_t> grid_cells, grid_arcs;
    for (size_t i = 0; i < cell_arc.size(); i++) {
        if (!i || cell_arc[i].first != cell_arc[i - 1].first) { grid_cells.push_back(cell_arc[i].first); grid_cells.push_back((uint32_t)grid_arcs.size()); }
        grid_arcs.push_back(cell_arc[i].second);
    }
    grid_cells.push_back(0xFFFFFFFFu);
    grid_cells.push_back((uint32_t)grid_arcs.size());
    std::vector<std::pair<uint32_t, uint32_t>>().swap(cell_arc);

    // strings and way tables
    std::string strings;
    std::unordered_map<std::string, uint32_t> sid;
    auto intern = [&](const std::string &s) -> uint32_t {
        if (s.empty()) return 0xFFFFFFFFu;
        auto it = sid.find(s);
        if (it != sid.end()) return it->second;
        uint32_t o = (uint32_t)strings.size();
        strings += s;
        strings.push_back('\0');
        sid[s] = o;
        return o;
    };
    std::vector<uint32_t> way_name(ways.size()), way_ref(ways.size()), way_info(ways.size());
    for (size_t w = 0; w < ways.size(); w++) { way_name[w] = intern(ways[w].name); way_ref[w] = intern(ways[w].ref); way_info[w] = ways[w].info; }
    while (strings.size() % 4) strings.push_back('\0');

    while (chf_data.size() % 4) chf_data.push_back('\0');
    while (chb_data.size() % 4) chb_data.push_back('\0');

    // write
    FILE *out = std::fopen(argv[2], "wb");
    if (!out) { std::perror(argv[2]); return 1; }
    bgr_header_t h{};
    std::fwrite(&h, sizeof h, 1, out);
    uint64_t pos = sizeof h;
    auto sec = [&](int id, const void *data, size_t bytes) {
        if (pos + bytes > 0xFFFFFFF0ull) { std::fprintf(stderr, "arquivo de rotas passaria de 4 GB\n"); std::exit(1); }
        h.sec[id].off = (uint32_t)pos;
        h.sec[id].len = (uint32_t)bytes;
        if (bytes) std::fwrite(data, 1, bytes, out);
        pos += bytes;
    };
    auto vec = [&](int id, const std::vector<uint32_t> &v) { sec(id, v.data(), v.size() * 4); };
    auto uvec = [&](int id, const std::vector<unsigned> &v) { sec(id, v.data(), v.size() * 4); };
    static_assert(sizeof(unsigned) == 4, "unsigned must be 32 bits");
    vec(BGR_S_NODE_X, nx); vec(BGR_S_NODE_Y, ny);
    uvec(BGR_S_FIRST_OUT, g.first_out); uvec(BGR_S_ARC_TAIL, tail); uvec(BGR_S_ARC_HEAD, g.head); uvec(BGR_S_ARC_WAY, g.way);
    vec(BGR_S_ARC_LEN, len_dm); vec(BGR_S_ARC_TIME, time_ds);
    vec(BGR_S_GEOM_FIRST, geom_first); vec(BGR_S_GEOM_X, gx); vec(BGR_S_GEOM_Y, gy);
    vec(BGR_S_WAY_NAME, way_name); vec(BGR_S_WAY_REF, way_ref); vec(BGR_S_WAY_INFO, way_info);
    sec(BGR_S_STRINGS, strings.data(), strings.size());
    if (node_mode) sec(BGR_S_EX_HEAD, nullptr, 0);
    else uvec(BGR_S_EX_HEAD, ex_head);
    uvec(BGR_S_CH_ID, ch_id);
    vec(BGR_S_CHF_FIRST, chf_first); sec(BGR_S_CHF_DATA, chf_data.data(), chf_data.size());
    vec(BGR_S_CHB_FIRST, chb_first); sec(BGR_S_CHB_DATA, chb_data.data(), chb_data.size());
    vec(BGR_S_GRID_CELLS, grid_cells); vec(BGR_S_GRID_ARCS, grid_arcs);
    h.magic = BGR_MAGIC; h.version = BGR_VERSION; h.profile = (uint32_t)profile;
    h.nodes = n; h.arcs = m; h.ways = (uint32_t)ways.size(); h.geom = (uint32_t)gx.size();
    h.ex_edges = node_mode ? 0 : (uint32_t)ex_head.size(); h.ch_fwd = ch_arcs[0]; h.ch_bwd = ch_arcs[1];
    h.flags = node_mode ? BGR_F_NODE : 0;
    h.grid_x0 = x0; h.grid_y0 = y0; h.grid_shift = shift; h.grid_w = gw; h.grid_h = gh;
    h.created = (uint32_t)time(nullptr);
    std::fseek(out, 0, SEEK_SET);
    std::fwrite(&h, sizeof h, 1, out);
    if (std::fclose(out)) { std::perror(argv[2]); return 1; }
    std::fprintf(stderr, "%s: %llu bytes em %ld s\n", argv[2], (unsigned long long)pos, (long)(time(nullptr) - t0));
    return 0;
}
