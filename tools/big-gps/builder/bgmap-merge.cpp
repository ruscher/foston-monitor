// bgmap-merge.cpp - join Big GPS map parts (bgmap-build --levels / --chunk11) into one pack.
//
//   bgmap-merge OUT.bgm PART1.bgm PART2.bgm ...
//
// Levels with the same zoom are joined; a tile present in two parts is an error (the parts
// must not overlap).  Every part is checked like the device does (magic, version, index).
// FAT32 limits a file to 4 GB: past ~3.9 GB the output continues in OUT-2.bgm, OUT-3.bgm…
// (tile-disjoint files of one map; the device opens every .bgm of the folder together).
// SPDX-License-Identifier: GPL-3.0-or-later
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <map>
#include <string>
#include <vector>

extern "C" {
#include "../../../src/ui/biggps/bgmap_format.h"
}

struct Part {
    std::string path;
    FILE *f;
    bgm_header_t h;
    std::vector<bgm_level_t> lv;
};

struct Src { size_t part; uint32_t off, len; };

int main(int argc, char **argv)
{
    if (argc < 3) { std::fprintf(stderr, "uso: %s SAIDA.bgm PARTE.bgm...\n", argv[0]); return 2; }
    std::vector<Part> parts;
    std::map<int, std::map<uint32_t, Src>> levels;    // zoom -> key -> source
    uint32_t bb[4] = {UINT32_MAX, UINT32_MAX, 0, 0};
    for (int i = 2; i < argc; i++) {
        Part p{argv[i], std::fopen(argv[i], "rb"), {}, {}};
        if (!p.f || std::fread(&p.h, sizeof p.h, 1, p.f) != 1 || p.h.magic != BGM_MAGIC || p.h.version != BGM_VERSION
            || p.h.nlevels < 1 || p.h.nlevels > BGM_MAX_LEVELS) {
            std::fprintf(stderr, "%s: não é um mapa do Big GPS\n", argv[i]);
            return 1;
        }
        p.lv.resize(p.h.nlevels);
        if (std::fread(p.lv.data(), sizeof(bgm_level_t), p.h.nlevels, p.f) != p.h.nlevels) { std::fprintf(stderr, "%s: cabeçalho curto\n", argv[i]); return 1; }
        for (auto &l : p.lv) {
            std::vector<bgm_index_t> idx(l.ntiles);
            std::fseek(p.f, l.index_off, SEEK_SET);
            if (std::fread(idx.data(), sizeof(bgm_index_t), l.ntiles, p.f) != l.ntiles) { std::fprintf(stderr, "%s: índice curto\n", argv[i]); return 1; }
            auto &dst = levels[l.zoom];
            for (auto &e : idx) {
                if (dst.count(e.key)) {
                    std::fprintf(stderr, "tile z%d %u/%u em duas partes (%s e %s)\n", l.zoom, e.key & 0xFFFF, e.key >> 16,
                                 parts[dst[e.key].part].path.c_str(), argv[i]);
                    return 1;
                }
                dst[e.key] = {parts.size(), e.off, e.len};
            }
        }
        bb[0] = std::min(bb[0], p.h.bbox[0]); bb[1] = std::min(bb[1], p.h.bbox[1]);
        bb[2] = std::max(bb[2], p.h.bbox[2]); bb[3] = std::max(bb[3], p.h.bbox[3]);
        parts.push_back(p);
    }
    /* BGM_SPLIT_MB: smaller files, to test the split */
    const char *lim_env = getenv("BGM_SPLIT_MB");
    const uint64_t LIMIT = (lim_env && atoi(lim_env) > 1 ? (uint64_t)atoi(lim_env) : 3900ull) << 20;
    const char attr[] = "Dados do mapa \xC2\xA9 colaboradores do OpenStreetMap, licen\xC3\xA7" "a ODbL 1.0 (openstreetmap.org/copyright)";
    std::string base(argv[1]);
    if (base.size() > 4 && base.compare(base.size() - 4, 4, ".bgm") == 0) base.resize(base.size() - 4);
    int fileno_ = 0;
    FILE *out = nullptr;
    uint64_t pos = 0;
    std::map<int, std::vector<bgm_index_t>> idx;           // zoom -> entries of the current file
    std::string cur_path;
    auto open_next = [&]() {
        fileno_++;
        cur_path = fileno_ == 1 ? std::string(argv[1]) : base + "-" + std::to_string(fileno_) + ".bgm";
        out = std::fopen(cur_path.c_str(), "wb");
        if (!out) { std::perror(cur_path.c_str()); std::exit(1); }
        /* room for the header and up to BGM_MAX_LEVELS levels, written at the end */
        bgm_header_t h{};
        bgm_level_t lv[BGM_MAX_LEVELS]{};
        std::fwrite(&h, sizeof h, 1, out);
        std::fwrite(lv, sizeof lv, 1, out);
        pos = sizeof h + sizeof lv;
        idx.clear();
    };
    auto finish = [&]() {
        std::vector<bgm_level_t> lv;
        for (auto &kv : idx) {
            bgm_level_t l{};
            l.zoom = (uint8_t)kv.first;
            l.ntiles = (uint32_t)kv.second.size();
            l.index_off = (uint32_t)pos;
            std::fwrite(kv.second.data(), sizeof(bgm_index_t), kv.second.size(), out);
            pos += kv.second.size() * sizeof(bgm_index_t);
            lv.push_back(l);
        }
        bgm_header_t h{};
        h.magic = BGM_MAGIC;
        h.version = BGM_VERSION;
        h.nlevels = (uint32_t)lv.size();
        h.attr_off = (uint32_t)pos;
        std::memcpy(h.bbox, bb, sizeof bb);
        h.created = (uint32_t)time(nullptr);
        std::fwrite(attr, 1, sizeof attr, out);
        /* the level table sits right after the header (unused slots stay zero) */
        std::fseek(out, 0, SEEK_SET);
        std::fwrite(&h, sizeof h, 1, out);
        std::fwrite(lv.data(), sizeof(bgm_level_t), lv.size(), out);
        if (std::fclose(out)) { std::perror(cur_path.c_str()); std::exit(1); }
        std::fprintf(stderr, "%s: %llu bytes, %zu níveis\n", cur_path.c_str(), (unsigned long long)(pos + sizeof attr), lv.size());
    };
    open_next();
    std::vector<char> buf;
    for (auto &kv : levels) {
        size_t n = 0;
        for (auto &t : kv.second) {
            if (pos + t.second.len + (1u << 20) > LIMIT) { finish(); open_next(); }
            buf.resize(t.second.len);
            FILE *f = parts[t.second.part].f;
            std::fseek(f, t.second.off, SEEK_SET);
            if (std::fread(buf.data(), 1, buf.size(), f) != buf.size()) { std::fprintf(stderr, "erro ao ler %s\n", parts[t.second.part].path.c_str()); return 1; }
            std::fwrite(buf.data(), 1, buf.size(), out);
            idx[kv.first].push_back({t.first, (uint32_t)pos, (uint32_t)buf.size()});
            pos += buf.size();
            n++;
        }
        std::fprintf(stderr, "nível z%d: %zu tiles\n", kv.first, n);
    }
    finish();
    std::fprintf(stderr, "%d arquivo(s), %zu partes de entrada\n", fileno_, parts.size());
    return 0;
}
