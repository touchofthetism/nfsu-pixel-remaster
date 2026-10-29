// Every rim size: the wheel pack generator (android/app/src/main/cpp/rim_sizes.h) on a synthetic wheel file
// and GLOBALB built here (no game data), JDLZ, and the game side (src/runtime/nfs_rims.h): the parts lookup
// against a transcription of Speed.exe's 0x57CE10 on random databases, and the rim browser's list.
#include "../android/app/src/main/cpp/rim_sizes.h"
#include "../src/runtime/nfs_rims.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <random>
#include <set>
#include <string>
#include <vector>

static int failures = 0;
static void check(bool ok, const char* what) {
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", what); ++failures; }
}

using Bytes = std::vector<uint8_t>;
static void put32(Bytes& d, uint32_t v) { size_t at = d.size(); d.resize(at + 4); rim_sizes::write32(d, at, v); }
static void putF(Bytes& d, float v) { uint32_t u; std::memcpy(&u, &v, 4); put32(d, u); }

// --- a synthetic wheel file in the game's layout ---------------------------------------------------------------
constexpr uint32_t kMag = 0x22719FA9u, kPlain = 0x010CB64Au;

struct TestSolid { std::string name; int inner, outer; };

// Radii of a test wheel: rim, bead (the tyre's inner edge), the sidewall's middle, tread, back disc corners.
static double rimR(int inner) { return inner * 0.0127 * 0.8; }
static double beadR(int inner) { return inner * 0.0127; }
static double treadR(int outer) { return outer * 0.0127; }

// One solid chunk appended at the next 128-byte boundary of `file`, its mesh arrays at the boundaries the
// game's files keep (vertices 128, groups and indices 16), each ring 8 vertices.
static void appendSolid(Bytes& file, const TestSolid& s) {
    rim_sizes::padTo(file, 0, 128);
    const size_t chunkAt = file.size();
    Bytes header(rim_sizes::kSolidHeaderBytes, 0);
    rim_sizes::write32(header, 0x10, rim_sizes::binHash(s.name));
    std::memcpy(header.data() + 0x94, s.name.data(), std::min<size_t>(s.name.size(), 27));
    Bytes materials;
    for (uint32_t m : {rim_sizes::kRubber, kMag, kPlain}) { put32(materials, m); put32(materials, 0); }
    // vertices: rings at the rim (MAG), bead, sidewall middle, tread (RUBBER), and 4 disc corners (PLAIN)
    std::vector<std::array<float, 3>> pos;
    auto ring = [&](double r, float y) { for (int k = 0; k < 8; ++k) pos.push_back({float(r * std::cos(k * 0.785398)), y, float(r * std::sin(k * 0.785398))}); };
    ring(rimR(s.inner), 0.f);                                 // 0..7
    ring(beadR(s.inner), 0.02f);                              // 8..15
    ring((beadR(s.inner) + treadR(s.outer)) / 2, 0.05f + 0.001f * float(s.outer));   // 16..23 (y names the tyre size)
    ring(treadR(s.outer), 0.08f);                             // 24..31
    for (int k = 0; k < 4; ++k) {                             // 32..35
        double r = treadR(s.outer) * 1.41, a = 0.785398 + k * 1.570796;
        pos.push_back({float(r * std::cos(a)), 0.2f, float(r * std::sin(a))});
    }
    std::vector<uint16_t> indices;
    auto quads = [&](int a, int b) { for (int k = 0; k < 8; ++k) { int k2 = (k + 1) % 8; indices.insert(indices.end(), {uint16_t(a + k), uint16_t(b + k), uint16_t(b + k2)}); } };
    quads(0, 0); const size_t rimCount = indices.size();          // MAG: the rim ring
    quads(8, 16); quads(16, 24); const size_t tyreCount = indices.size() - rimCount;   // RUBBER
    indices.insert(indices.end(), {32, 33, 34, 32, 34, 35});      // PLAIN: the disc
    if (indices.size() % 2) indices.push_back(0);
    struct G { uint32_t slot, start, count; };
    std::vector<G> groups = {{1, 0, uint32_t(rimCount)}, {0, uint32_t(rimCount), uint32_t(tyreCount)}, {2, uint32_t(rimCount + tyreCount), 6}};

    // The chunk: header, texture, materials, then the mesh whose arrays depend on their file offsets.
    Bytes chunk;
    rim_sizes::appendChunk(chunk, rim_sizes::kSolidHeader, header);
    rim_sizes::appendChunk(chunk, 0x134012u, Bytes{1, 2, 3, 4, 0, 0, 0, 0});
    rim_sizes::appendChunk(chunk, rim_sizes::kSolidMaterials, materials);
    const size_t meshAt = chunkAt + 8 + chunk.size();                 // file offset of the mesh chunk
    Bytes mesh;
    rim_sizes::appendChunk(mesh, 0x134900u, Bytes(16, 0x11));
    auto aligned = [&](uint32_t id, size_t alignment, const Bytes& body) {
        const size_t payload = meshAt + 8 + mesh.size() + 8;          // file offset of the child's payload
        Bytes withLead(rim_sizes::alignedLead(payload, alignment), 0x11);
        withLead.insert(withLead.end(), body.begin(), body.end());
        rim_sizes::appendChunk(mesh, id, withLead);
    };
    Bytes v;
    for (auto& p : pos) { putF(v, p[0]); putF(v, p[1]); putF(v, p[2]); putF(v, 0.f); putF(v, 1.f); putF(v, 0.f); put32(v, 0xFFFFFFFFu); putF(v, 0.5f); putF(v, 0.25f); }
    aligned(rim_sizes::kVertices, 128, v);
    Bytes g;
    for (const G& gr : groups) {
        Bytes rec(60, 0);
        rim_sizes::write32(rec, 12, gr.count); rim_sizes::write32(rec, 32, gr.slot); rim_sizes::write32(rec, 52, gr.start);
        rim_sizes::write32(rec, 44, 0x4180);
        g.insert(g.end(), rec.begin(), rec.end());
    }
    aligned(rim_sizes::kGroups, 16, g);
    Bytes ix(indices.size() * 2);
    std::memcpy(ix.data(), indices.data(), ix.size());
    aligned(rim_sizes::kIndices, 16, ix);
    rim_sizes::appendChunk(chunk, rim_sizes::kMesh, mesh);
    rim_sizes::appendChunk(file, rim_sizes::kSolid, chunk);
}

