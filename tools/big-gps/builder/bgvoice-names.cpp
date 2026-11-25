// bgvoice-names.cpp - road names for the Big GPS voice pack (runs on the PC).
//
//   bgvoice-names IN.osm.pbf OUT.txt --bbox MINLON,MINLAT,MAXLON,MAXLAT
//
// The name a maneuver speaks is the way's name, or its ref when it has no name
// (bgr_way_name in src/ui/biggps/bgroute.c).  This lists, once each, those of the roads
// (highway=*) with a node inside the box, most frequent first, for make-faber-voice.py --nomes.
//
// SPDX-License-Identifier: GPL-3.0-or-later
#include <osmium/handler.hpp>
#include <osmium/io/pbf_input.hpp>
#include <osmium/visitor.hpp>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace {

double bx0, by0, bx1, by1;
std::unordered_set<osmium::object_id_type> inside;
std::unordered_map<std::string, unsigned> names;

struct Nodes : osmium::handler::Handler {
    void node(const osmium::Node &n)
    {
        if (!n.location().valid()) return;
        double lon = n.location().lon(), lat = n.location().lat();
        if (lon >= bx0 && lon <= bx1 && lat >= by0 && lat <= by1) inside.insert(n.id());
    }
};

struct Ways : osmium::handler::Handler {
    void way(const osmium::Way &w)
    {
        const char *h = w.tags()["highway"];
        if (!h || !std::strcmp(h, "proposed") || !std::strcmp(h, "construction") || !std::strcmp(h, "platform")) return;
        const char *n = w.tags()["name"];
        if (!n) n = w.tags()["ref"];
        if (!n || !*n || std::strlen(n) > 60) return;
        for (const auto &nr : w.nodes())
            if (inside.count(nr.ref())) { names[n]++; return; }
    }
};

} // namespace

int main(int argc, char **argv)
{
    const char *bbox = argc == 4 && !std::strncmp(argv[3], "--bbox=", 7) ? argv[3] + 7 : argc == 5 && !std::strcmp(argv[3], "--bbox") ? argv[4] : nullptr;
    if (!bbox || std::sscanf(bbox, "%lf,%lf,%lf,%lf", &bx0, &by0, &bx1, &by1) != 4) {
        std::fprintf(stderr, "uso: %s IN.osm.pbf SAIDA.txt --bbox MINLON,MINLAT,MAXLON,MAXLAT\n", argv[0]);
        return 2;
    }
    {
        osmium::io::Reader rd{argv[1], osmium::osm_entity_bits::node};
        Nodes h;
        osmium::apply(rd, h);
    }
    {
        osmium::io::Reader rd{argv[1], osmium::osm_entity_bits::way};
        Ways h;
        osmium::apply(rd, h);
    }
    std::vector<std::pair<std::string, unsigned>> v(names.begin(), names.end());
    std::sort(v.begin(), v.end(), [](const auto &a, const auto &b) { return a.second != b.second ? a.second > b.second : a.first < b.first; });
    FILE *f = std::fopen(argv[2], "w");
    if (!f) { std::perror(argv[2]); return 1; }
    for (const auto &e : v) std::fprintf(f, "%s\n", e.first.c_str());
    std::fclose(f);
    std::printf("%s: %zu nomes de vias\n", argv[2], v.size());
    return 0;
}
