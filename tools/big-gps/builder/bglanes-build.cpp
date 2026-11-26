// bglanes-build.cpp - Big GPS lane guidance (runs on the PC).
//
//   bglanes-build IN.osm.pbf OUT.bgl [--bbox MINLON,MINLAT,MAXLON,MAXLAT]
//
// Ways with turn:lanes (or :forward / :backward) become entries at the junction a driver
// reaches, with the bearing they arrive with (src/ui/biggps/bglanes_format.h).  Two passes
// over the file: the tagged ways first (only the node ids needed), then those nodes'
// locations — no location index for the whole country.  Data: OpenStreetMap, ODbL.
//
// SPDX-License-Identifier: GPL-3.0-or-later
#include <osmium/handler.hpp>
#include <osmium/io/pbf_input.hpp>
#include <osmium/visitor.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>
#include <unordered_map>
#include <vector>

extern "C" {
#include "../../../src/ui/biggps/bglanes_format.h"
}

namespace {

double bx0 = -180, by0 = -90, bx1 = 180, by1 = 90;

struct Want {
    osmium::object_id_type at, prev;   // junction node and the node before it (arriving)
    uint8_t n;
    uint16_t lane[BGL_MAXLANES];
};
std::vector<Want> wants;
std::unordered_map<osmium::object_id_type, std::pair<double, double>> loc;
unsigned bad_tags;

// "left|through;right||slight_right" -> masks; 0 for an empty lane or "none"
int parse(const char *v, uint16_t *out)
{
    int n = 0;
    std::string s(v);
    size_t a = 0;
    for (;;) {
        size_t b = s.find('|', a);
        std::string lane = s.substr(a, b == std::string::npos ? std::string::npos : b - a);
        if (n == BGL_MAXLANES) return -1;
        uint16_t m = 0;
        size_t c = 0;
        while (c <= lane.size()) {
            size_t d = lane.find(';', c);
            std::string t = lane.substr(c, d == std::string::npos ? std::string::npos : d - c);
            if (t == "left") m |= BGL_LEFT;
            else if (t == "slight_left") m |= BGL_SLIGHT_LEFT;
            else if (t == "sharp_left") m |= BGL_SHARP_LEFT;
            else if (t == "through") m |= BGL_THROUGH;
            else if (t == "right") m |= BGL_RIGHT;
            else if (t == "slight_right") m |= BGL_SLIGHT_RIGHT;
            else if (t == "sharp_right") m |= BGL_SHARP_RIGHT;
            else if (t == "reverse") m |= BGL_REVERSE;
            else if (t == "merge_to_left") m |= BGL_MERGE_LEFT;
            else if (t == "merge_to_right") m |= BGL_MERGE_RIGHT;
            else if (!t.empty() && t != "none") return -1;          // unknown value: skip the way
            if (d == std::string::npos) break;
            c = d + 1;
        }
        out[n++] = m;
        if (b == std::string::npos) break;
        a = b + 1;
    }
    return n;
}

struct Ways : osmium::handler::Handler {
    void way(const osmium::Way &w)
    {
        const auto &t = w.tags();
        if (!t["highway"] || w.nodes().size() < 2) return;
        const auto &nd = w.nodes();
        size_t k = nd.size();
        const char *fw = t["turn:lanes:forward"], *bw = t["turn:lanes:backward"], *both = t["turn:lanes"];
        const char *ow = t["oneway"];
        bool oneway = ow && (!std::strcmp(ow, "yes") || !std::strcmp(ow, "1") || !std::strcmp(ow, "true"));
        bool reversed = ow && !std::strcmp(ow, "-1");
        if (!fw && both && (oneway || t["junction"] || reversed || !std::strcmp(t["highway"], "motorway")
                            || !std::strcmp(t["highway"], "motorway_link"))) fw = both;
        auto add = [&](const char *v, bool forward) {
            Want x;
            int n = parse(v, x.lane);
            if (n <= 0) { bad_tags++; return; }
            x.n = (uint8_t)n;
            if (forward) { x.at = nd[k - 1].ref(); x.prev = nd[k - 2].ref(); }
            else { x.at = nd[0].ref(); x.prev = nd[1].ref(); }
            wants.push_back(x);
        };
        if (reversed && fw) add(fw, false);
        else if (fw) add(fw, true);
        if (bw) add(bw, false);
    }
};

struct Nodes : osmium::handler::Handler {
    void node(const osmium::Node &n)
    {
        auto it = loc.find(n.id());
        if (it != loc.end() && n.location().valid()) it->second = { n.location().lon(), n.location().lat() };
    }
};

uint32_t world_x(double lon) { return (uint32_t)std::llround(std::min(std::max((lon + 180.0) / 360.0 * 4294967296.0, 0.0), 4294967295.0)); }
uint32_t world_y(double lat)
{
    double r = std::max(std::min(lat, 85.0511), -85.0511) * M_PI / 180.0;
    double y = (1.0 - std::log(std::tan(r) + 1.0 / std::cos(r)) / M_PI) / 2.0 * 4294967296.0;
    return (uint32_t)std::llround(std::min(std::max(y, 0.0), 4294967295.0));
}

void put32(FILE *f, uint32_t v) { unsigned char b[4] = { (unsigned char)v, (unsigned char)(v >> 8), (unsigned char)(v >> 16), (unsigned char)(v >> 24) }; std::fwrite(b, 1, 4, f); }
void put16(FILE *f, uint16_t v) { unsigned char b[2] = { (unsigned char)v, (unsigned char)(v >> 8) }; std::fwrite(b, 1, 2, f); }

} // namespace