// The game's layout: one container, an empty padding chunk, the header (info, index, stream table), the
// solids in hash order at 128-byte boundaries.
static Bytes buildWheels(std::vector<TestSolid> solids) {
    std::sort(solids.begin(), solids.end(), [](const TestSolid& a, const TestSolid& b) { return rim_sizes::binHash(a.name) < rim_sizes::binHash(b.name); });
    const size_t n = solids.size();
    Bytes info(0x80, 0);
    rim_sizes::write32(info, 8, 0x1C); rim_sizes::write32(info, 0xC, uint32_t(n));
    std::memcpy(info.data() + 0x10, "..\\PC\\CD\\CARS\\WHEELS\\GEOMETRY.BIN", 33);
    std::memcpy(info.data() + 0x48, "DEFAULT", 7);
    rim_sizes::write32(info, 0x70, 0x80);
    Bytes index(n * 8, 0), stream(n * 20, 0);
    // two passes: the stream table's offsets come from where the solids land
    Bytes file;
    for (int pass = 0; pass < 2; ++pass) {
        file.assign(8, 0);
        rim_sizes::write32(file, 0, rim_sizes::kSolidList);
        rim_sizes::appendChunk(file, 0, {});
        Bytes headerBody;
        rim_sizes::appendChunk(headerBody, rim_sizes::kSolidInfo, info);
        rim_sizes::appendChunk(headerBody, rim_sizes::kSolidIndex, index);
        rim_sizes::appendChunk(headerBody, rim_sizes::kSolidStream, stream);
        rim_sizes::appendChunk(headerBody, rim_sizes::kSolidListEnd, {});
        rim_sizes::appendChunk(file, rim_sizes::kSolidListHeader, headerBody);
        for (size_t i = 0; i < n; ++i) {
            const uint32_t hash = rim_sizes::binHash(solids[i].name);
            appendSolid(file, solids[i]);
            rim_sizes::write32(index, 8 * i, hash);
            rim_sizes::write32(stream, 20 * i, hash);
        }
        // the offsets of this pass's solids, for the next one (the header's size does not change)
        std::vector<rim_sizes::Chunk> inner;
        rim_sizes::write32(file, 4, uint32_t(file.size() - 8));
        rim_sizes::chunks(file, 8, file.size(), inner);
        size_t k = 0;
        for (const auto& c : inner)
            if (c.id == rim_sizes::kSolid) { rim_sizes::write32(stream, 20 * k + 4, uint32_t(c.offset)); rim_sizes::write32(stream, 20 * k + 8, uint32_t(c.size + 8)); ++k; }
    }
    rim_sizes::write32(file, 4, uint32_t(file.size() - 8));
    rim_sizes::padTo(file, 0, 128);
    return file;
}

// A GLOBALB with one parts database: the rim parts of `solids` (by their LOD A) plus a car's part.
struct TestPart { std::string base, display; int inner, outer, level; uint32_t texture; };
static Bytes buildGlobal(const std::vector<TestPart>& parts) {
    Bytes strings, attributes, records;
    const uint32_t wheels = rim_sizes::binHash("WHEELS"), keyInner = rim_sizes::binHash("INNER_RADIUS"),
                   keyOuter = rim_sizes::binHash("OUTER_RADIUS"), keyTexture = rim_sizes::binHash("TEXTURE_NAME");
    struct Rec { uint32_t car; Bytes bytes; };
    std::vector<Rec> recs;
    for (const TestPart& p : parts) {
        const uint32_t nameAt = uint32_t(strings.size());
        strings.insert(strings.end(), p.display.begin(), p.display.end()); strings.push_back(0);
        const uint32_t attrAt = uint32_t(attributes.size() / 8);
        for (auto kv : {std::pair<uint32_t, uint32_t>{keyInner, uint32_t(p.inner)}, {keyOuter, uint32_t(p.outer)}, {keyTexture, p.texture}}) {
            put32(attributes, kv.first); put32(attributes, kv.second);
        }
        Bytes r(48, 0);
        rim_sizes::write32(r, 0, nameAt); rim_sizes::write32(r, 4, wheels); rim_sizes::write32(r, 8, rim_sizes::binHash(p.base));
        rim_sizes::write32(r, 0xC, 0x9536); r[0x10] = 17; r[0x11] = uint8_t(p.level);
        rim_sizes::write32(r, 0x14, attrAt); r[0x1C] = 3;
        for (int l = 0; l < 3; ++l) rim_sizes::write32(r, 0x20 + 4 * l, rim_sizes::binHash(p.base + "_" + "ABC"[l]));
        recs.push_back({wheels, r});
    }
    Bytes car(48, 0);   // a car's own part, sorted among the rims by its car hash
    const uint32_t carHash = rim_sizes::binHash("TESTCAR");
    rim_sizes::write32(car, 4, carHash); rim_sizes::write32(car, 8, rim_sizes::binHash("TESTCAR_HOOD")); car[0x10] = 5;
    recs.push_back({carHash, car});
    std::stable_sort(recs.begin(), recs.end(), [](const Rec& a, const Rec& b) { return a.car < b.car; });
    for (auto& r : recs) records.insert(records.end(), r.bytes.begin(), r.bytes.end());
    Bytes header(32, 0);
    rim_sizes::write32(header, 8, 5); rim_sizes::write32(header, 0x10, uint32_t(recs.size())); rim_sizes::write32(header, 0x18, uint32_t(attributes.size() / 8));
    Bytes db;
    rim_sizes::appendChunk(db, rim_sizes::kPartsHeader, header);
    rim_sizes::appendChunk(db, rim_sizes::kPartsStrings, strings);
    rim_sizes::appendChunk(db, rim_sizes::kPartsAttributes, attributes);
    rim_sizes::appendChunk(db, rim_sizes::kPartsRecords, records);
    Bytes global;
    rim_sizes::appendChunk(global, 0x34600u, Bytes(0x20, 0));   // something else first, as in the game's
    rim_sizes::appendChunk(global, rim_sizes::kPartsDb, db);
    return global;
}

