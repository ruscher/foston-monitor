// bgsearch-build.cpp - Big GPS search index builder (runs on the PC).
//
//   bgsearch-build IN.osm.pbf OUT.bgs [--bbox MINLON,MINLAT,MAXLON,MAXLAT]
//
// Collects places, named streets (ways of the same name in the same neighbourhood become
// one entry), points of interest by category and addresses with a house number, gives each
// a context (nearest neighbourhood or town), and writes the word index described in
// src/ui/biggps/bgsearch_format.h.
//
// SPDX-License-Identifier: GPL-3.0-or-later
#include <osmium/handler.hpp>
#include <osmium/handler/node_locations_for_ways.hpp>
#include <osmium/index/map/flex_mem.hpp>
#include <osmium/io/pbf_input.hpp>
#include <osmium/visitor.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

extern "C" {
#include "../../../src/ui/biggps/bgsearch_format.h"
}

namespace {

bool is(const char *v, const char *s) { return v && !std::strcmp(v, s); }

struct Ent {
    double lon, lat;
    uint8_t type, cat;
    uint16_t rank;
    std::string name, context;
};

struct Place {
    double lon, lat;
    int level;                 // 0 neighbourhood-like, 1 town-like
    std::string name;
};

std::vector<Ent> ents;
std::vector<Place> places;

struct StreetAcc {
    double lon = 0, lat = 0, best_len = -1;
    uint16_t rank = 0;
    std::string name;
};

double bx0 = -180, by0 = -90, bx1 = 180, by1 = 90;
bool inside(double lon, double lat) { return lon >= bx0 && lon <= bx1 && lat >= by0 && lat <= by1; }

int place_rank(const char *v, int *level)
{
    *level = 0;
    if (is(v, "city")) { *level = 1; return 100; }
    if (is(v, "town")) { *level = 1; return 80; }
    if (is(v, "village")) { *level = 1; return 55; }
    if (is(v, "suburb") || is(v, "quarter")) return 60;
    if (is(v, "neighbourhood")) return 45;
    if (is(v, "hamlet") || is(v, "locality") || is(v, "isolated_dwelling")) return 30;
    return -1;
}

uint8_t poi_cat(const osmium::TagList &t)
{
    const char *a = t["amenity"], *s = t["shop"], *tu = t["tourism"], *l = t["leisure"];
    if (a) {
        if (is(a, "fuel")) return BGS_C_FUEL;
        if (is(a, "hospital") || is(a, "clinic") || is(a, "doctors")) return BGS_C_HOSPITAL;
        if (is(a, "pharmacy")) return BGS_C_PHARMACY;
        if (is(a, "police") || is(a, "fire_station")) return BGS_C_POLICE;
        if (is(a, "school") || is(a, "kindergarten") || is(a, "college")) return BGS_C_SCHOOL;
        if (is(a, "university")) return BGS_C_UNIVERSITY;
        if (is(a, "restaurant") || is(a, "fast_food") || is(a, "cafe") || is(a, "bar") || is(a, "pub") || is(a, "ice_cream")) return BGS_C_FOOD;
        if (is(a, "bank") || is(a, "atm")) return BGS_C_BANK;
        if (is(a, "parking")) return BGS_C_PARKING;
        if (is(a, "bus_station")) return BGS_C_BUS;
        if (is(a, "place_of_worship")) return BGS_C_WORSHIP;
        if (is(a, "townhall") || is(a, "courthouse") || is(a, "post_office") || is(a, "library")) return BGS_C_GOV;
    }
    if (s) {
        if (is(s, "supermarket") || is(s, "convenience") || is(s, "bakery") || is(s, "greengrocer") || is(s, "butcher")) return BGS_C_MARKET;
        if (is(s, "mall") || is(s, "department_store")) return BGS_C_MALL;
        return BGS_C_SHOP;
    }
    if (tu) {
        if (is(tu, "hotel") || is(tu, "motel") || is(tu, "hostel") || is(tu, "guest_house")) return BGS_C_HOTEL;
        if (is(tu, "attraction") || is(tu, "museum") || is(tu, "viewpoint") || is(tu, "zoo") || is(tu, "theme_park")) return BGS_C_ATTRACTION;
    }
    if (l && (is(l, "park") || is(l, "stadium") || is(l, "sports_centre"))) return BGS_C_ATTRACTION;
    if (is(t["aeroway"], "aerodrome")) return BGS_C_AIRPORT;
    if (is(t["public_transport"], "station") || is(t["railway"], "station")) return BGS_C_BUS;
    if (t["office"] && is(t["office"], "government")) return BGS_C_GOV;
    return BGS_C_NONE;
}

uint16_t street_rank(const char *h)
{
    if (is(h, "motorway") || is(h, "trunk")) return 75;
    if (is(h, "primary")) return 65;
    if (is(h, "secondary")) return 55;
    if (is(h, "tertiary")) return 45;
    if (is(h, "residential") || is(h, "unclassified") || is(h, "living_street") || is(h, "pedestrian")) return 35;
    if (is(h, "service") || is(h, "road")) return 20;
    return 0;
}

class Collect : public osmium::handler::Handler {
public:
    std::map<std::string, StreetAcc> streets;   // key: normalized name + "\x1f" + rough cell (merged later by context)

