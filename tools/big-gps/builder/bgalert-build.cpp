// bgalert-build.cpp - Big GPS road alert points (runs on the PC).
//
//   bgalert-build IN.osm.pbf OUT.bga [--bbox MINLON,MINLAT,MAXLON,MAXLAT]
//
// Reads only nodes: traffic signals, fixed speed cameras, speed bumps and level crossings,
// with the node's own maxspeed and direction tags when present (nothing is guessed), and
// writes src/ui/biggps/bgalert_format.h.  Data: OpenStreetMap contributors, ODbL.
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
#include <vector>

extern "C" {
#include "../../../src/ui/biggps/bgalert_format.h"
}

namespace {

bool is(const char *v, const char *s) { return v && !std::strcmp(v, s); }

double bx0 = -180, by0 = -90, bx1 = 180, by1 = 90;
std::vector<bga_entry_t> out;
unsigned counts[BGA_KINDS];

uint32_t world_x(double lon) { return (uint32_t)std::llround(std::min(std::max((lon + 180.0) / 360.0 * 4294967296.0, 0.0), 4294967295.0)); }
uint32_t world_y(double lat)
{
    double r = std::max(std::min(lat, 85.0511), -85.0511) * M_PI / 180.0;
    double y = (1.0 - std::log(std::tan(r) + 1.0 / std::cos(r)) / M_PI) / 2.0 * 4294967296.0;
    return (uint32_t)std::llround(std::min(std::max(y, 0.0), 4294967295.0));
}

// maxspeed: plain km/h numbers only ("60", "60 km/h"); anything else (mph, "BR:urban") = unknown
uint8_t speed(const char *v)
{
    if (!v) return 0;
    char *end;
    long n = std::strtol(v, &end, 10);
    while (*end == ' ') end++;
    if (end == v || n <= 0 || n > 250 || (*end && std::strcmp(end, "km/h") && std::strcmp(end, "kmh"))) return 0;
    return (uint8_t)n;
}

// direction: degrees or a compass point; "forward"/"backward"/"both" are relative to a way we
// do not read here, so they stay unknown
uint16_t direction(const char *v)
{
    if (!v) return BGA_DIR_ANY;
    static const char *pts[] = { "N", "NNE", "NE", "ENE", "E", "ESE", "SE", "SSE",
                                 "S", "SSW", "SW", "WSW", "W", "WNW", "NW", "NNW" };
    for (int i = 0; i < 16; i++)
        if (!std::strcmp(v, pts[i])) return (uint16_t)std::lround(i * 22.5) % 360;
    char *end;
    double d = std::strtod(v, &end);
    if (end == v || *end || d < 0 || d >= 360) return BGA_DIR_ANY;
    return (uint16_t)std::lround(d) % 360;
}

struct H : osmium::handler::Handler {
    void node(const osmium::Node &n)
    {
        const auto &t = n.tags();
        const char *h = t["highway"], *tc = t["traffic_calming"], *rw = t["railway"], *cr = t["crossing"];
        int kind = 0;
        if (is(h, "speed_camera")) kind = BGA_CAMERA;
        else if (is(h, "traffic_signals") || is(cr, "traffic_signals")) kind = is(h, "crossing") ? 0 : BGA_SIGNAL;
        else if (tc && (is(tc, "bump") || is(tc, "hump") || is(tc, "table") || is(tc, "cushion")
                        || is(tc, "yes") || is(tc, "speed_bump"))) kind = BGA_BUMP;
        else if (is(rw, "level_crossing")) kind = BGA_RAIL;
        if (!kind || !n.location().valid()) return;
        double lon = n.location().lon(), lat = n.location().lat();
        if (lon < bx0 || lon > bx1 || lat < by0 || lat > by1) return;
        bga_entry_t e;
        e.x = world_x(lon);
        e.y = world_y(lat);
        e.kind = (uint8_t)kind;
        e.maxspeed = kind == BGA_CAMERA ? speed(t["maxspeed"]) : 0;
        const char *d = t["direction"];
        if (!d) d = t["camera:direction"];
        e.dir = direction(d);
        out.push_back(e);
        counts[kind]++;
    }
};

void put32(FILE *f, uint32_t v) { unsigned char b[4] = { (unsigned char)v, (unsigned char)(v >> 8), (unsigned char)(v >> 16), (unsigned char)(v >> 24) }; std::fwrite(b, 1, 4, f); }

} // namespace

int main(int argc, char **argv)
{
    const char *bbox = argc == 4 && !std::strncmp(argv[3], "--bbox=", 7) ? argv[3] + 7 : argc == 5 && !std::strcmp(argv[3], "--bbox") ? argv[4] : nullptr;
    if ((argc != 3 && !bbox) || (bbox && std::sscanf(bbox, "%lf,%lf,%lf,%lf", &bx0, &by0, &bx1, &by1) != 4)) {
        std::fprintf(stderr, "uso: %s IN.osm.pbf OUT.bga [--bbox MINLON,MINLAT,MAXLON,MAXLAT]\n", argv[0]);
        return 2;
    }
    osmium::io::Reader rd{argv[1], osmium::osm_entity_bits::node};
    H h;
    osmium::apply(rd, h);
    rd.close();
    std::sort(out.begin(), out.end(), [](const bga_entry_t &a, const bga_entry_t &b) { return a.x != b.x ? a.x < b.x : a.y < b.y; });
    std::string tmp = std::string(argv[2]) + ".tmp";
    FILE *f = std::fopen(tmp.c_str(), "wb");
    if (!f) { std::perror(argv[2]); return 1; }
    std::fwrite(BGA_MAGIC, 1, 4, f);
    put32(f, BGA_VERSION);
    put32(f, (uint32_t)out.size());
    put32(f, (uint32_t)std::time(nullptr));
    for (const auto &e : out) {
        put32(f, e.x);
        put32(f, e.y);
        unsigned char b[4] = { e.kind, e.maxspeed, (unsigned char)e.dir, (unsigned char)(e.dir >> 8) };
        std::fwrite(b, 1, 4, f);
    }
    if (std::fclose(f) || std::rename(tmp.c_str(), argv[2])) { std::perror(argv[2]); return 1; }
    unsigned cam_speed = 0;
    for (const auto &e : out) cam_speed += e.kind == BGA_CAMERA && e.maxspeed;
    std::printf("%s: %zu pontos — semáforos %u, radares %u (%u com velocidade no próprio nó), lombadas %u, passagens de nível %u\n",
                argv[2], out.size(), counts[BGA_SIGNAL], counts[BGA_CAMERA], cam_speed, counts[BGA_BUMP], counts[BGA_RAIL]);
    return 0;
}
