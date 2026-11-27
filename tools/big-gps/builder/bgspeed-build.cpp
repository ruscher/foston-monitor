// bgspeed-build.cpp - Big GPS road speed limits (runs on the PC).
//
//   bgspeed-build IN.osm.pbf OUT.bgk [--bbox MINLON,MINLAT,MAXLON,MAXLAT]
//
// Ways (highway=*) with a numeric maxspeed, maxspeed:forward or maxspeed:backward become
// pieces of at most 200 m with the limit in each direction (src/ui/biggps/bgspeed_format.h).
// Only what the way says: "BR:urban", "signals", "none" and the like are skipped, and no
// limit is inferred from the road type.  Two passes over the file (ways, then only their
// nodes' locations), like bglanes-build.  Data: OpenStreetMap contributors, ODbL.
//
// SPDX-License-Identifier: GPL-3.0-or-later
#include <osmium/handler.hpp>
#include <osmium/io/pbf_input.hpp>
#include <osmium/visitor.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <unordered_map>
#include <vector>

extern "C" {
#include "../../../src/ui/biggps/bgspeed_format.h"
}

namespace {

double bx0 = -180, by0 = -90, bx1 = 180, by1 = 90;

struct Way {
    size_t first, n;            // node ids in ids[first .. first + n)
    uint8_t fwd, bwd;
};
std::vector<Way> ways;
std::vector<osmium::object_id_type> ids;
std::unordered_map<osmium::object_id_type, std::pair<double, double>> loc;
unsigned skipped_values;

// "60", "60 km/h", "60kmh", "40 mph"; 0 for anything else
int speed(const char *v)
{
    if (!v) return 0;
    char *end;
    long n = std::strtol(v, &end, 10);
    if (end == v) { skipped_values++; return 0; }
    while (*end == ' ') end++;
    if (!std::strcmp(end, "mph")) n = std::lround(n * 1.609344);
    else if (*end && std::strcmp(end, "km/h") && std::strcmp(end, "kmh") && std::strcmp(end, "kph")) { skipped_values++; return 0; }
    return n >= 5 && n <= 200 ? (int)n : 0;
}

struct Ways : osmium::handler::Handler {
    void way(const osmium::Way &w)
    {
        const auto &t = w.tags();
        if (!t["highway"] || w.nodes().size() < 2) return;
        // o do sentido vale quando existe, mesmo não numérico ("signals": sem placa naquele
        // sentido); o geral só preenche o sentido sem etiqueta própria
        int both = speed(t["maxspeed"]);
        int f = t["maxspeed:forward"] ? speed(t["maxspeed:forward"]) : both;
        int b = t["maxspeed:backward"] ? speed(t["maxspeed:backward"]) : both;
        if (!f && !b) return;
        Way x{ ids.size(), w.nodes().size(), (uint8_t)f, (uint8_t)b };
        for (const auto &nd : w.nodes()) { ids.push_back(nd.ref()); loc.emplace(nd.ref(), std::make_pair(1000.0, 1000.0)); }
        ways.push_back(x);
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

} // namespace

int main(int argc, char **argv)
{
    if (argc < 3) { std::fprintf(stderr, "uso: bgspeed-build IN.osm.pbf OUT.bgk [--bbox MINLON,MINLAT,MAXLON,MAXLAT]\n"); return 2; }
    for (int i = 3; i < argc; i++)
        if (!std::strncmp(argv[i], "--bbox=", 7)) std::sscanf(argv[i] + 7, "%lf,%lf,%lf,%lf", &bx0, &by0, &bx1, &by1);
        else if (!std::strcmp(argv[i], "--bbox") && i + 1 < argc) std::sscanf(argv[++i], "%lf,%lf,%lf,%lf", &bx0, &by0, &bx1, &by1);
    {
        osmium::io::Reader rd(argv[1], osmium::osm_entity_bits::way);
        Ways h;
        osmium::apply(rd, h);
    }
    {
        osmium::io::Reader rd(argv[1], osmium::osm_entity_bits::node);
        Nodes h;
        osmium::apply(rd, h);
    }
    std::vector<bgk_entry_t> out;
    for (const Way &w : ways) {
        for (size_t i = 0; i + 1 < w.n; i++) {
            auto a = loc[ids[w.first + i]], b = loc[ids[w.first + i + 1]];
            if (a.first > 999 || b.first > 999) continue;                      // node outside the extract
            if (std::max(a.first, b.first) < bx0 || std::min(a.first, b.first) > bx1 ||
                std::max(a.second, b.second) < by0 || std::min(a.second, b.second) > by1) continue;
            double lat = (a.second + b.second) / 2 * M_PI / 180;
            double dm = std::hypot((b.first - a.first) * std::cos(lat), b.second - a.second) * 111320.0;
            int pieces = std::max(1, (int)std::ceil(dm / BGK_MAX_PIECE_M));
            for (int k = 0; k < pieces; k++) {
                double t0 = (double)k / pieces, t1 = (double)(k + 1) / pieces;
                bgk_entry_t e{};
                e.xa = world_x(a.first + (b.first - a.first) * t0);
                e.ya = world_y(a.second + (b.second - a.second) * t0);
                e.xb = world_x(a.first + (b.first - a.first) * t1);
                e.yb = world_y(a.second + (b.second - a.second) * t1);
                e.fwd = w.fwd;
                e.bwd = w.bwd;
                out.push_back(e);
            }
        }
    }
    std::sort(out.begin(), out.end(), [](const bgk_entry_t &p, const bgk_entry_t &q) { return std::min(p.xa, p.xb) < std::min(q.xa, q.xb); });
    FILE *f = std::fopen(argv[2], "wb");
    if (!f) { std::perror(argv[2]); return 1; }
    std::fwrite(BGK_MAGIC, 1, 4, f);
    put32(f, BGK_VERSION);
    put32(f, (uint32_t)out.size());
    put32(f, (uint32_t)std::time(nullptr));
    for (const auto &e : out) {
        put32(f, e.xa); put32(f, e.ya); put32(f, e.xb); put32(f, e.yb);
        unsigned char tail[4] = { e.fwd, e.bwd, 0, 0 };
        std::fwrite(tail, 1, 4, f);
    }
    // um fwrite que falhou no meio (cartão ou disco cheio) não pode virar um .bgk cortado "ok"
    bool bad = std::ferror(f) != 0;
    if (std::fclose(f) || bad) { std::fprintf(stderr, "%s: erro ao gravar\n", argv[2]); std::remove(argv[2]); return 1; }
    std::fprintf(stderr, "bgspeed-build: %zu vias com limite, %zu trechos; %u valores não numéricos ignorados\n",
                 ways.size(), out.size(), skipped_values);
    return 0;
}