    void add_feature(const osmium::TagList &t, double lon, double lat)
    {
        if (!inside(lon, lat)) return;
        const char *name = t["name"];
        uint8_t cat = poi_cat(t);
        if (name && *name && cat != BGS_C_NONE) ents.push_back({lon, lat, BGS_T_POI, cat, (uint16_t)(cat == BGS_C_AIRPORT ? 90 : cat == BGS_C_MALL || cat == BGS_C_HOSPITAL ? 50 : 25), name, {}});
        const char *hn = t["addr:housenumber"], *st = t["addr:street"];
        if (hn && st && *hn && *st) {
            Ent e{lon, lat, BGS_T_ADDRESS, BGS_C_NONE, 10, std::string(st) + ", " + hn, {}};
            if (const char *c = t["addr:suburb"]) e.context = c;
            else if (const char *c2 = t["addr:city"]) e.context = c2;
            ents.push_back(std::move(e));
        }
    }

    void node(const osmium::Node &n)
    {
        if (!n.location().valid() || n.tags().empty()) return;
        double lon = n.location().lon(), lat = n.location().lat();
        const char *pl = n.tags()["place"], *name = n.tags()["name"];
        int level;
        if (pl && name && *name && inside(lon, lat)) {
            int r = place_rank(pl, &level);
            if (r >= 0) {
                ents.push_back({lon, lat, BGS_T_PLACE, BGS_C_NONE, (uint16_t)r, name, {}});
                places.push_back({lon, lat, level, name});
            }
        }
        add_feature(n.tags(), lon, lat);
    }

