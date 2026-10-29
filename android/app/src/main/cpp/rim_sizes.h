#pragma once
// Every rim size on every car (Garage, build 18): the tyre sizes Speed.exe 1.4 lacks, generated on the phone
// from the wheel models the game has, and handed to the game in a wheel pack the host serves in place of its
// Cars/WHEELS/GEOMETRY.BIN: the game's solids and the new ones in one solid list, and the new parts in the
// pack's resident section.
//
// The game's rims: 41 styles (ENKEI_STYLE10 ...), each in 12 (inner, outer) sizes, inches of diameter:
// (15,23) (16,23) (16,24) (17,23) (17,24) (17,25) (18,24) (18,25) (18,26) (19,25) (19,26) (20,26). A car's
// outer size (CarTypeInfo +0xDC: 23-26) is its tyre's, so a car can wear three inner sizes of each style (the
// rim browser 0x50D420 shows one: the size its tier gives a style). This makes the other (inner 15-20, outer
// 23-26) pairs: 12 per style, 3 solids each (LOD A, B, C), 1,476 solids and 492 parts, so that every car can
// wear all six inner sizes on its own tyre size.
//
// A wheel solid (0x80134010, read from the file and the game's loaders 0x53FEF0/0x53FFD0) draws the rim, the
// tyre and a back disc from one texture atlas. Measured on the game's own variants:
//   - the tread is at outer * 0.0127 m (one inch of diameter); the tyre's bead (inner edge) at a radius set by
//     the inner size; the rim, below the bead, is the same in every outer size of an inner size;
//   - the back disc is a square whose corners reach about 1.41 x the tread, scaled with the outer size;
//   - each LOD draws at its own radial scale (the C solids are polygons about 8 % outside the true circle).
// So a new (inner, outer) comes from the game's (inner, outer') nearest in outer: by radius alone, whatever
// the materials (some C solids draw everything as RUBBER), nothing below the bead moves, the tyre between bead
// and tread stretches to the new tread, and the disc corners scale with outer / outer'. Topology, UVs, normals
// and colours are the donor's. Checked against the game's own variants: tread and disc exact, intermediate
// sidewall rings within a few mm (EA modelled each size by hand).
//
// The parts database (chunk 0x80034602: header 0x34603, strings 0x34606, attributes 0x34605, 48-byte records
// 0x34604; loader 0x57C8B0) accepts several databases, each linked into 0x73AD40 after the last. A new part is
// its donor's record (display name such as "BBS CH 18", brand, levels, TEXTURE_NAME) with OUTER_RADIUS
// changed, the part hash BinHash("STYLE_INNER_OUTER") (the game's own scheme for 35 of the 41 styles) and the
// three new solids. The database carries GLOBALB's strings unchanged: the game keeps only the last loaded
// database's strings for string attributes. Saves keep installed parts by part hash.
//
// Header-only and free of game data: tests/rim_sizes.cpp builds a synthetic wheel file; tools check it
// against the real files offline.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <tuple>
#include <vector>