static void testGenerator() {
    // Two styles: ONE in (16,24) (17,24) (18,24) (17,25); TWO in (15,23) (16,24). Each in LOD A, B, C.
    std::vector<TestSolid> solids;
    std::vector<TestPart> parts;
    auto add = [&](const std::string& style, const std::string& display, int inner, int outer, int level, uint32_t texture) {
        std::string base = style + "_" + std::to_string(inner) + "_" + std::to_string(outer);
        for (char lod : {'A', 'B', 'C'}) solids.push_back({base + "_" + lod, inner, outer});
        parts.push_back({base, display + " " + std::to_string(inner), inner, outer, level, texture});
    };
    add("TEST_STYLE01", "TEST ONE", 16, 24, 1, 111); add("TEST_STYLE01", "TEST ONE", 17, 24, 1, 111);
    add("TEST_STYLE01", "TEST ONE", 18, 24, 1, 111); add("TEST_STYLE01", "TEST ONE", 17, 25, 1, 111);
    add("TEST_STYLE02", "TEST TWO", 15, 23, 3, 222); add("TEST_STYLE02", "TEST TWO", 16, 24, 3, 222);
    add("TEST_STYLE04", "TEST FOUR", 18, 24, 2, 444); add("TEST_STYLE04", "TEST FOUR", 18, 26, 2, 444);   // 25: a tie
    const Bytes wheels = buildWheels(solids), global = buildGlobal(parts);

    std::vector<rim_sizes::Solid> read;
    std::string error;
    check(rim_sizes::readSolids(wheels, read, error) && read.size() == solids.size(), "the synthetic wheel file reads");

    rim_sizes::Result r = rim_sizes::generate(wheels, global);
    check(r.error.empty(), r.error.c_str());
    if (!r.error.empty()) return;
    // ONE: inner 16 lacks 23, 25, 26; 17 lacks 23, 26; 18 lacks 23, 25, 26. TWO: 15 lacks 24-26; 16 lacks 23, 25, 26.
    // FOUR: 18 lacks 23 and 25.
    check(r.parts == 16 && r.solids == 48 && r.stockSolids == solids.size(), "the missing sizes: 16 parts, 48 solids");
    check(r.partHashes.size() == 16, "the parts' hashes");
    std::set<uint32_t> planned;
    for (uint32_t h : rim_sizes::plannedPartHashes(wheels)) planned.insert(h);
    check(planned == std::set<uint32_t>(r.partHashes.begin(), r.partHashes.end()), "the planned hashes are the generated ones");
    check(planned.count(rim_sizes::binHash("TEST_STYLE02_15_26")) && !planned.count(rim_sizes::binHash("TEST_STYLE01_17_24")), "planned by name");

    // The pack the streamer reads: one container, the header right after an empty padding chunk.
    const Bytes& pack = r.file;
    std::vector<rim_sizes::Chunk> top, inner, header;
    check(rim_sizes::chunks(pack, 0, pack.size(), top) && top.size() >= 3, "the pack tiles into chunks");
    check(top[0].id == rim_sizes::kSolidList && top[0].offset == 0, "a solid list at 0");
    rim_sizes::chunks(pack, 8, 8 + top[0].size, inner);
    check(inner[0].id == 0 && inner[0].size == 0 && inner[1].id == rim_sizes::kSolidListHeader && inner[1].offset == 0x10, "padding chunk, then the header at 10h");
    rim_sizes::chunks(pack, inner[1].offset + 8, inner[1].offset + 8 + inner[1].size, header);
    check(header.size() == 4 && header[0].id == rim_sizes::kSolidInfo && header[1].id == rim_sizes::kSolidIndex &&
          header[2].id == rim_sizes::kSolidStream && header[3].id == rim_sizes::kSolidListEnd, "the header's children");
    const size_t infoAt = header[0].offset + 8;
    const uint32_t count = rim_sizes::read32(pack, infoAt + 0xC);
    check(count == solids.size() + r.solids, "the header counts every solid");
    check(rim_sizes::read32(pack, infoAt + 8) == 0x1C && rim_sizes::read32(pack, infoAt + 0x70) == 0x80, "the info is the game's");
    std::vector<uint32_t> hashes;
    for (uint32_t i = 0; i < count; ++i) {
        const uint32_t h = rim_sizes::read32(pack, header[1].offset + 8 + 8 * i);
        check(rim_sizes::read32(pack, header[1].offset + 12 + 8 * i) == 0, "index second words zero");
        const size_t e = header[2].offset + 8 + 20 * i;
        check(rim_sizes::read32(pack, e) == h, "stream table in index order");
        const uint32_t at = rim_sizes::read32(pack, e + 4), bytes = rim_sizes::read32(pack, e + 8);
        check(at % 128 == 0 && rim_sizes::read32(pack, at) == rim_sizes::kSolid && rim_sizes::read32(pack, at + 4) + 8 == bytes, "a stream entry names its solid chunk");
        hashes.push_back(h);
    }
    check(std::is_sorted(hashes.begin(), hashes.end()) && std::adjacent_find(hashes.begin(), hashes.end()) == hashes.end(), "sorted, unique hashes");
    // The game's solids, byte for byte; the new ones by their geometry.
    std::vector<rim_sizes::Solid> stock, all;
    rim_sizes::readSolids(wheels, stock, error);
    check(rim_sizes::readSolids(pack, all, error) && all.size() == count, "the pack's solids read");
    std::map<std::string, const rim_sizes::Solid*> byName;
    for (const auto& s : all) byName[s.name] = &s;
    for (const auto& s : stock) {
        auto p = byName.find(s.name);
        check(p != byName.end() && p->second->size == s.size &&
              std::equal(wheels.begin() + s.offset, wheels.begin() + s.offset + 8 + s.size, pack.begin() + p->second->offset), "a stock solid is carried over unchanged");
    }
    auto radii = [&](const rim_sizes::Solid& s) { std::vector<double> out; for (size_t i = 0; i < s.vertexCount; ++i) out.push_back(rim_sizes::radiusAt(pack, s, i)); return out; };
    // TWO 15 x 26 comes from TWO 15 x 23 (the only outer of inner 15): rim and bead stay, the tread moves to 26.
    for (char lod : {'A', 'B', 'C'}) {
        auto made = byName.find(std::string("TEST_STYLE02_15_26_") + lod);
        check(made != byName.end(), "TEST_STYLE02_15_26 made");
        if (made == byName.end()) continue;
        auto r2 = radii(*made->second);
        check(std::fabs(r2[0] - rimR(15)) < 1e-6 && std::fabs(r2[8] - beadR(15)) < 1e-6, "rim and bead unchanged");
        check(std::fabs(r2[24] - treadR(26)) < 1e-5, "the tread at 26 inches");
        const double k = (treadR(26) - beadR(15)) / (treadR(23) - beadR(15));
        check(std::fabs(r2[16] - (beadR(15) + ((beadR(15) + treadR(23)) / 2 - beadR(15)) * k)) < 1e-5, "the sidewall stretched between bead and tread");
        check(std::fabs(r2[32] - treadR(23) * 1.41 * 26 / 23) < 1e-5, "the disc corners scaled with the outer size");
        check(rim_sizes::read32(pack, made->second->header + 0x10) == rim_sizes::binHash(made->second->name), "the new solid's hash");
        // y, UVs and normals stay the donor's
        float y; std::memcpy(&y, pack.data() + made->second->vertexData + 24 * rim_sizes::kVertexBytes + 4, 4);
        check(y == 0.08f, "axle position kept");
    }
    // The donor is the nearest tyre size, the larger on a tie: ONE 17 has 24 and 25, so 23 comes from 24 and 26
    // from 25 (the sidewall ring's y, copied from the donor, names its tyre size).
    auto sidewallY = [&](const std::string& name) {
        float y = -1; auto it = byName.find(name);
        if (it != byName.end()) std::memcpy(&y, pack.data() + it->second->vertexData + 16 * rim_sizes::kVertexBytes + 4, 4);
        return y;
    };
    check(sidewallY("TEST_STYLE01_17_23_A") == 0.05f + 0.001f * 24 && sidewallY("TEST_STYLE01_17_26_B") == 0.05f + 0.001f * 25,
          "the nearest tyre size gives each new size");
    check(sidewallY("TEST_STYLE04_18_25_C") == 0.05f + 0.001f * 26, "a tie goes to the larger tyre");
    check(byName.count("TEST_STYLE01_16_25_A") && byName.count("TEST_STYLE01_18_26_C"), "ONE's sizes made");
    check(!byName.count("TEST_STYLE01_19_24_A"), "no inner size a style lacks");

    // The resident section: the parts database at info +68h, +6Ch bytes, after the solids.
    const uint32_t dbAt = rim_sizes::read32(pack, infoAt + 0x68), dbBytes = rim_sizes::read32(pack, infoAt + 0x6C);
    check(dbAt == r.databaseAt && dbBytes == r.databaseBytes && dbAt % 16 == 0 && dbAt >= top[0].size + 8, "the resident section after the solids");
    check(rim_sizes::read32(pack, dbAt) == rim_sizes::kPartsDb && rim_sizes::read32(pack, dbAt + 4) + 8 == dbBytes, "exactly the parts database");
    rim_sizes::PartsDb db, source;
    if (uint64_t(dbAt) + dbBytes > pack.size()) { check(false, "the resident section inside the pack"); return; }
    Bytes section(pack.begin() + dbAt, pack.begin() + dbAt + dbBytes);
    if (!rim_sizes::readPartsDb(section, db, error) || !rim_sizes::readPartsDb(global, source, error)) { check(false, "the database reads"); return; }
    check(db.strings == source.strings, "GLOBALB's strings carried");
    check(rim_sizes::read32(db.header, 8) == 5 && rim_sizes::read32(db.header, 0x10) == 16 && rim_sizes::read32(db.header, 0x18) * 8 == db.attributes.size(), "the database header");
    const uint32_t keyOuter = rim_sizes::binHash("OUTER_RADIUS"), keyInner = rim_sizes::binHash("INNER_RADIUS");
    std::set<uint32_t> seen;
    for (size_t k = 0; k + 48 <= db.records.size(); k += 48) {
        const uint32_t part = rim_sizes::read32(db.records, k + 8);
        check(rim_sizes::read32(db.records, k + 4) == rim_sizes::binHash("WHEELS") && db.records[k + 0x10] == 17, "a rim record");
        check(seen.insert(part).second, "unique part hashes");
        const uint32_t attr = rim_sizes::read32(db.records, k + 0x14);
        uint16_t c; std::memcpy(&c, db.records.data() + k + 0x1C, 2);
        int inn = 0, out = 0;
        for (uint32_t a = 0; a < c; ++a) {
            if (rim_sizes::read32(db.attributes, 8 * (attr + a)) == keyInner) inn = int(rim_sizes::read32(db.attributes, 8 * (attr + a) + 4));
            if (rim_sizes::read32(db.attributes, 8 * (attr + a)) == keyOuter) out = int(rim_sizes::read32(db.attributes, 8 * (attr + a) + 4));
        }
        std::string name(reinterpret_cast<const char*>(db.strings.data() + rim_sizes::read32(db.records, k)));
        const std::string display = name.substr(0, name.rfind(' '));
        const std::map<std::string, std::pair<std::string, int>> styles = {
            {"TEST ONE", {"TEST_STYLE01", 1}}, {"TEST TWO", {"TEST_STYLE02", 3}}, {"TEST FOUR", {"TEST_STYLE04", 2}}};
        auto known = styles.find(display);
        check(known != styles.end(), "a display name of the test's styles");
        if (known == styles.end()) continue;
        const std::string style = known->second.first;
        const std::string base = style + "_" + std::to_string(inn) + "_" + std::to_string(out);
        check(part == rim_sizes::binHash(base), "the part hash names style, inner and outer");
        check(name == display + " " + std::to_string(inn), "the donor's display name, its size the part's");
        check(db.records[k + 0x11] == known->second.second, "the style's level");
        for (int l = 0; l < 3; ++l) check(byName.count(base + "_" + "ABC"[l]) && rim_sizes::read32(db.records, k + 0x20 + 4 * l) == rim_sizes::binHash(base + "_" + "ABC"[l]), "LOD hashes name the new solids");
    }
    check(pack.size() % 16 == 0, "the pack ends at a 16-byte boundary");

    // What a save holds: a generated part's hash anywhere in it.
    Bytes save(1000, 0x5A);
    check(!rim_sizes::containsAny(save, r.partHashes), "a save without them");
    rim_sizes::write32(save, 333, r.partHashes[5]);
    check(rim_sizes::containsAny(save, r.partHashes), "a save wearing one (at any offset)");

    // Refusals: a file already carrying sizes, a damaged one, a GLOBALB without parts, nothing to add.
    check(!rim_sizes::generate(pack, global).error.empty(), "a pack with a resident section is not a wheel file to extend");
    Bytes damaged = wheels; rim_sizes::write32(damaged, 4, 0x7FFFFFFF);
    check(!rim_sizes::generate(damaged, global).error.empty(), "a damaged wheel file");
    check(!rim_sizes::generate(wheels, Bytes(64, 0)).error.empty(), "no parts database");
    std::vector<TestSolid> full; std::vector<TestPart> fullParts;
    for (int outer = 23; outer <= 26; ++outer) {
        std::string base = "TEST_STYLE03_17_" + std::to_string(outer);
        for (char lod : {'A', 'B', 'C'}) full.push_back({base + "_" + lod, 17, outer});
        fullParts.push_back({base, "TEST THREE 17", 17, outer, 2, 333});
    }
    check(rim_sizes::generate(buildWheels(full), buildGlobal(fullParts)).error == "every size is already there", "nothing to add");
}

