// bgmap-build.cpp - Big GPS map builder (runs on the PC).
//
//   bgmap-build IN.osm.pbf OUT.bgm
//
// Reads OpenStreetMap data with libosmium, keeps what the Big GPS map draws (roads, water,
// land use, buildings, places), projects to 32-bit Web Mercator, simplifies per data level,
// clips into slippy tiles and writes the .bgm pack described in src/biggps/bgmap_format.h.
// All the heavy work (projection, multipolygons, simplification, clipping) happens here so
// the device (ARM1176 without FPU) only decodes integers.
//
// SPDX-License-Identifier: GPL-3.0-or-later
#include <osmium/area/assembler.hpp>
#include <osmium/area/multipolygon_manager.hpp>
#include <osmium/handler.hpp>
#include <osmium/handler/node_locations_for_ways.hpp>
#include <osmium/index/map/flex_mem.hpp>
#include <osmium/io/pbf_input.hpp>
#include <osmium/osm/area.hpp>
#include <osmium/tags/tags_filter.hpp>
#include <osmium/visitor.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <map>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

extern "C" {
#include "../../../src/biggps/bgmap_format.h"
#include "../../../src/biggps/bgkinds.h"
}

namespace {

struct Pt { int64_t x, y; };
using Ring = std::vector<Pt>;

struct Feat {
    uint8_t kind, flags;
    std::string name;
    std::vector<Ring> rings;     // line: 1 ring; point: 1 ring of 1 point
};

const int LEVELS[] = {5, 8, 11, 14};
// zoom at which each level's simplification and minimum area are chosen (inside its display range)
const int DETAIL_ZOOM[] = {6, 9, 12, 17};
const int NLEVELS = 4;

Pt project(double lon, double lat)
{
    const double W = 4294967296.0;
    if (lat > 85.0511) lat = 85.0511;
    if (lat < -85.0511) lat = -85.0511;
    double r = lat * M_PI / 180.0;
    double x = (lon + 180.0) / 360.0 * W;
    double y = (1.0 - std::log(std::tan(r) + 1.0 / std::cos(r)) / M_PI) / 2.0 * W;
    return {(int64_t)std::llround(std::min(std::max(x, 0.0), W - 1)), (int64_t)std::llround(std::min(std::max(y, 0.0), W - 1))};
}

bool is(const char *v, const char *s) { return v && !std::strcmp(v, s); }

int area_kind(const osmium::TagList &t)
{
    const char *v;
    if (t.has_key("building") && !is(t["building"], "no")) return BGK_A_BUILDING;
    if ((v = t["natural"])) {
        if (is(v, "water")) return BGK_A_WATER;
        if (is(v, "wood") || is(v, "scrub")) return BGK_A_FOREST;
        if (is(v, "wetland")) return BGK_A_WETLAND;
        if (is(v, "beach") || is(v, "sand") || is(v, "bare_rock")) return BGK_A_SAND;
        if (is(v, "grassland") || is(v, "heath")) return BGK_A_PARK;
    }
    if (is(t["waterway"], "riverbank") || is(t["waterway"], "dock")) return BGK_A_WATER;
    if ((v = t["landuse"])) {
        if (is(v, "reservoir") || is(v, "basin")) return BGK_A_WATER;
        if (is(v, "residential")) return BGK_A_RESIDENTIAL;
        if (is(v, "commercial") || is(v, "retail")) return BGK_A_COMMERCIAL;
        if (is(v, "industrial") || is(v, "railway") || is(v, "quarry")) return BGK_A_INDUSTRIAL;
        if (is(v, "farmland") || is(v, "farmyard") || is(v, "orchard") || is(v, "meadow") || is(v, "plant_nursery")) return BGK_A_FARM;
        if (is(v, "forest")) return BGK_A_FOREST;
        if (is(v, "grass") || is(v, "recreation_ground") || is(v, "cemetery") || is(v, "village_green")) return BGK_A_PARK;
    }
    if ((v = t["leisure"])) {
        if (is(v, "park") || is(v, "garden") || is(v, "golf_course") || is(v, "pitch") || is(v, "nature_reserve") || is(v, "playground")) return BGK_A_PARK;
    }
    if (is(t["aeroway"], "aerodrome")) return BGK_A_AEROWAY;
    if (is(t["amenity"], "parking")) return BGK_A_PARKING;
    if (is(t["highway"], "pedestrian") || is(t["place"], "square")) return BGK_A_PEDESTRIAN;
    return BGK_NONE;
}

int line_kind(const osmium::TagList &t, uint8_t *flags)
{
    const char *v;
    if ((v = t["highway"])) {
        std::string s(v);
        bool link = s.size() > 5 && s.compare(s.size() - 5, 5, "_link") == 0;
        if (link) { s.resize(s.size() - 5); *flags |= BGM_F_LINK; }
        if (s == "motorway") return BGK_L_MOTORWAY;
        if (s == "trunk") return BGK_L_TRUNK;
        if (s == "primary") return BGK_L_PRIMARY;
        if (s == "secondary") return BGK_L_SECONDARY;
        if (s == "tertiary") return BGK_L_TERTIARY;
        if (s == "residential" || s == "unclassified" || s == "living_street" || s == "road") return BGK_L_RESIDENTIAL;
        if (s == "service") return BGK_L_SERVICE;
        if (s == "track") return BGK_L_TRACK;
        if (s == "footway" || s == "path" || s == "cycleway" || s == "pedestrian" || s == "steps" || s == "bridleway") return BGK_L_PATH;
        return BGK_NONE;
    }
    if ((v = t["railway"]) && (is(v, "rail") || is(v, "subway") || is(v, "light_rail"))) return BGK_L_RAIL;
    if ((v = t["waterway"])) {
        if (is(v, "river")) return BGK_L_RIVER;
        if (is(v, "stream") || is(v, "canal") || is(v, "drain")) return BGK_L_STREAM;
    }
    if (is(t["route"], "ferry")) return BGK_L_FERRY;
    return BGK_NONE;
}

int place_kind(const osmium::TagList &t)
{
    const char *v = t["place"];
    if (!v) return BGK_NONE;
    if (is(v, "city")) return BGK_P_CITY;
    if (is(v, "town")) return BGK_P_TOWN;
    if (is(v, "suburb") || is(v, "quarter")) return BGK_P_SUBURB;
    if (is(v, "village")) return BGK_P_VILLAGE;
    if (is(v, "neighbourhood") || is(v, "hamlet") || is(v, "locality")) return BGK_P_NEIGHBOURHOOD;
    return BGK_NONE;
}

std::string feature_name(const osmium::TagList &t, bool road)
{
    const char *n = t["name"];
    if (n && *n) return n;
    if (road && (n = t["ref"]) && *n) return n;
    return {};
}

std::vector<Feat> feats;
std::unordered_set<osmium::object_id_type> boundary_ways;

class Collect : public osmium::handler::Handler {
public:
    void node(const osmium::Node &n)
    {
        int k = place_kind(n.tags());
        if (!k || !n.location().valid()) return;
        Feat f{(uint8_t)k, BGM_F_POINT, feature_name(n.tags(), false), {}};
        if (f.name.empty()) return;
        f.flags |= BGM_F_NAME;
        f.rings.push_back({project(n.location().lon(), n.location().lat())});
        feats.push_back(std::move(f));
    }