    void way(const osmium::Way &w)
    {
        const auto &nodes = w.nodes();
        if (nodes.size() < 2) return;
        const char *h = w.tags()["highway"], *name = w.tags()["name"];
        if (h && name && *name && street_rank(h)) {
            /* midpoint and length */
            double len = 0;
            for (size_t i = 1; i < nodes.size(); i++) {
                if (!nodes[i].location().valid() || !nodes[i - 1].location().valid()) continue;
                double dx = (nodes[i].lon() - nodes[i - 1].lon()) * std::cos(nodes[i].lat() * M_PI / 180), dy = nodes[i].lat() - nodes[i - 1].lat();
                len += std::sqrt(dx * dx + dy * dy);
            }
            const auto &mid = nodes[nodes.size() / 2];
            if (!mid.location().valid() || !inside(mid.lon(), mid.lat())) return;
            char norm[256];
            bgs_norm(name, norm, sizeof norm);
            /* split far-apart streets of the same name: ~5 km cells */
            char key[320];
            std::snprintf(key, sizeof key, "%s\x1f%d\x1f%d", norm, (int)std::floor(mid.lon() / 0.05), (int)std::floor(mid.lat() / 0.05));
            StreetAcc &a = streets[key];
            uint16_t r = street_rank(h);
            if (len > a.best_len) { a.best_len = len; a.lon = mid.lon(); a.lat = mid.lat(); a.name = name; }
            a.rank = std::max(a.rank, r);
            return;
        }
        /* POIs and addresses on closed ways: centroid of the nodes */
        if (!w.is_closed() || w.tags().empty()) return;
        double sx = 0, sy = 0;
        int k = 0;
        for (const auto &nr : nodes)
            if (nr.location().valid()) { sx += nr.lon(); sy += nr.lat(); k++; }
        if (k) add_feature(w.tags(), sx / k, sy / k);
    }
};

/* nearest neighbourhood within ~3 km, else nearest town within ~30 km */
struct PlaceGrid {
    std::unordered_map<int64_t, std::vector<int>> cell;
    static int64_t key(int cx, int cy) { return (int64_t)cx << 32 | (uint32_t)cy; }
    void build() {
        for (size_t i = 0; i < places.size(); i++)
            cell[key((int)std::floor(places[i].lon / 0.1), (int)std::floor(places[i].lat / 0.1))].push_back((int)i);
    }
    std::string context(double lon, double lat) const {
        double best[2] = {1e9, 1e9};
        int bi[2] = {-1, -1};
        int cx = (int)std::floor(lon / 0.1), cy = (int)std::floor(lat / 0.1);
        for (int dy = -3; dy <= 3; dy++)
            for (int dx = -3; dx <= 3; dx++) {
                auto it = cell.find(key(cx + dx, cy + dy));
                if (it == cell.end()) continue;
                for (int i : it->second) {
                    const Place &p = places[i];
                    double ex = (p.lon - lon) * std::cos(lat * M_PI / 180) * 111.32, ey = (p.lat - lat) * 111.32, d = ex * ex + ey * ey;
                    if (d < best[p.level]) { best[p.level] = d; bi[p.level] = i; }
                }
            }
        if (bi[0] >= 0 && best[0] < 3.0 * 3.0) return places[bi[0]].name;
        if (bi[1] >= 0 && best[1] < 30.0 * 30.0) return places[bi[1]].name;
        return {};
    }
};

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
    const char *bbox = argc == 4 && !std::strncmp(argv[3], "--bbox=", 7) ? argv[3] + 7 : argc == 5 && !std::strcmp(argv[3], "--bbox") ? argv[4] : nullptr;
    if ((argc != 3 && !bbox) || (bbox && std::sscanf(bbox, "%lf,%lf,%lf,%lf", &bx0, &by0, &bx1, &by1) != 4)) {
        std::fprintf(stderr, "uso: %s IN.osm.pbf OUT.bgs [--bbox MINLON,MINLAT,MAXLON,MAXLAT]\n", argv[0]);
        return 2;
    }
    time_t t0 = time(nullptr);
    using Index = osmium::index::map::FlexMem<osmium::unsigned_object_id_type, osmium::Location>;
    Index index;
    osmium::handler::NodeLocationsForWays<Index> loc{index};
    loc.ignore_errors();
    Collect col;
    {
        osmium::io::Reader rd{osmium::io::File{argv[1]}, osmium::osm_entity_bits::node | osmium::osm_entity_bits::way};
        osmium::apply(rd, loc, col);
        rd.close();
    }
    PlaceGrid grid;
    grid.build();
    /* streets: merge the cells of the same name and context */
    std::map<std::string, StreetAcc> merged;
    for (auto &kv : col.streets) {
        StreetAcc &a = kv.second;
        std::string ctx = grid.context(a.lon, a.lat);
        char norm[256];
        bgs_norm(a.name.c_str(), norm, sizeof norm);
        StreetAcc &m = merged[std::string(norm) + "\x1f" + ctx];
        if (a.best_len > m.best_len) { m.best_len = a.best_len; m.lon = a.lon; m.lat = a.lat; m.name = a.name; }
        m.rank = std::max(m.rank, a.rank);
    }
    for (auto &kv : merged) {
        std::string ctx = kv.first.substr(kv.first.find('\x1f') + 1);
        ents.push_back({kv.second.lon, kv.second.lat, BGS_T_STREET, BGS_C_NONE, kv.second.rank, kv.second.name, ctx});
    }
    for (auto &e : ents)
        if (e.context.empty() && e.type != BGS_T_PLACE) e.context = grid.context(e.lon, e.lat);
    for (auto &e : ents)
        if (e.type == BGS_T_PLACE) {                  /* a place's context is the town around it */
            std::string c = grid.context(e.lon, e.lat);
            if (c != e.name) e.context = c;
        }
    std::fprintf(stderr, "%zu entradas (%zu lugares) em %ld s\n", ents.size(), places.size(), (long)(time(nullptr) - t0));