// --- JDLZ ----------------------------------------------------------------------------------------------------
static Bytes jdlz(const Bytes& stream, uint32_t unpacked) {
    Bytes out = {'J', 'D', 'L', 'Z', 2, 0x10, 0, 0};
    put32(out, unpacked); put32(out, uint32_t(16 + stream.size()));
    out.insert(out.end(), stream.begin(), stream.end());
    return out;
}
static void testJdlz() {
    // literals only: flag bytes 0 (and the second flag byte, read once)
    Bytes text = {'w', 'h', 'e', 'e', 'l', 's', '!', '?', 'x'};
    Bytes stream = {0x00, 0x00};
    stream.insert(stream.end(), text.begin(), text.begin() + 8);
    stream.push_back(0x00); stream.push_back(text[8]);
    Bytes out;
    check(rim_sizes::jdlzDecompress(jdlz(stream, 9), out) && out == text, "JDLZ literals");
    // "abcabcabcabc": 3 literals, then a short reference (flags2 bit 0 clear): distance 3 -> (b1 | (b0 & E0) << 3) + 17
    // cannot reach 3, so a long one (flags2 bit 0 set): distance (b0 & 0F) + 1 = 3, length (b1 | (b0 & F0) << 4) + 3 = 9.
    Bytes ref = {0x08 /* flags1: literal, literal, literal, reference */, 0x01 /* flags2: long */, 'a', 'b', 'c', 0x02, 0x06};
    check(rim_sizes::jdlzDecompress(jdlz(ref, 12), out) && std::string(out.begin(), out.end()) == "abcabcabcabc", "JDLZ long reference");
    // a short reference: distance (b1 | (b0 & E0) << 3) + 17 = 17 over 17 literals, length (b0 & 1F) + 3 = 4
    Bytes lit17 = {0x00, 0x00};
    std::string seventeen = "0123456789ABCDEFG";
    for (int i = 0; i < 8; ++i) lit17.push_back(uint8_t(seventeen[i]));
    lit17.push_back(0x00); for (int i = 8; i < 16; ++i) lit17.push_back(uint8_t(seventeen[i]));
    lit17.push_back(0x02 /* literal, then a reference */); lit17.push_back(uint8_t(seventeen[16]));
    lit17.push_back(0x01 /* length 4 */); lit17.push_back(0x00 /* distance 17 */);
    check(rim_sizes::jdlzDecompress(jdlz(lit17, 21), out) && std::string(out.begin(), out.end()) == seventeen + "0123", "JDLZ short reference");
    // a long reference then a short one: the second flag byte steps on with each reference
    Bytes two = {0x00, 0x01 /* flags2: long, then short */};
    for (int i = 0; i < 8; ++i) two.push_back(uint8_t(seventeen[i]));
    two.push_back(0x00); for (int i = 8; i < 16; ++i) two.push_back(uint8_t(seventeen[i]));
    two.push_back(0x06 /* literal, reference, reference */); two.push_back(uint8_t(seventeen[16]));
    two.push_back(0x00); two.push_back(0x00);   // long: distance 1, length 3
    two.push_back(0x01); two.push_back(0x00);   // short: distance 17, length 4
    check(rim_sizes::jdlzDecompress(jdlz(two, 24), out) && std::string(out.begin(), out.end()) == seventeen + "GGG3456", "JDLZ long then short reference");
    // refused: not JDLZ, a reference before its data, a stream that ends early
    check(!rim_sizes::jdlzDecompress(Bytes(32, 0), out), "not JDLZ");
    check(!rim_sizes::jdlzDecompress(jdlz({0x01, 0x01, 0x00, 0x00}, 3), out), "a reference before any output");
    check(!rim_sizes::jdlzDecompress(jdlz({0x00, 0x00, 'a'}, 4), out), "ends early");
    check(!rim_sizes::jdlzDecompress(Bytes{'J', 'D', 'L', 'Z'}, out), "a header cut short");
}