    void way(const osmium::Way &w)
    {
        uint8_t flags = BGM_F_LINE;
        int k = line_kind(w.tags(), &flags);
        if (!k && boundary_ways.count(w.id())) k = BGK_L_BOUNDARY;
        if (!k) return;
        if (w.tags().has_tag("area", "yes")) return;
        const char *v;
        if ((v = w.tags()["bridge"]) && !is(v, "no")) flags |= BGM_F_BRIDGE;
        if ((v = w.tags()["tunnel"]) && !is(v, "no")) flags |= BGM_F_TUNNEL;
        if (is(w.tags()["oneway"], "yes") || k == BGK_L_MOTORWAY) flags |= BGM_F_ONEWAY;
        Feat f{(uint8_t)k, flags, k == BGK_L_BOUNDARY ? std::string() : feature_name(w.tags(), k >= BGK_L_PATH), {}};
        if (!f.name.empty()) f.flags |= BGM_F_NAME;
        Ring r;
        for (const auto &nr : w.nodes())
            if (nr.location().valid()) r.push_back(project(nr.location().lon(), nr.location().lat()));
        if (r.size() < 2) return;
        f.rings.push_back(std::move(r));
        feats.push_back(std::move(f));
    }

    void area(const osmium::Area &a)
    {
        int k = area_kind(a.tags());
        if (!k) return;
        Feat f{(uint8_t)k, BGM_F_AREA, {}, {}};
        for (const auto &outer : a.outer_rings()) {
            Ring r;
            for (const auto &nr : outer) r.push_back(project(nr.lon(), nr.lat()));
            if (r.size() >= 4) f.rings.push_back(std::move(r));
            for (const auto &inner : a.inner_rings(outer)) {
                Ring h;
                for (const auto &nr : inner) h.push_back(project(nr.lon(), nr.lat()));
                if (h.size() >= 4) f.rings.push_back(std::move(h));
            }
        }
        if (!f.rings.empty()) feats.push_back(std::move(f));
    }
};

class Boundaries : public osmium::handler::Handler {
public:
    void relation(const osmium::Relation &r)
    {
        const auto &t = r.tags();
        if (!is(t["boundary"], "administrative")) return;
        const char *l = t["admin_level"];
        if (!is(l, "2") && !is(l, "4")) return;
        for (const auto &m : r.members())
            if (m.type() == osmium::item_type::way) boundary_ways.insert(m.ref());
    }
};

// ---------- geometry ----------

double seg_dist2(const Pt &p, const Pt &a, const Pt &b)
{
    double dx = (double)(b.x - a.x), dy = (double)(b.y - a.y);
    double px = (double)(p.x - a.x), py = (double)(p.y - a.y);
    double l = dx * dx + dy * dy;
    double t = l > 0 ? (px * dx + py * dy) / l : 0;
    t = std::max(0.0, std::min(1.0, t));
    double ex = px - t * dx, ey = py - t * dy;
    return ex * ex + ey * ey;
}

void dp(const Ring &in, size_t a, size_t b, double tol2, std::vector<char> &keep)
{
    double best = -1;
    size_t bi = a;
    for (size_t i = a + 1; i < b; i++) {
        double d = seg_dist2(in[i], in[a], in[b]);
        if (d > best) { best = d; bi = i; }
    }
    if (best > tol2) {
        keep[bi] = 1;
        dp(in, a, bi, tol2, keep);
        dp(in, bi, b, tol2, keep);
    }
}

Ring simplify(const Ring &in, double tol)
{
    if (in.size() <= 2) return in;
    std::vector<char> keep(in.size(), 0);
    keep.front() = keep.back() = 1;
    dp(in, 0, in.size() - 1, tol * tol, keep);
    Ring out;
    for (size_t i = 0; i < in.size(); i++)
        if (keep[i]) out.push_back(in[i]);
    return out;
}

double ring_area(const Ring &r)
{
    double s = 0;
    for (size_t i = 0, j = r.size() - 1; i < r.size(); j = i++)
        s += ((double)r[j].x + r[i].x) * ((double)r[j].y - r[i].y);
    return std::fabs(s) / 2;
}

struct Box { int64_t x0, y0, x1, y1; };

// Sutherland-Hodgman against one axis-aligned edge
Ring clip_edge(const Ring &in, int axis, int64_t v, bool keep_greater)
{
    Ring out;
    if (in.empty()) return out;
    auto inside = [&](const Pt &p) { int64_t c = axis ? p.y : p.x; return keep_greater ? c >= v : c <= v; };
    auto cross = [&](const Pt &a, const Pt &b) {
        double ca = axis ? a.y : a.x, cb = axis ? b.y : b.x;
        double t = (v - ca) / (cb - ca);
        Pt p;
        if (axis) { p.y = v; p.x = (int64_t)std::llround(a.x + t * (double)(b.x - a.x)); }
        else { p.x = v; p.y = (int64_t)std::llround(a.y + t * (double)(b.y - a.y)); }
        return p;
    };
    Pt prev = in.back();
    bool pin = inside(prev);
    for (const Pt &cur : in) {
        bool cin = inside(cur);
        if (cin) {
            if (!pin) out.push_back(cross(prev, cur));
            out.push_back(cur);
        } else if (pin) {
            out.push_back(cross(prev, cur));
        }
        prev = cur;
        pin = cin;
    }
    return out;
}

Ring clip_polygon(const Ring &r, const Box &b)
{
    Ring o = clip_edge(r, 0, b.x0, true);
    o = clip_edge(o, 0, b.x1, false);
    o = clip_edge(o, 1, b.y0, true);
    o = clip_edge(o, 1, b.y1, false);
    return o;
}

// Liang-Barsky on each segment; consecutive visible pieces are joined
std::vector<Ring> clip_line(const Ring &r, const Box &b)
{
    std::vector<Ring> out;
    Ring cur;
    for (size_t i = 0; i + 1 < r.size(); i++) {
        double x0 = r[i].x, y0 = r[i].y, x1 = r[i + 1].x, y1 = r[i + 1].y;
        double t0 = 0, t1 = 1, dx = x1 - x0, dy = y1 - y0;
        double p[4] = {-dx, dx, -dy, dy}, q[4] = {x0 - b.x0, b.x1 - x0, y0 - b.y0, b.y1 - y0};
        bool vis = true;
        for (int k = 0; k < 4 && vis; k++) {
            if (p[k] == 0) { if (q[k] < 0) vis = false; }
            else {
                double t = q[k] / p[k];
                if (p[k] < 0) { if (t > t1) vis = false; else if (t > t0) t0 = t; }
                else { if (t < t0) vis = false; else if (t < t1) t1 = t; }
            }
        }
        if (!vis) {
            if (cur.size() >= 2) out.push_back(cur);
            cur.clear();
            continue;
        }
        Pt a{(int64_t)std::llround(x0 + t0 * dx), (int64_t)std::llround(y0 + t0 * dy)};
        Pt c{(int64_t)std::llround(x0 + t1 * dx), (int64_t)std::llround(y0 + t1 * dy)};
        if (cur.empty() || t0 > 0) {
            if (cur.size() >= 2) out.push_back(cur);
            cur.clear();
            cur.push_back(a);
        }
        cur.push_back(c);
        if (t1 < 1) {
            if (cur.size() >= 2) out.push_back(cur);
            cur.clear();
        }
    }
    if (cur.size() >= 2) out.push_back(cur);
    return out;
}

// ---------- encoding ----------

void put_varint(std::string &o, uint64_t v)
{
    while (v >= 0x80) { o.push_back((char)(v | 0x80)); v >>= 7; }
    o.push_back((char)v);
}
void put_zz(std::string &o, int64_t v) { put_varint(o, ((uint64_t)v << 1) ^ (uint64_t)(v >> 63)); }

struct TileFeat {
    uint8_t kind, flags;
    std::string name;
    std::vector<std::vector<std::pair<int32_t, int32_t>>> rings;
};

struct Tile {
    std::vector<TileFeat> feats;
};

std::string encode_tile(Tile &t)
{
    std::stable_sort(t.feats.begin(), t.feats.end(), [](const TileFeat &a, const TileFeat &b) { return a.kind < b.kind; });
    std::vector<std::string> names;
    std::unordered_map<std::string, uint32_t> name_id;
    for (auto &f : t.feats)
        if (!f.name.empty() && !name_id.count(f.name)) { name_id[f.name] = (uint32_t)names.size(); names.push_back(f.name); }
    std::string o;
    put_varint(o, names.size());
    for (auto &n : names) { put_varint(o, n.size()); o += n; }
    size_t nring = 0, npts = 0;
    for (auto &f : t.feats)
        for (auto &r : f.rings) { nring++; npts += r.size(); }
    put_varint(o, t.feats.size());
    put_varint(o, nring);
    put_varint(o, npts);
    for (auto &f : t.feats) {
        uint8_t flags = f.flags & ~BGM_F_NAME;
        if (!f.name.empty()) flags |= BGM_F_NAME;
        o.push_back((char)f.kind);
        o.push_back((char)flags);
        if (!f.name.empty()) put_varint(o, name_id[f.name]);
        int g = flags & BGM_F_GEOM;
        if (g == BGM_F_POINT) {
            put_zz(o, f.rings[0][0].first);
            put_zz(o, f.rings[0][0].second);
            continue;
        }
        if (g == BGM_F_AREA) put_varint(o, f.rings.size());
        int32_t px = 0, py = 0;
        for (auto &r : f.rings) {
            put_varint(o, r.size());
            for (auto &p : r) { put_zz(o, p.first - px); put_zz(o, p.second - py); px = p.first; py = p.second; }
        }
    }
    return o;
}

} // namespace