    /* strings, tokens, postings */
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
    std::map<std::string, std::vector<uint32_t>> words;
    std::vector<bgs_entry_t> out(ents.size());
    for (size_t i = 0; i < ents.size(); i++) {
        const Ent &e = ents[i];
        out[i] = {world_x(e.lon), world_y(e.lat), intern(e.name), intern(e.context), e.type, e.cat, e.rank};
        char norm[512];
        size_t n = bgs_norm(e.name.c_str(), norm, sizeof norm);
        size_t a = 0;
        while (a < n) {
            size_t b = a;
            while (b < n && norm[b] != ' ') b++;
            if (!bgs_stopword(norm + a, b - a)) {
                auto &v = words[std::string(norm + a, b - a)];
                if (v.empty() || v.back() != i) v.push_back((uint32_t)i);
            }
            a = b + 1;
        }
    }
    std::vector<bgs_token_t> toks;
    std::vector<uint32_t> post;
    for (auto &kv : words) {
        toks.push_back({intern(kv.first), (uint32_t)post.size(), (uint32_t)kv.second.size()});
        post.insert(post.end(), kv.second.begin(), kv.second.end());
    }
    while (strings.size() % 4) strings.push_back('\0');

    FILE *f = std::fopen(argv[2], "wb");
    if (!f) { std::perror(argv[2]); return 1; }
    bgs_header_t h{};
    h.magic = BGS_MAGIC;
    h.version = BGS_VERSION;
    h.nentries = (uint32_t)out.size();
    h.ntokens = (uint32_t)toks.size();
    h.npostings = (uint32_t)post.size();
    h.strings_len = (uint32_t)strings.size();
    h.off_entries = sizeof h;
    h.off_tokens = h.off_entries + (uint32_t)(out.size() * sizeof(bgs_entry_t));
    h.off_postings = h.off_tokens + (uint32_t)(toks.size() * sizeof(bgs_token_t));
    h.off_strings = h.off_postings + (uint32_t)(post.size() * 4);
    h.created = (uint32_t)time(nullptr);
    std::fwrite(&h, sizeof h, 1, f);
    std::fwrite(out.data(), sizeof(bgs_entry_t), out.size(), f);
    std::fwrite(toks.data(), sizeof(bgs_token_t), toks.size(), f);
    std::fwrite(post.data(), 4, post.size(), f);
    std::fwrite(strings.data(), 1, strings.size(), f);
    if (std::fclose(f)) { std::perror(argv[2]); return 1; }
    std::fprintf(stderr, "%s: %u entradas, %u palavras, %u referências, %u bytes de texto (%ld s)\n", argv[2], h.nentries, h.ntokens,
                 h.npostings, h.strings_len, (long)(time(nullptr) - t0));
    return 0;
}