// --- the game side: guest memory and the parts lookup ------------------------------------------------------------
struct Fake {
    std::map<uint32_t, uint8_t> bytes;
    uint32_t next = 0x10000000u;
    uint32_t alloc(uint32_t n) { uint32_t at = next; next += (n + 15) & ~15u; for (uint32_t i = 0; i < n; ++i) bytes[at + i] = 0; return at; }
    uint32_t r32(uint32_t va) const {
        uint32_t v = 0;
        for (int i = 0; i < 4; ++i) { auto b = bytes.find(va + i); if (b == bytes.end()) { std::fprintf(stderr, "FAIL: read of unmapped %08x\n", va + i); std::exit(1); } v |= uint32_t(b->second) << (8 * i); }
        return v;
    }
    void w32(uint32_t va, uint32_t v) { for (int i = 0; i < 4; ++i) bytes[va + i] = uint8_t(v >> (8 * i)); }
};
static uint32_t fakeRead(void* c, uint32_t va) { return static_cast<Fake*>(c)->r32(va); }
static void fakeWrite(void* c, uint32_t va, uint32_t v) { static_cast<Fake*>(c)->w32(va, v); }
static uint32_t fakeAlloc(void* c, uint32_t n) { return static_cast<Fake*>(c)->alloc(n); }