int main(int argc, char **argv)
{
    if (argc != 3) {
        std::fprintf(stderr, "uso: %s IN.osm.pbf OUT.bgm\n", argv[0]);
        return 2;
    }
    osmium::io::File input{argv[1]};
    time_t t0 = time(nullptr);

    // pass 1: relations (multipolygons and state/country borders)
    osmium::TagsFilter filter{false};
    for (const char *k : {"building", "natural", "waterway", "landuse", "leisure", "aeroway", "amenity", "highway", "place"})
        filter.add_rule(true, osmium::TagMatcher{k});
    osmium::area::Assembler::config_type acfg;
    osmium::area::MultipolygonManager<osmium::area::Assembler> mp{acfg, filter};
    {
        Boundaries bh;
        osmium::io::Reader rr{input, osmium::osm_entity_bits::relation};
        while (osmium::memory::Buffer buf = rr.read()) {
            osmium::apply(buf, bh);
            for (auto &r : buf.select<osmium::Relation>()) mp.relation(r);
        }
        rr.close();
        mp.prepare_for_lookup();
    }
    std::fprintf(stderr, "relações lidas (%zu vias de divisa)\n", boundary_ways.size());

    // pass 2: nodes, ways, areas
    using Index = osmium::index::map::FlexMem<osmium::unsigned_object_id_type, osmium::Location>;
    Index index;
    osmium::handler::NodeLocationsForWays<Index> loc{index};
    loc.ignore_errors();
    Collect col;
    {
        osmium::io::Reader rd{input};
        osmium::apply(rd, loc, col, mp.handler([&col](osmium::memory::Buffer &&b) { osmium::apply(b, col); }));
        rd.close();
    }
    std::fprintf(stderr, "%zu feições coletadas em %ld s\n", feats.size(), (long)(time(nullptr) - t0));

    // bounding box
    Box bb{INT64_MAX, INT64_MAX, INT64_MIN, INT64_MIN};
    for (auto &f : feats)
        for (auto &r : f.rings)
            for (auto &p : r) { bb.x0 = std::min(bb.x0, p.x); bb.y0 = std::min(bb.y0, p.y); bb.x1 = std::max(bb.x1, p.x); bb.y1 = std::max(bb.y1, p.y); }

    FILE *out = std::fopen(argv[2], "wb");
    if (!out) { std::perror(argv[2]); return 1; }
    bgm_header_t hdr{};
    bgm_level_t lv[NLEVELS]{};
    std::fwrite(&hdr, sizeof hdr, 1, out);
    std::fwrite(lv, sizeof lv, 1, out);
    uint32_t pos = sizeof hdr + sizeof lv;
    std::vector<std::vector<bgm_index_t>> idx(NLEVELS);

    for (int L = 0; L < NLEVELS; L++) {
        int z = LEVELS[L];
        int next = L + 1 < NLEVELS ? LEVELS[L + 1] : 99;
        int shift = 32 - z - BGM_EXTENT_BITS;           // world units per tile unit (log2)
        double px = std::ldexp(1.0, 32 - DETAIL_ZOOM[L] - 8);   // world units per pixel at the detail zoom
        double tol = 0.35 * px;
        double min_area = 2.0 * px * px;
        int64_t buf = (int64_t)BGM_BUFFER << shift;
        std::map<uint32_t, Tile> tiles;
        size_t nf = 0;
        for (auto &f : feats) {
            if (bgk_minzoom(f.kind) >= next) continue;
            int g = f.flags & BGM_F_GEOM;
            std::vector<Ring> rings;
            for (auto &r : f.rings) {
                if (g == BGM_F_POINT) { rings.push_back(r); continue; }
                Ring s = simplify(r, tol);
                if (g == BGM_F_AREA && (s.size() < 4 || ring_area(s) < min_area)) continue;
                if (g == BGM_F_LINE && s.size() < 2) continue;
                rings.push_back(std::move(s));
            }
            if (rings.empty()) continue;
            Box fb{INT64_MAX, INT64_MAX, INT64_MIN, INT64_MIN};
            for (auto &r : rings)
                for (auto &p : r) { fb.x0 = std::min(fb.x0, p.x); fb.y0 = std::min(fb.y0, p.y); fb.x1 = std::max(fb.x1, p.x); fb.y1 = std::max(fb.y1, p.y); }
            int64_t tx0 = std::max<int64_t>(0, (fb.x0 - buf) >> (32 - z)), tx1 = (fb.x1 + buf) >> (32 - z);
            int64_t ty0 = std::max<int64_t>(0, (fb.y0 - buf) >> (32 - z)), ty1 = (fb.y1 + buf) >> (32 - z);
            if (g == BGM_F_POINT) { tx1 = tx0 = fb.x0 >> (32 - z); ty1 = ty0 = fb.y0 >> (32 - z); }
            for (int64_t ty = ty0; ty <= ty1; ty++)
                for (int64_t tx = tx0; tx <= tx1; tx++) {
                    int64_t ox = tx << (32 - z), oy = ty << (32 - z);
                    Box tb{ox - buf, oy - buf, ox + ((int64_t)1 << (32 - z)) + buf, oy + ((int64_t)1 << (32 - z)) + buf};
                    TileFeat tf{f.kind, f.flags, f.name, {}};
                    auto local = [&](const Ring &r) {
                        std::vector<std::pair<int32_t, int32_t>> o;
                        for (auto &p : r) {
                            int32_t x = (int32_t)((p.x - ox) >> shift), y = (int32_t)((p.y - oy) >> shift);
                            if (o.empty() || o.back().first != x || o.back().second != y) o.emplace_back(x, y);
                        }
                        return o;
                    };
                    if (g == BGM_F_POINT) {
                        tf.rings.push_back(local(rings[0]));
                    } else if (g == BGM_F_LINE) {
                        for (auto &piece : clip_line(rings[0], tb)) {
                            auto l = local(piece);
                            if (l.size() >= 2) {
                                TileFeat t2{f.kind, f.flags, f.name, {std::move(l)}};
                                tiles[(uint32_t)(ty << 16 | tx)].feats.push_back(std::move(t2));
                                nf++;
                            }
                        }
                        continue;
                    } else {
                        for (auto &r : rings) {
                            Ring c = clip_polygon(r, tb);
                            if (c.size() < 3) continue;
                            auto l = local(c);
                            if (l.size() >= 3) tf.rings.push_back(std::move(l));
                        }
                        if (tf.rings.empty()) continue;
                    }
                    tiles[(uint32_t)(ty << 16 | tx)].feats.push_back(std::move(tf));
                    nf++;
                }
        }
        size_t bytes = 0;
        for (auto &kv : tiles) {
            std::string blob = encode_tile(kv.second);
            std::fwrite(blob.data(), 1, blob.size(), out);
            idx[L].push_back({kv.first, pos, (uint32_t)blob.size()});
            pos += (uint32_t)blob.size();
            bytes += blob.size();
        }
        lv[L].zoom = (uint8_t)z;
        lv[L].ntiles = (uint32_t)tiles.size();
        std::fprintf(stderr, "nível z%d: %zu tiles, %zu feições, %zu bytes\n", z, tiles.size(), nf, bytes);
    }
    for (int L = 0; L < NLEVELS; L++) {
        lv[L].index_off = pos;
        std::fwrite(idx[L].data(), sizeof(bgm_index_t), idx[L].size(), out);
        pos += (uint32_t)(idx[L].size() * sizeof(bgm_index_t));
    }
    const char attr[] = "Dados do mapa \xC2\xA9 colaboradores do OpenStreetMap, licen\xC3\xA7" "a ODbL 1.0 (openstreetmap.org/copyright)";
    hdr.attr_off = pos;
    std::fwrite(attr, 1, sizeof attr, out);
    hdr.magic = BGM_MAGIC;
    hdr.version = BGM_VERSION;
    hdr.nlevels = NLEVELS;
    hdr.bbox[0] = (uint32_t)bb.x0; hdr.bbox[1] = (uint32_t)bb.y0; hdr.bbox[2] = (uint32_t)bb.x1; hdr.bbox[3] = (uint32_t)bb.y1;
    hdr.created = (uint32_t)time(nullptr);
    std::fseek(out, 0, SEEK_SET);
    std::fwrite(&hdr, sizeof hdr, 1, out);
    std::fwrite(lv, sizeof lv, 1, out);
    if (std::fclose(out)) { std::perror(argv[2]); return 1; }
    std::fprintf(stderr, "%s: %u bytes em %ld s\n", argv[2], pos + (uint32_t)sizeof attr, (long)(time(nullptr) - t0));
    return 0;
}