int main(int argc, char **argv)
{
    const char *bbox = argc == 4 && !std::strncmp(argv[3], "--bbox=", 7) ? argv[3] + 7 : argc == 5 && !std::strcmp(argv[3], "--bbox") ? argv[4] : nullptr;
    if ((argc != 3 && !bbox) || (bbox && std::sscanf(bbox, "%lf,%lf,%lf,%lf", &bx0, &by0, &bx1, &by1) != 4)) {
        std::fprintf(stderr, "uso: %s IN.osm.pbf OUT.bgl [--bbox MINLON,MINLAT,MAXLON,MAXLAT]\n", argv[0]);
        return 2;
    }
    {
        osmium::io::Reader rd{argv[1], osmium::osm_entity_bits::way};
        Ways h;
        osmium::apply(rd, h);
    }
    for (const auto &w : wants) { loc.emplace(w.at, std::make_pair(1e9, 1e9)); loc.emplace(w.prev, std::make_pair(1e9, 1e9)); }
    {
        osmium::io::Reader rd{argv[1], osmium::osm_entity_bits::node};
        Nodes h;
        osmium::apply(rd, h);
    }
    std::vector<bgl_entry_t> out;
    for (const auto &w : wants) {
        auto a = loc[w.at], p = loc[w.prev];
        if (a.first > 1e8 || p.first > 1e8) continue;
        if (a.first < bx0 || a.first > bx1 || a.second < by0 || a.second > by1) continue;
        bgl_entry_t e;
        std::memset(&e, 0, sizeof e);
        e.x = world_x(a.first);
        e.y = world_y(a.second);
        double dx = (double)(int32_t)(e.x - world_x(p.first)), dy = (double)(int32_t)(e.y - world_y(p.second));
        double br = std::fmod(std::atan2(dx, -dy) * 180 / M_PI + 360, 360);   // from prev to the junction
        e.bearing = (uint16_t)std::lround(br) % 360;
        e.n = w.n;
        for (int i = 0; i < w.n; i++) e.lane[i] = w.lane[i];
        out.push_back(e);
    }
    std::sort(out.begin(), out.end(), [](const bgl_entry_t &a, const bgl_entry_t &b) { return a.x != b.x ? a.x < b.x : a.y < b.y; });
    std::string tmp = std::string(argv[2]) + ".tmp";
    FILE *f = std::fopen(tmp.c_str(), "wb");
    if (!f) { std::perror(argv[2]); return 1; }
    std::fwrite(BGL_MAGIC, 1, 4, f);
    put32(f, BGL_VERSION);
    put32(f, (uint32_t)out.size());
    put32(f, (uint32_t)std::time(nullptr));
    for (const auto &e : out) {
        put32(f, e.x);
        put32(f, e.y);
        put16(f, e.bearing);
        unsigned char nb[2] = { e.n, 0 };
        std::fwrite(nb, 1, 2, f);
        for (int i = 0; i < BGL_MAXLANES; i++) put16(f, e.lane[i]);
    }
    if (std::fclose(f) || std::rename(tmp.c_str(), argv[2])) { std::perror(argv[2]); return 1; }
    std::printf("%s: %zu cruzamentos com faixas (%u vias com turn:lanes inválido ignoradas)\n", argv[2], out.size(), bad_tags);
    return 0;
}