// Speed.exe 0x57CE10 as it is, for comparison: the walk over the databases, the binary search 0x53E400 and
// the record loop, transcribed from the disassembly (labels in the comments).
static uint32_t gameLookup(const Fake& f, uint32_t head, uint32_t car, int32_t group, uint32_t part, uint32_t prev, int32_t level) {
    for (uint32_t ebx = f.r32(head); ebx != head; ebx = f.r32(ebx)) {               // 57CE30 .. 57CEB5
        const uint32_t ecx = f.r32(ebx + 0x10), recs = f.r32(ebx + 0xC), esi = recs + ecx * 48;
        uint32_t eax;
        if (prev) {                                                                   // 57CE42
            if (prev < recs || prev >= esi) continue;                                 // jb/jae 57CEB5
            eax = prev + 0x30;
        } else if (car) {                                                             // 57CE53
            eax = 0;                                                                  // 53E400
            if (recs && int32_t(ecx) > 0) {
                int32_t lo = 0, hi = int32_t(ecx) - 1;
                while (lo <= hi) {
                    int32_t mid = (lo + hi) / 2;
                    uint32_t at = recs + uint32_t(mid) * 48, key = f.r32(at + 4);
                    if (car < key) hi = mid - 1;
                    else if (car > key) lo = mid + 1;
                    else { while (mid > 0 && f.r32(at - 48 + 4) == car) { at -= 48; --mid; } eax = at; break; }
                }
            }
            if (!eax) return 0;                                                       // je 57CEBF
        } else eax = recs;
        for (; eax < esi; eax += 0x30) {                                              // 57CE72 .. 57CEB3
            if (car && f.r32(eax + 4) != car) return 0;                               // jne 57CEBF
            const uint32_t b = f.r32(eax + 0x10);
            if (group != -1 && int32_t(int8_t(b & 0xFF)) != group) continue;
            if (part && f.r32(eax + 8) != part) continue;
            if (level != -1 && int32_t((b >> 8) & 0xFF) != level) continue;
            return eax;
        }
    }
    return 0;
}

// The extended search's meaning, independently: every record in list order (database by database, record by
// record) that a database's own search would visit for `car`, after `prev`, matching the filters.
static uint32_t everyDatabase(const Fake& f, uint32_t head, uint32_t car, int32_t group, uint32_t part, uint32_t prev, int32_t level) {
    bool past = prev == 0;
    for (uint32_t node = f.r32(head); node != head; node = f.r32(node)) {
        const uint32_t n = f.r32(node + 0x10), recs = f.r32(node + 0xC);
        for (uint32_t i = 0; i < n; ++i) {
            const uint32_t rec = recs + 48 * i;
            if (!past) { if (rec == prev) past = true; continue; }
            if (car && f.r32(rec + 4) != car) continue;
            const uint32_t b = f.r32(rec + 0x10);
            if (group != -1 && int32_t(int8_t(b & 0xFF)) != group) continue;
            if (part && f.r32(rec + 8) != part) continue;
            if (level != -1 && int32_t((b >> 8) & 0xFF) != level) continue;
            return rec;
        }
    }
    return 0;
}