namespace rim_sizes {

constexpr uint32_t kGeneratorVersion = 2;    // part of the cache key: a change here regenerates the file
constexpr double kInchRadius = 0.0127;       // radius of one inch of diameter, metres
constexpr int kInnerMin = 15, kInnerMax = 20, kOuterMin = 23, kOuterMax = 26;
constexpr uint32_t kSolidList = 0x80134000u, kSolidListHeader = 0x80134001u, kSolidInfo = 0x134002u,
                   kSolidIndex = 0x134003u, kSolidStream = 0x134004u, kSolidListEnd = 0x80134008u,
                   kSolid = 0x80134010u, kSolidHeader = 0x134011u, kSolidMaterials = 0x134013u,
                   kMesh = 0x80134100u, kVertices = 0x134B01u, kGroups = 0x134B02u, kIndices = 0x134B03u;
constexpr uint32_t kPartsDb = 0x80034602u, kPartsHeader = 0x34603u, kPartsStrings = 0x34606u,
                   kPartsAttributes = 0x34605u, kPartsRecords = 0x34604u;
constexpr uint32_t kRubber = 0x78743CA1u;    // light material RUBBER: the tyre
constexpr uint32_t kRimGroup = 17;
constexpr size_t kVertexBytes = 36, kGroupBytes = 60, kRecordBytes = 48, kSolidHeaderBytes = 0xB0;

inline uint32_t binHash(const char* text) {
    uint32_t hash = 0xFFFFFFFFu;
    for (const unsigned char* c = reinterpret_cast<const unsigned char*>(text); *c; ++c) hash = hash * 33u + *c;
    return hash;
}
inline uint32_t binHash(const std::string& text) { return binHash(text.c_str()); }

inline uint32_t read32(const std::vector<uint8_t>& d, size_t at) { uint32_t v; std::memcpy(&v, d.data() + at, 4); return v; }
inline void write32(std::vector<uint8_t>& d, size_t at, uint32_t v) { std::memcpy(d.data() + at, &v, 4); }
inline float readF(const std::vector<uint8_t>& d, size_t at) { float v; std::memcpy(&v, d.data() + at, 4); return v; }
inline void writeF(std::vector<uint8_t>& d, size_t at, float v) { std::memcpy(d.data() + at, &v, 4); }

struct Chunk { uint32_t id; size_t offset, size; };   // offset of the chunk header; size of its payload

// The chunks between [begin, end); false when one runs past `end` (a damaged or foreign file).
inline bool chunks(const std::vector<uint8_t>& d, size_t begin, size_t end, std::vector<Chunk>& out) {
    out.clear();
    if (end > d.size()) return false;
    for (size_t at = begin; at + 8 <= end;) {
        Chunk c{read32(d, at), at, read32(d, at + 4)};
        if (c.size > end - at - 8) return false;
        out.push_back(c);
        at += 8 + c.size;
    }
    return true;
}

inline const Chunk* find(const std::vector<Chunk>& list, uint32_t id) {
    for (const Chunk& c : list) if (c.id == id) return &c;
    return nullptr;
}

// Mesh arrays start at the next file offset that is a multiple of their alignment (the file is loaded
// aligned); the bytes before are 0x11 filler.
inline size_t alignedLead(size_t payload, size_t alignment) { return (alignment - payload % alignment) % alignment; }

struct Group { uint32_t material; uint32_t start, count; size_t record; };   // record: file offset

struct Solid {
    std::string name, style;
    int inner = 0, outer = 0;
    char lod = 0;
    size_t offset = 0, size = 0;          // the 0x80134010 chunk: header offset, payload size
    size_t header = 0;                    // payload offset of 0x134011
    size_t vertexData = 0, vertexCount = 0;
    std::vector<uint16_t> indices;
    std::vector<Group> groups;
};

// Splits "ENKEI_STYLE10_18_24_A" into style, inner, outer, LOD; false for any other name.
inline bool parseName(const std::string& name, std::string& style, int& inner, int& outer, char& lod) {
    size_t c = name.rfind('_');
    if (c == std::string::npos || c + 2 != name.size()) return false;
    size_t b = name.rfind('_', c - 1), a = b == std::string::npos ? b : name.rfind('_', b - 1);
    if (a == std::string::npos || a == 0) return false;
    auto number = [&](size_t from, size_t to, int& out) {
        if (to <= from || to - from > 2) return false;
        out = 0;
        for (size_t i = from; i < to; ++i) { if (name[i] < '0' || name[i] > '9') return false; out = out * 10 + (name[i] - '0'); }
        return true;
    };
    lod = name[c + 1];
    style = name.substr(0, a);
    return number(a + 1, b, inner) && number(b + 1, c, outer) && (lod == 'A' || lod == 'B' || lod == 'C');
}

// Every wheel solid of every solid list in the file; false when the file does not read as one.
inline bool readSolids(const std::vector<uint8_t>& d, std::vector<Solid>& out, std::string& error) {
    out.clear();
    std::vector<Chunk> top, inner, sub, mesh;
    if (!chunks(d, 0, d.size(), top)) { error = "not a chunk file"; return false; }
    for (const Chunk& list : top) {
        if (list.id != kSolidList) continue;
        if (!chunks(d, list.offset + 8, list.offset + 8 + list.size, inner)) { error = "damaged solid list"; return false; }
        for (const Chunk& c : inner) {
            if (c.id != kSolid) continue;
            if (!chunks(d, c.offset + 8, c.offset + 8 + c.size, sub)) { error = "damaged solid"; return false; }
            const Chunk* h = find(sub, kSolidHeader);
            const Chunk* m = find(sub, kMesh);
            const Chunk* mats = find(sub, kSolidMaterials);
            if (!h || h->size < kSolidHeaderBytes || !m || !mats) continue;
            Solid s;
            s.offset = c.offset; s.size = c.size; s.header = h->offset + 8;
            const char* name = reinterpret_cast<const char*>(d.data() + s.header + 0x94);
            s.name.assign(name, strnlen(name, 28));
            if (!parseName(s.name, s.style, s.inner, s.outer, s.lod)) continue;
            if (!chunks(d, m->offset + 8, m->offset + 8 + m->size, mesh)) { error = "damaged mesh in " + s.name; return false; }
            const Chunk* v = find(mesh, kVertices);
            const Chunk* g = find(mesh, kGroups);
            const Chunk* ix = find(mesh, kIndices);
            if (!v || !g || !ix) { error = "incomplete mesh in " + s.name; return false; }
            size_t vl = alignedLead(v->offset + 8, 128), il = alignedLead(ix->offset + 8, 16), gl = alignedLead(g->offset + 8, 16);
            if (vl > v->size || il > ix->size || gl > g->size) { error = "mesh arrays out of their chunks in " + s.name; return false; }
            s.vertexData = v->offset + 8 + vl;
            s.vertexCount = (v->size - vl) / kVertexBytes;
            for (size_t i = 0; i + 2 <= ix->size - il; i += 2) {
                uint16_t value; std::memcpy(&value, d.data() + ix->offset + 8 + il + i, 2); s.indices.push_back(value);
            }
            std::vector<uint32_t> materials;
            for (size_t i = 0; i + 8 <= mats->size; i += 8) materials.push_back(read32(d, mats->offset + 8 + i));
            for (size_t r = gl; r + kGroupBytes <= g->size; r += kGroupBytes) {
                size_t rec = g->offset + 8 + r;
                uint32_t count = read32(d, rec + 12), slot = read32(d, rec + 32), start = read32(d, rec + 52);
                if (uint64_t(start) + count > s.indices.size()) { error = "material group past the indices in " + s.name; return false; }
                s.groups.push_back({slot < materials.size() ? materials[slot] : 0u, start, count, rec});
            }
            for (const Group& gr : s.groups)
                for (uint32_t i = gr.start; i < gr.start + gr.count; ++i)
                    if (s.indices[i] >= s.vertexCount) { error = "index past the vertices in " + s.name; return false; }
            out.push_back(std::move(s));
        }
    }
    if (out.empty()) { error = "no wheel solids"; return false; }
    return true;
}

inline double radiusAt(const std::vector<uint8_t>& d, const Solid& s, size_t i) {
    size_t at = s.vertexData + i * kVertexBytes;
    return std::hypot(double(readF(d, at)), double(readF(d, at + 8)));
}

// The tyre's bead: the smallest radius of the RUBBER vertices (measured on LOD A, where RUBBER is the tyre).
inline double beadRadius(const std::vector<uint8_t>& d, const Solid& s) {
    double bead = 0; bool any = false;
    for (const Group& g : s.groups) {
        if (g.material != kRubber) continue;
        for (uint32_t i = g.start; i < g.start + g.count; ++i) {
            double r = radiusAt(d, s, s.indices[i]);
            if (!any || r < bead) { bead = r; any = true; }
        }
    }
    return any ? bead : -1;
}

// The largest radius short of the back disc's corners.
inline double treadRadius(const std::vector<uint8_t>& d, const Solid& s, int outer) {
    double limit = outer * kInchRadius * 1.25, tread = 0;
    for (size_t i = 0; i < s.vertexCount; ++i) { double r = radiusAt(d, s, i); if (r < limit && r > tread) tread = r; }
    return tread;
}

// The donor's 0x80134010 chunk (header included), renamed and remapped to `outerTo`.
inline std::vector<uint8_t> remapSolid(const std::vector<uint8_t>& d, const Solid& s, double beadA, int outerFrom, int outerTo,
                                       const std::string& name) {
    std::vector<uint8_t> chunk(d.begin() + s.offset, d.begin() + s.offset + 8 + s.size);
    const size_t base = s.offset;
    double treadFrom = treadRadius(d, s, outerFrom);
    double scale = treadFrom / (outerFrom * kInchRadius);
    double bead = beadA * scale, treadTo = scale * outerTo * kInchRadius;
    double k = (treadTo - bead) / (treadFrom - bead);
    double disc = double(outerTo) / double(outerFrom);
    std::vector<float> pos(s.vertexCount * 3);
    for (size_t i = 0; i < s.vertexCount; ++i) {
        size_t at = s.vertexData + i * kVertexBytes;
        float x = readF(d, at), y = readF(d, at + 4), z = readF(d, at + 8);
        double r = std::hypot(double(x), double(z)), f = 1.0;
        if (r > treadFrom * 1.05) f = disc;
        else if (r > bead) f = (bead + (r - bead) * k) / r;
        pos[3 * i] = float(double(x) * f); pos[3 * i + 1] = y; pos[3 * i + 2] = float(double(z) * f);
        writeF(chunk, at - base, pos[3 * i]);
        writeF(chunk, at - base + 8, pos[3 * i + 2]);
    }
    auto box = [&](auto&& each, float* mn, float* mx) {
        bool any = false;
        each([&](size_t i) {
            for (int a = 0; a < 3; ++a) {
                float v = pos[3 * i + a];
                if (!any || v < mn[a]) mn[a] = v;
                if (!any || v > mx[a]) mx[a] = v;
            }
            any = true;
        });
        return any;
    };
    size_t hb = s.header - base;
    write32(chunk, hb + 0x10, binHash(name));
    float mn[3], mx[3];
    if (box([&](auto&& f) { for (size_t i = 0; i < s.vertexCount; ++i) f(i); }, mn, mx)) {
        for (int a = 0; a < 3; ++a) { writeF(chunk, hb + 0x20 + 4 * a, mn[a]); writeF(chunk, hb + 0x30 + 4 * a, mx[a]); }
        writeF(chunk, hb + 0x2C, 0.f); writeF(chunk, hb + 0x3C, 0.f);
    }
    std::memset(chunk.data() + hb + 0x94, 0, 28);
    std::memcpy(chunk.data() + hb + 0x94, name.data(), std::min<size_t>(name.size(), 27));
    for (const Group& g : s.groups) {
        if (!g.count) continue;   // an empty group keeps its box
        if (box([&](auto&& f) { for (uint32_t i = g.start; i < g.start + g.count; ++i) f(s.indices[i]); }, mn, mx))
            for (int a = 0; a < 3; ++a) { writeF(chunk, g.record - base + 4 * a, mn[a]); writeF(chunk, g.record - base + 16 + 4 * a, mx[a]); }
    }
    return chunk;
}

inline void appendChunk(std::vector<uint8_t>& out, uint32_t id, const std::vector<uint8_t>& payload) {
    size_t at = out.size();
    out.resize(at + 8 + payload.size());
    write32(out, at, id); write32(out, at + 4, uint32_t(payload.size()));
    if (!payload.empty()) std::memcpy(out.data() + at + 8, payload.data(), payload.size());
}

// A padding chunk (id 0, zeros) from the end of `out` to the next multiple of `alignment` past `fileBase +
// out.size()`; one alignment further when the gap is too small for a chunk header.
inline void padTo(std::vector<uint8_t>& out, size_t fileBase, size_t alignment) {
    size_t pos = fileBase + out.size(), target = (pos + alignment - 1) / alignment * alignment;
    if (target - pos && target - pos < 8) target += alignment;
    if (target == pos) return;
    appendChunk(out, 0, std::vector<uint8_t>(target - pos - 8, 0));
}

struct PartsDb { std::vector<uint8_t> header, strings, attributes, records; };

inline bool readPartsDb(const std::vector<uint8_t>& d, PartsDb& db, std::string& error) {
    std::vector<Chunk> top, sub;
    if (!chunks(d, 0, d.size(), top)) { error = "GLOBALB is not a chunk file"; return false; }
    for (const Chunk& c : top) {
        if (c.id != kPartsDb) continue;
        if (!chunks(d, c.offset + 8, c.offset + 8 + c.size, sub)) { error = "damaged parts database"; return false; }
        const Chunk *h = find(sub, kPartsHeader), *s = find(sub, kPartsStrings), *a = find(sub, kPartsAttributes), *r = find(sub, kPartsRecords);
        if (!h || !s || !a || !r || h->size < 32) { error = "incomplete parts database"; return false; }
        auto take = [&](const Chunk* x) { return std::vector<uint8_t>(d.begin() + x->offset + 8, d.begin() + x->offset + 8 + x->size); };
        db = {take(h), take(s), take(a), take(r)};
        return true;
    }
    error = "no parts database in GLOBALB";
    return false;
}

// EA's JDLZ (the game's .lzc files): "JDLZ", flags, the unpacked size at +8, the stream from +16. False for
// anything else or a stream that would read or copy out of bounds.
inline bool jdlzDecompress(const std::vector<uint8_t>& in, std::vector<uint8_t>& out) {
    if (in.size() < 16 || std::memcmp(in.data(), "JDLZ", 4) != 0) return false;
    uint32_t size = read32(in, 8);
    if (size > (1u << 28)) return false;
    out.assign(size, 0);
    size_t ip = 16, op = 0;
    unsigned flags1 = 1, flags2 = 1;
    while (ip < in.size() && op < size) {
        if (flags1 == 1) { flags1 = in[ip++] | 0x100u; if (ip > in.size()) return false; }
        if (flags2 == 1) { if (ip >= in.size()) return false; flags2 = in[ip++] | 0x100u; }
        if (flags1 & 1) {
            if (ip + 2 > in.size()) return false;
            size_t length, distance;
            if (flags2 & 1) { length = (in[ip + 1] | ((in[ip] & 0xF0u) << 4)) + 3; distance = (in[ip] & 0x0Fu) + 1; }
            else { distance = (in[ip + 1] | ((in[ip] & 0xE0u) << 3)) + 17; length = (in[ip] & 0x1Fu) + 3; }
            ip += 2;
            if (distance > op) return false;
            for (size_t i = 0; i < length && op < size; ++i, ++op) out[op] = out[op - distance];
            flags2 >>= 1;
        } else {
            if (ip >= in.size()) return false;
            out[op++] = in[ip++];
        }
        flags1 >>= 1;
    }
    return op == size;
}

// The part hashes generate() would add, from the solid names alone (cheap: no remapping). Used to tell
// whether a save wears one of them.
inline std::vector<uint32_t> plannedPartHashes(const std::vector<uint8_t>& wheels) {
    std::vector<uint32_t> out;
    std::vector<Solid> solids;
    std::string error;
    if (!readSolids(wheels, solids, error)) return out;
    std::map<std::pair<std::string, int>, std::set<int>> outers;
    for (const Solid& s : solids) outers[{s.style, s.inner}].insert(s.outer);
    for (auto& [key, have] : outers) {
        if (key.second < kInnerMin || key.second > kInnerMax) continue;
        for (int outer = kOuterMin; outer <= kOuterMax; ++outer)
            if (!have.count(outer)) out.push_back(binHash(key.first + "_" + std::to_string(key.second) + "_" + std::to_string(outer)));
    }
    return out;
}

// Whether any of `hashes` appears, at any byte offset, in `data` (a save file).
inline bool containsAny(const std::vector<uint8_t>& data, const std::vector<uint32_t>& hashes) {
    std::set<uint32_t> set(hashes.begin(), hashes.end());
    for (size_t i = 0; i + 4 <= data.size(); ++i) if (set.count(read32(data, i))) return true;
    return false;
}

// The payload offset of a solid chunk's 0x134011 header inside `chunk` (the chunk's own bytes, header first).
inline size_t solidHeaderIn(const std::vector<uint8_t>& chunk) {
    std::vector<Chunk> sub;
    if (!chunks(chunk, 8, chunk.size(), sub)) return 0;
    const Chunk* h = find(sub, kSolidHeader);
    return h ? h->offset + 8 : 0;
}

struct Result {
    std::vector<uint8_t> file;          // the wheel file to serve in place of the game's
    unsigned stockSolids = 0;           // the game's solids, carried over unchanged
    unsigned solids = 0, parts = 0;     // what was added
    std::vector<uint32_t> partHashes;   // the parts added, in database order (a save wearing one needs this file)
    size_t databaseAt = 0, databaseBytes = 0;   // the resident section: the parts database chunk
    std::string error;                  // empty on success
};

// The game's file as it ships: one 0x80134000 container at offset 0 holding an empty padding chunk, then the
// 0x80134001 header (where the streamer's header locator 0x5405B0 looks), then the solids. False otherwise.
inline bool stockLayout(const std::vector<uint8_t>& d, std::vector<uint8_t>& info, std::string& error) {
    std::vector<Chunk> top, inner, header;
    if (!chunks(d, 0, d.size(), top) || top.empty() || top[0].id != kSolidList || top[0].offset != 0) {
        error = "the wheel file does not start with a solid list";
        return false;
    }
    if (!chunks(d, 8, 8 + top[0].size, inner) || inner.size() < 2 || inner[0].id != 0 || inner[1].id != kSolidListHeader) {
        error = "the wheel file's solid list does not start with its header";
        return false;
    }
    for (size_t i = 1; i < top.size(); ++i)
        if (top[i].id == kSolidList) { error = "the wheel file has more than one solid list"; return false; }
    if (!chunks(d, inner[1].offset + 8, inner[1].offset + 8 + inner[1].size, header)) { error = "damaged solid list header"; return false; }
    const Chunk* i = find(header, kSolidInfo);
    if (!i || i->size < 0x74) { error = "no solid list info"; return false; }
    info.assign(d.begin() + i->offset + 8, d.begin() + i->offset + 8 + i->size);
    if (read32(info, 0x6C) != 0) { error = "the wheel file already has a resident section"; return false; }
    return true;
}

// wheels: Cars/WHEELS/GEOMETRY.BIN as installed; globalb: GLOBALB as the game loads it (GlobalB.lzc unpacked).
//
// The wheel file is a streamed pack (registered at boot by 0x448650 -> 0x53EED0 with the solid streamer at
// 0x743C20): the game reads only its header - the solid count, an index of solid hashes and a stream table
// {hash, file offset, bytes} - and streams each solid in when a car wears it. So the result is one solid list
// like the game's, whose header lists the game's solids and the new ones, every one at a 128-byte boundary, in
// hash order as the game's are (index and stream table both sorted). The parts database goes in the pack's
// resident section, info +0x68 (offset) and +0x6C (bytes), which every car's GEOMETRY.BIN uses for its own
// resident chunk: the streamer loads it as a bundle when it registers the pack (0x53F220), and the database
// loader 0x57C8B0 links it after GLOBALB's, before the boot finishes waiting on its loads.
inline Result generate(const std::vector<uint8_t>& wheels, const std::vector<uint8_t>& globalb) {
    Result result;
    std::vector<uint8_t> info;
    if (!stockLayout(wheels, info, result.error)) return result;
    std::vector<Solid> solids;
    if (!readSolids(wheels, solids, result.error)) return result;
    PartsDb db;
    if (!readPartsDb(globalb, db, result.error)) return result;
    // the game's rim parts by their LOD A solid hash, and every part hash (no collision allowed)
    const uint32_t wheelsCar = binHash("WHEELS"), keyOuter = binHash("OUTER_RADIUS");
    std::map<uint32_t, size_t> donorParts;
    std::set<uint32_t> partHashes;
    for (size_t r = 0; r + kRecordBytes <= db.records.size(); r += kRecordBytes) {
        partHashes.insert(read32(db.records, r + 8));
        if (read32(db.records, r + 4) == wheelsCar && db.records[r + 0x10] == kRimGroup) donorParts[read32(db.records, r + 0x20)] = r;
    }
    std::map<std::tuple<std::string, int, int, char>, const Solid*> byKey;
    std::map<std::pair<std::string, int>, std::set<int>> outers;
    std::set<std::string> styles;
    std::set<uint32_t> solidHashes;
    for (const Solid& s : solids) {
        byKey[{s.style, s.inner, s.outer, s.lod}] = &s;
        outers[{s.style, s.inner}].insert(s.outer);
        styles.insert(s.style);
        solidHashes.insert(read32(wheels, s.header + 0x10));
    }
    // every solid chunk the pack will hold, keyed by its hash: the game's as they are, then the new ones
    struct Piece { uint32_t hash; const uint8_t* bytes; size_t size; };
    std::vector<std::vector<uint8_t>> made;
    std::vector<Piece> pieces;
    std::vector<Chunk> top, inner;
    chunks(wheels, 0, wheels.size(), top);
    chunks(wheels, 8, 8 + top[0].size, inner);
    for (const Chunk& c : inner)
        if (c.id == kSolid) {
            std::vector<Chunk> sub;
            chunks(wheels, c.offset + 8, c.offset + 8 + c.size, sub);
            const Chunk* h = find(sub, kSolidHeader);
            if (!h || h->size < kSolidHeaderBytes) { result.error = "a solid without its header"; return result; }
            pieces.push_back({read32(wheels, h->offset + 8 + 0x10), wheels.data() + c.offset, 8 + c.size});
        }
    result.stockSolids = unsigned(pieces.size());
    struct NewPart { std::string base; int outer; size_t donor; };
    std::vector<NewPart> newParts;
    for (const std::string& style : styles) {
        for (int inner = kInnerMin; inner <= kInnerMax; ++inner) {
            auto have = outers.find({style, inner});
            if (have == outers.end()) continue;
            for (int outer = kOuterMin; outer <= kOuterMax; ++outer) {
                if (have->second.count(outer)) continue;
                int donor = 0, best = 1 << 30;   // the nearest outer size, the larger one on a tie
                for (int o : have->second) { int cost = std::abs(o - outer) * 2 + (o < outer); if (cost < best) { best = cost; donor = o; } }
                std::string base = style + "_" + std::to_string(inner) + "_" + std::to_string(outer);
                auto a = byKey.find({style, inner, donor, 'A'});
                if (a == byKey.end()) continue;
                double beadA = beadRadius(wheels, *a->second);
                if (beadA <= 0) continue;
                auto part = donorParts.find(read32(wheels, a->second->header + 0x10));
                if (part == donorParts.end() || partHashes.count(binHash(base))) continue;
                bool all = true;
                for (char lod : {'A', 'B', 'C'}) {
                    auto s = byKey.find({style, inner, donor, lod});
                    if (s == byKey.end() || solidHashes.count(binHash(base + "_" + lod))) { all = false; break; }
                }
                if (!all) continue;
                for (char lod : {'A', 'B', 'C'}) {
                    std::string name = base + "_" + lod;
                    made.push_back(remapSolid(wheels, *byKey[{style, inner, donor, lod}], beadA, donor, outer, name));
                    solidHashes.insert(binHash(name));
                }
                newParts.push_back({base, outer, part->second});
                partHashes.insert(binHash(base));
            }
        }
    }
    if (newParts.empty()) { result.error = "every size is already there"; return result; }
    for (const auto& m : made) pieces.push_back({read32(m, solidHeaderIn(m) + 0x10), m.data(), m.size()});
    std::stable_sort(pieces.begin(), pieces.end(), [](const Piece& a, const Piece& b) { return a.hash < b.hash; });
    for (size_t i = 1; i < pieces.size(); ++i)
        if (pieces[i].hash == pieces[i - 1].hash) { result.error = "two solids share a hash"; return result; }

    // --- the parts database: header, strings, attributes, records (the game's order) ---------------------
    // The strings are GLOBALB's, byte for byte, and each record keeps its donor's name offset: the loader
    // leaves the last database's strings in [0x734584], and the game resolves every part's string attributes
    // there (0x57CC30: the vinyls' texture names), whichever database the part is in.
    std::vector<uint8_t> attributes, records;
    for (const NewPart& p : newParts) {
        std::vector<uint8_t> rec(db.records.begin() + p.donor, db.records.begin() + p.donor + kRecordBytes);
        write32(rec, 8, binHash(p.base));
        uint32_t index = read32(rec, 0x14);
        uint16_t count, second;
        std::memcpy(&count, rec.data() + 0x1C, 2); std::memcpy(&second, rec.data() + 0x1E, 2);
        uint32_t start = uint32_t(attributes.size() / 8);
        for (uint32_t a = 0; a < count && second == 0; ++a) {
            size_t at = size_t(index + a) * 8;
            if (at + 8 > db.attributes.size()) break;
            uint32_t key = read32(db.attributes, at), value = read32(db.attributes, at + 4);
            if (key == keyOuter) value = uint32_t(p.outer);
            size_t o = attributes.size(); attributes.resize(o + 8); write32(attributes, o, key); write32(attributes, o + 4, value);
        }
        uint16_t written = uint16_t(attributes.size() / 8 - start), none = 0;
        write32(rec, 0x14, start); write32(rec, 0x18, 0);
        std::memcpy(rec.data() + 0x1C, &written, 2); std::memcpy(rec.data() + 0x1E, &none, 2);
        const char* lods = "ABC";
        for (int l = 0; l < 3; ++l) write32(rec, 0x20 + 4 * l, binHash(p.base + "_" + lods[l]));
        write32(rec, 0x2C, 0);
        records.insert(records.end(), rec.begin(), rec.end());
        result.partHashes.push_back(binHash(p.base));
    }
    std::vector<uint8_t> header(32, 0);
    write32(header, 8, read32(db.header, 8));                  // the database version
    write32(header, 0x10, uint32_t(newParts.size()));
    write32(header, 0x18, uint32_t(attributes.size() / 8));
    std::vector<uint8_t> dbBody;
    appendChunk(dbBody, kPartsHeader, header);
    appendChunk(dbBody, kPartsStrings, db.strings);
    appendChunk(dbBody, kPartsAttributes, attributes);
    appendChunk(dbBody, kPartsRecords, records);

    // --- the pack: one solid list, then the resident section --------------------------------------------------
    const size_t n = pieces.size();
    write32(info, 0xC, uint32_t(n));
    std::vector<uint8_t> index(n * 8, 0), stream(n * 20, 0);
    for (size_t i = 0; i < n; ++i) { write32(index, 8 * i, pieces[i].hash); write32(stream, 20 * i, pieces[i].hash); }
    // file offsets: the header's size depends only on n, so the solids' places are known before it is written
    const size_t headerBody = (8 + info.size()) + (8 + index.size()) + (8 + stream.size()) + 8;
    size_t at = 8 + 8 + 8 + headerBody;                      // container, padding chunk, header chunk
    auto alignedFrom = [](size_t pos, size_t alignment) {
        size_t target = (pos + alignment - 1) / alignment * alignment;
        if (target - pos && target - pos < 8) target += alignment;
        return target;
    };
    std::vector<size_t> where(n);
    for (size_t i = 0; i < n; ++i) {
        at = alignedFrom(at, 128);
        where[i] = at;
        write32(stream, 20 * i + 4, uint32_t(at));
        write32(stream, 20 * i + 8, uint32_t(pieces[i].size));
        at += pieces[i].size;
    }
    const size_t containerEnd = at;
    result.databaseAt = alignedFrom(containerEnd, 128);
    result.databaseBytes = 8 + dbBody.size();
    write32(info, 0x68, uint32_t(result.databaseAt));
    write32(info, 0x6C, uint32_t(result.databaseBytes));

    std::vector<uint8_t>& out = result.file;
    out.reserve(alignedFrom(result.databaseAt + result.databaseBytes, 16) + 16);
    out.resize(8);
    write32(out, 0, kSolidList);
    appendChunk(out, 0, {});
    std::vector<uint8_t> headerChunk;
    appendChunk(headerChunk, kSolidInfo, info);
    appendChunk(headerChunk, kSolidIndex, index);
    appendChunk(headerChunk, kSolidStream, stream);
    appendChunk(headerChunk, kSolidListEnd, {});
    appendChunk(out, kSolidListHeader, headerChunk);
    for (size_t i = 0; i < n; ++i) {
        padTo(out, 0, 128);
        if (out.size() != where[i]) { result.error = "layout mismatch"; result.file.clear(); return result; }
        out.insert(out.end(), pieces[i].bytes, pieces[i].bytes + pieces[i].size);
    }
    write32(out, 4, uint32_t(out.size() - 8));
    padTo(out, 0, 128);
    if (out.size() != result.databaseAt) { result.error = "layout mismatch"; result.file.clear(); return result; }
    appendChunk(out, kPartsDb, dbBody);
    padTo(out, 0, 16);
    result.solids = unsigned(made.size());
    result.parts = unsigned(newParts.size());
    return result;
}

}  // namespace rim_sizes