static void testLookup() {
    std::mt19937 rng(18);
    const uint32_t cars[] = {0x0000ABCDu, 0x10000000u, 0x83347C07u, 0x90000000u, 0xFFFF0000u};
    uint64_t compared = 0, differing = 0;
    for (int world = 0; world < 300; ++world) {
        Fake f;
        nfs_guest_memory m = {fakeRead, fakeWrite, fakeAlloc, &f};
        const uint32_t head = f.alloc(8);
        f.w32(head, head); f.w32(head + 4, head);
        std::vector<uint32_t> all;
        const int databases = 1 + int(rng() % 3);
        for (int d = 0; d < databases; ++d) {
            // a database: records sorted by car hash, a few cars each, random groups, parts and levels
            std::vector<std::array<uint32_t, 4>> recs;
            const int count = int(rng() % 12);
            // groups 1-4 and 90h (a negative byte: the game compares it sign-extended)
            for (int i = 0; i < count; ++i) recs.push_back({cars[rng() % 5], uint32_t(rng() % 6), rng() % 5 ? uint32_t(rng() % 4) + 1 : 0x90u, uint32_t(rng() % 3)});
            std::sort(recs.begin(), recs.end(), [](const auto& a, const auto& b) { return a[0] < b[0]; });
            const uint32_t node = f.alloc(32), records = count ? f.alloc(uint32_t(48 * count)) : 0;
            f.w32(node + 0xC, records); f.w32(node + 0x10, uint32_t(count));
            for (int i = 0; i < count; ++i) {
                const uint32_t r = records + 48 * i;
                f.w32(r + 4, recs[i][0]); f.w32(r + 8, recs[i][1]); f.w32(r + 0x10, recs[i][2] | (recs[i][3] << 8));
                all.push_back(r);
            }
            const uint32_t tail = f.r32(head + 4);   // linked at the tail, as 0x57C8B0 does
            f.w32(tail, node); f.w32(head + 4, node); f.w32(node + 4, tail); f.w32(node, head);
        }
        for (int q = 0; q < 200; ++q) {
            const uint32_t car = rng() % 4 ? cars[rng() % 5] : 0;
            const int32_t group = rng() % 3 ? -1 : rng() % 4 ? int32_t(rng() % 4) + 1 : int32_t(int8_t(0x90)) + (rng() % 2 ? 0 : 256);
            const uint32_t part = rng() % 3 ? 0 : rng() % 6;
            const int32_t level = rng() % 3 ? -1 : int32_t(rng() % 3);
            // any record as the one to continue after for the game's search; for the extended one a record of the
            // searched car, as its callers pass (the last one a search with the same car returned)
            const uint32_t prev = rng() % 2 || all.empty() ? 0 : all[rng() % all.size()];
            const uint32_t game = gameLookup(f, head, car, group, part, prev, level);
            check(nfs_parts_lookup(&m, head, car, group, part, prev, level, 0, nullptr) == game, "not extended: the game's lookup, record for record");
            if (prev && car && f.r32(prev + 4) != car) continue;
            const uint32_t extended = nfs_parts_lookup(&m, head, car, group, part, prev, level, 1, nullptr);
            check(extended == everyDatabase(f, head, car, group, part, prev, level), "extended: the next match in every database");
            if (databases == 1) check(extended == game, "one database: the extension changes nothing");
            ++compared; differing += extended != game;
        }
    }
    check(differing > 0 && differing < compared, "the extension matters only sometimes");
    // Iterating as the browser does (prev = the last found): every rim of every database, once each, in order.
    Fake f;
    nfs_guest_memory m = {fakeRead, fakeWrite, fakeAlloc, &f};
    const uint32_t head = f.alloc(8);
    f.w32(head, head); f.w32(head + 4, head);
    std::vector<uint32_t> rims;
    for (int d = 0; d < 2; ++d) {
        // GLOBALB's: a car's part, three rims, a part of a car hashed after WHEELS; the served one: five rims
        const uint32_t node = f.alloc(32), records = f.alloc(48 * 5);
        const uint32_t carHashes[2][5] = {{0x1000u, 0x83347C07u, 0x83347C07u, 0x83347C07u, 0x90000000u},
                                          {0x83347C07u, 0x83347C07u, 0x83347C07u, 0x83347C07u, 0x83347C07u}};
        for (int i = 0; i < 5; ++i) { f.w32(records + 48 * i + 4, carHashes[d][i]); f.w32(records + 48 * i + 0x10, 17); }
        f.w32(node + 0xC, records); f.w32(node + 0x10, 5);
        for (int i = 0; i < 5; ++i) if (f.r32(records + 48 * i + 4) == 0x83347C07u) rims.push_back(records + 48 * i);
        const uint32_t tail = f.r32(head + 4);
        f.w32(tail, node); f.w32(head + 4, node); f.w32(node + 4, tail); f.w32(node, head);
    }
    std::vector<uint32_t> walked, gameWalk;
    uint64_t later = 0;
    for (uint32_t p = nfs_parts_lookup(&m, head, 0x83347C07u, -1, 0, 0, -1, 1, &later); p; p = nfs_parts_lookup(&m, head, 0x83347C07u, -1, 0, p, -1, 1, &later)) walked.push_back(p);
    for (uint32_t p = gameLookup(f, head, 0x83347C07u, -1, 0, 0, -1); p; p = gameLookup(f, head, 0x83347C07u, -1, 0, p, -1)) gameWalk.push_back(p);
    check(walked == rims && walked.size() == 8, "extended: the rims of both databases, in order");
    check(gameWalk.size() == 3, "the game's own walk stops with the first database's rims");
    check(later == 5, "five found past the first database");
    // A broken list (a null link) ends the search instead of reading address 0.
    Fake b;
    nfs_guest_memory mb = {fakeRead, fakeWrite, fakeAlloc, &b};
    const uint32_t h2 = b.alloc(8), n2 = b.alloc(32);
    b.w32(h2, n2); b.w32(n2, 0); b.w32(n2 + 0xC, 0); b.w32(n2 + 0x10, 0);
    check(nfs_parts_lookup(&mb, h2, 1, -1, 0, 0, -1, 1, nullptr) == 0, "a broken list is no match");
}

// --- the rim browser's list ---------------------------------------------------------------------------------------
static void testBrowser() {
    Fake f;
    nfs_guest_memory m = {fakeRead, fakeWrite, fakeAlloc, &f};
    const uint32_t browser = f.alloc(0x90);
    f.w32(browser, NFS_RIM_BROWSER_VTABLE);
    const uint32_t head = browser + NFS_RIM_BROWSER_ENTRIES;
    f.w32(head, head); f.w32(head + 4, head);
    // records with INNER_RADIUS; entries linked as 0x50DD40 does (at the tail), in database order: the game's
    // sizes of styles 1, 2, 3, then the generated sizes of 1, 3, 2
    auto record = [&](uint32_t inner) {
        const uint32_t attrs = f.alloc(24), rec = f.alloc(48);
        f.w32(attrs, NFS_ATTR_INNER_RADIUS); f.w32(attrs + 4, inner);
        f.w32(attrs + 8, 0xCE7D8DB5u); f.w32(attrs + 12, 24);
        f.w32(attrs + 16, NFS_ATTR_TEXTURE_NAME); f.w32(attrs + 20, 7);
        f.w32(rec + 0x14, attrs); f.w32(rec + 0x1C, 3);
        return rec;
    };
    struct E { uint32_t key, inner; };
    const E order[] = {{11, 16}, {11, 17}, {11, 18}, {22, 16}, {22, 17}, {22, 18}, {33, 17}, {11, 15}, {11, 19}, {11, 20},
                       {33, 15}, {33, 20}, {22, 20}, {22, 15}, {22, 19}};
    std::map<uint32_t, std::pair<uint32_t, uint32_t>> entryOf;   // entry -> (key, inner)
    uint32_t installed = 0;
    for (const E& e : order) {
        const uint32_t entry = f.alloc(0x3C), rec = record(e.inner);
        f.w32(entry + NFS_RIM_ENTRY_KEY, e.key); f.w32(entry + NFS_RIM_ENTRY_RECORD, rec);
        const uint32_t tail = f.r32(head + 4);
        f.w32(tail, entry); f.w32(head + 4, entry); f.w32(entry + 4, tail); f.w32(entry, head);
        entryOf[entry] = {e.key, e.inner};
        if (e.key == 22 && e.inner == 19) installed = rec;
    }
    check(nfs_rim_browser_sort(&m, browser) == 1, "the list is put in order");
    std::vector<std::pair<uint32_t, uint32_t>> got;
    uint32_t back = head;
    for (uint32_t e = f.r32(head); e != head; e = f.r32(e)) {
        check(f.r32(e + 4) == back, "prev links follow the next links");
        back = e;
        got.push_back(entryOf[e]);
    }
    check(f.r32(head + 4) == back, "the head's prev is the last entry");
    const std::vector<std::pair<uint32_t, uint32_t>> want = {{11, 15}, {11, 16}, {11, 17}, {11, 18}, {11, 19}, {11, 20}, {22, 15}, {22, 16}, {22, 17},
                                                             {22, 18}, {22, 19}, {22, 20}, {33, 15}, {33, 17}, {33, 20}};
    check(got == want, "by style in the order they first appear, then by inner size");
    check(nfs_rim_browser_sort(&m, browser) == 0, "an ordered list is left as it is");
    const uint32_t own = nfs_rim_browser_find(&m, browser, installed);
    check(own && entryOf[own] == std::make_pair(22u, 19u), "the car's own rim found");
    check(nfs_rim_browser_find(&m, browser, 0) == 0 && nfs_rim_browser_find(&m, browser, 0x12345678u) == 0, "no entry for no rim");

    // Thumbnails: the horizontal browser's items (list head at items+4, key at +8).
    const uint32_t items = browser + NFS_RIM_BROWSER_ITEMS;
    f.w32(items + 4, items + 4); f.w32(items + 8, items + 4);
    check(!nfs_rim_browser_shares_item(&m, items, 11), "no item yet: the style's first size makes one");
    for (uint32_t key : {11u, 22u}) {
        const uint32_t item = f.alloc(0x24);
        f.w32(item + 8, key);
        const uint32_t tail = f.r32(items + 8);
        f.w32(tail, item); f.w32(items + 8, item); f.w32(item + 4, tail); f.w32(item, items + 4);
    }
    check(nfs_rim_browser_shares_item(&m, items, 11) && nfs_rim_browser_shares_item(&m, items, 22), "a listed style's later sizes share its item");
    check(!nfs_rim_browser_shares_item(&m, items, 33), "a new style gets its own");

    // A broken list is left alone.
    Fake g;
    nfs_guest_memory mg = {fakeRead, fakeWrite, fakeAlloc, &g};
    const uint32_t b2 = g.alloc(0x90), h2 = b2 + NFS_RIM_BROWSER_ENTRIES, e2 = g.alloc(0x3C);
    g.w32(h2, e2); g.w32(e2, 0); g.w32(e2 + NFS_RIM_ENTRY_KEY, 1); g.w32(e2 + NFS_RIM_ENTRY_RECORD, 0);
    check(nfs_rim_browser_sort(&mg, b2) == 0 && g.r32(h2) == e2, "a broken list is not relinked");
}

int main() {
    testGenerator();
    testJdlz();
    testLookup();
    testBrowser();
    if (failures) { std::fprintf(stderr, "%d failure(s)\n", failures); return 1; }
    std::printf("rim sizes: generator, JDLZ, parts lookup and rim browser ok\n");
    return 0;
}
