#pragma once
// Free roam (build 19): Olympic City and the two drift districts as Free Run tracks, made from the Free Roam Mod
// Beta 2.0 (hockeyguy731, konigsseg, BojanV03; nfsmods #577) and from the player's own installed files. The host
// side: what is served to the game in place of the files it opens for tracks 1000, 1300 and 1400.
//
// Beta 2.0's only new data are its three track bundles, Tracks/TRACKB1000/1300/1400.lzc (raw chunk streams). Each is
// a stock race's bundle (1001, 1301, 1305) with the track polygons (0x34130-0x34133: the drivable surface the
// game takes ground height and collision from) merged over a whole district, more breakable roadside objects and
// the race's track path zones cleared. Everything else the mod ships is a copy of a stock file, so the host serves
// the installed one instead, HD packs included:
//   - STREAML1RB_1300.BUN is STREAML1RB_1301.BUN, STREAML1RC_1400.BUN is STREAML1RC_1305.BUN (the race streams);
//   - TEX1000/1300/1400TRACK.BIN are TEX1001/1301/1308TRACK.BIN (sky textures and the track map). TEX1305 and
//     TEX1308 hold the same sky textures, so here each track's comes from its base race, 1305 for 1400.
// A bundle's section table (0x34107: where each of the region's sections lies in the stream files, its size and
// resident size; 0x34108: the stream files' sizes) is the base race's. An HD tracks pack rebuilds the stream files
// and moves every stock race's table to match, so the served bundle takes the installed base race's table: the
// same sections in the same order, only the numbers moved (checked entry by entry before anything is copied).
//
// The track map: the game's minimap (0x58E540) draws the texture TRACK<n>_MAP of TEX<n>TRACK.BIN, placed by the
// track record's calibration (u = (x - offX) / width, v = (offY - y) / width). Beta 2.0's TEX1000 only has
// TRACK1001_MAP, so its minimap stays off. Here each free roam track gets its own map, made from the game's data:
// the stock race maps of the district as the blueprint background (coarse ones first, finer ones over them),
// and the free roam bundle's road polygons drawn over it in the colour the game draws a race's route in.
//
// The select screen's picture: SELECT LOCATION draws a track's map from the dynamic texture pack Global/DYNTEX.BIN,
// which has none for these tracks (it stayed black); the host serves that pack with theirs added (makePicturePack).
//
// Header-only and free of game data: tests/free_roam.cpp builds synthetic bundles, records and packs.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace free_roam {

constexpr uint32_t kGeneratorVersion = 1;   // part of the cache key: a change here remakes the files
constexpr uint32_t kSectionTable = 0x34107u, kStreamSizes = 0x34108u, kTrackPolygons = 0x34132u;
constexpr size_t kSectionEntryBytes = 56, kPolygonBytes = 32, kTrackRecordBytes = 0xFC;

using Bytes = std::vector<uint8_t>;

inline uint32_t binHash(const char* text) {
    uint32_t hash = 0xFFFFFFFFu;
    for (auto c = reinterpret_cast<const unsigned char*>(text); *c; ++c) hash = hash * 33u + *c;
    return hash;
}
inline uint32_t read32(const uint8_t* d) { uint32_t v; std::memcpy(&v, d, 4); return v; }
inline uint16_t read16(const uint8_t* d) { uint16_t v; std::memcpy(&v, d, 2); return v; }
inline float readF(const uint8_t* d) { float v; std::memcpy(&v, d, 4); return v; }
inline void write32(uint8_t* d, uint32_t v) { std::memcpy(d, &v, 4); }
inline void write16(uint8_t* d, uint16_t v) { std::memcpy(d, &v, 2); }
inline void writeF(uint8_t* d, float v) { std::memcpy(d, &v, 4); }

// ---- the tracks ----------------------------------------------------------------------------------------------

struct Track {
    uint16_t number;              // the free roam track: TRACKB<number>.lzc, TrackInfo +0x6C
    uint16_t base;                // the stock race whose section table, race stream and sky textures it shares
    uint16_t recordBase;          // the stock race whose TrackInfo record its own is made from (freeroam.c)
    uint16_t icon;                // the stock race whose icon (TRACK_<n>_ICON) its track list entry shows
    const char* region;           // TrackInfo +0x40, the stream file STREAM<region>.BUN
    const char* regionDirectory;  // TrackInfo +0x48
    const char* name;             // shown in Free Run (TRACK_NAME_<number>) and in the record's own name fields
    const char* pictureArt;       // the stock background its select screen picture copies, when the game asks for
                                  // the track's own (TRACK_<number>, below 1100); nullptr: the game's choice exists
};
// All three records come from 1001's (Olympic Square in the menus, "CT1 - Market St." in the record), as Beta 2.0's
// do: Free Run's list leaves out a track made from a drift race's record (seen in the test host). The icons: 1003's
// traffic lights (Inner City's) for the city, each drift district its first race's. The city's picture: the whole
// city, the sprints' background (the drift districts get the game's own drift backgrounds).
inline const Track kTracks[] = {
    {1000, 1001, 1001, 1003, "L1RA", "Location1\\RegionA", "Olympic City", "SPRINT_TRACKS_BACK"},
    {1300, 1301, 1001, 1301, "L1RB", "Location1\\RegionB", "Industrial Drift", nullptr},
    {1400, 1305, 1001, 1305, "L1RC", "Location1\\RegionC", "Parkade Drift", nullptr},
};
constexpr unsigned kTrackCount = sizeof(kTracks) / sizeof(kTracks[0]);

inline std::string trackFile(unsigned number) { return "TRACKB" + std::to_string(number) + ".lzc"; }
inline std::string texFile(unsigned number) { return "TEX" + std::to_string(number) + "TRACK.BIN"; }
inline std::string raceStream(const char* region, unsigned number) { return std::string("STREAM") + region + "_" + std::to_string(number) + ".BUN"; }
inline std::string mapName(unsigned number) { return "TRACK" + std::to_string(number) + "_MAP"; }

// ---- chunked bundles ----------------------------------------------------------------------------------------

struct Chunk { uint32_t id; size_t offset; uint32_t size; };   // offset of the 8-byte header; payload size

// The top-level chunks of a bundle; false when one runs past the end (a damaged or foreign file).
inline bool topChunks(const Bytes& d, std::vector<Chunk>& out) {
    out.clear();
    size_t at = 0;
    while (at + 8 <= d.size()) {
        Chunk c{read32(&d[at]), at, read32(&d[at + 4])};
        if (c.size > d.size() - at - 8) return false;
        out.push_back(c);
        at += 8 + size_t(c.size);
    }
    return at == d.size() || d.size() - at < 8;
}
inline const Chunk* findChunk(const std::vector<Chunk>& list, uint32_t id) {
    for (const Chunk& c : list) if (c.id == id) return &c;
    return nullptr;
}

// The first chunk `id` anywhere in a bundle: containers (bit 31 of the id) are searched depth first, as the game
// walks them. False when there is none or the bundle is damaged on the way to it.
inline bool findNested(const Bytes& d, uint32_t id, Chunk& out, size_t begin = 0, size_t end = SIZE_MAX, int depth = 0) {
    if (end > d.size()) end = d.size();
    for (size_t at = begin; at + 8 <= end;) {
        const uint32_t cid = read32(&d[at]), size = read32(&d[at + 4]);
        if (size > end - at - 8) return false;
        if (cid == id) { out = {cid, at, size}; return true; }
        if ((cid & 0x80000000u) && depth < 16 && findNested(d, id, out, at + 8, at + 8 + size, depth + 1)) return true;
        at += 8 + size_t(size);
    }
    return false;
}

// ---- track records (GLOBALB's TrackInfo, chunk 0x34201) --------------------------------------------------------

constexpr uint32_t kTrackInfo = 0x34201u;
constexpr uint32_t kModeDrag = 0x02;         // +0x74 race types: a drag-only track has no minimap

struct TrackRecord {
    uint16_t number = 0;
    std::string region;
    uint32_t modes = 0;
    float mapX = 0, mapY = 0, mapWidth = 0;  // +0x8C/+0x90/+0x94: where the track's map lies
};
inline std::vector<TrackRecord> trackRecords(const Bytes& globalb) {
    std::vector<TrackRecord> records;
    Chunk c;
    if (!findNested(globalb, kTrackInfo, c) || c.size % kTrackRecordBytes) return records;
    for (size_t i = 0; i < c.size / kTrackRecordBytes; ++i) {
        const uint8_t* r = &globalb[c.offset + 8 + i * kTrackRecordBytes];
        TrackRecord t;
        t.number = read16(r + 0x6C);
        t.region.assign(reinterpret_cast<const char*>(r + 0x40), strnlen(reinterpret_cast<const char*>(r + 0x40), 8));
        t.modes = read32(r + 0x74);
        t.mapX = readF(r + 0x8C); t.mapY = readF(r + 0x90); t.mapWidth = readF(r + 0x94);
        records.push_back(std::move(t));
    }
    return records;
}

// The stock races whose maps make a free roam track's map background: the district's races with a minimap (not
// drag-only, not the free roam tracks themselves) and a sane placement, by track number.
inline std::vector<TrackRecord> mapSources(const std::vector<TrackRecord>& records, const Track& track) {
    std::vector<TrackRecord> out;
    for (const TrackRecord& r : records) {
        bool freeRoam = false;
        for (const Track& t : kTracks) freeRoam |= r.number == t.number;
        if (freeRoam || r.region != track.region || !(r.modes & ~kModeDrag)) continue;
        if (!(r.mapWidth > 1 && r.mapWidth < 100000) || !std::isfinite(r.mapX) || !std::isfinite(r.mapY)) continue;
        out.push_back(r);
    }
    std::sort(out.begin(), out.end(), [](const TrackRecord& a, const TrackRecord& b) { return a.number < b.number; });
    return out;
}

// ---- section tables -----------------------------------------------------------------------------------------

// The base race's section table into the free roam bundle, in place. The two tables must describe the same
// sections in the same order (name, number, the race's own flag, which file) and differ only where they lie in
// the streams; the stream sizes chunk goes along. Returns "" or why nothing was copied.
inline std::string transplantSections(Bytes& track, const Bytes& base) {
    std::vector<Chunk> t, b;
    if (!topChunks(track, t)) return "the free roam bundle is damaged";
    if (!topChunks(base, b)) return "the base race's bundle is damaged";
    const Chunk *tt = findChunk(t, kSectionTable), *bt = findChunk(b, kSectionTable);
    const Chunk *ts = findChunk(t, kStreamSizes), *bs = findChunk(b, kStreamSizes);
    if (!tt || !bt || !ts || !bs) return "a section table is missing";
    if (tt->size != bt->size || tt->size % kSectionEntryBytes) return "the section tables differ in size";
    if (ts->size != 12 || bs->size != 12) return "unexpected stream sizes chunk";
    const size_t count = tt->size / kSectionEntryBytes;
    const uint8_t* te = &track[tt->offset + 8];
    const uint8_t* be = &base[bt->offset + 8];
    for (size_t i = 0; i < count; ++i) {
        const uint8_t* x = te + i * kSectionEntryBytes;
        const uint8_t* y = be + i * kSectionEntryBytes;
        if (std::memcmp(x, y, 8) || read16(x + 8) != read16(y + 8) || x[0x0A] != y[0x0A] || read32(x + 0x10) != read32(y + 0x10))
            return "section " + std::to_string(i) + " is not the same section";
    }
    std::memcpy(&track[tt->offset + 8], be, tt->size);
    std::memcpy(&track[ts->offset + 8], &base[bs->offset + 8], 12);
    return "";
}

// Where a track's map lies (TrackInfo +0x8C/+0x90/+0x94, u = (x - offX) / width, v = (offY - y) / width); width 0:
// none. The free roam records themselves are made by the game side (nfs_freeroam_record), from the game's table.
struct MapPlacement { float offX = 0, offY = 0, width = 0; };

// ---- texture packs (TEX<n>TRACK.BIN) --------------------------------------------------------------------------

constexpr uint32_t kTpk = 0xB3300000u, kTpkInfo = 0xB3310000u, kTpkHeader = 0x33310001u, kTpkHashes = 0x33310002u,
                   kTpkCompressed = 0x33310003u, kTpkRecords = 0x33310004u, kTpkFormats = 0x33310005u,
                   kTpkData = 0xB3320000u, kTpkDataHeader = 0x33320001u, kTpkDataBody = 0x33320002u;
constexpr size_t kTexRecordBytes = 124, kTexFormatBytes = 32, kAlign = 128;
constexpr uint8_t kA8R8G8B8 = 0x20, kDXT1 = 0x22, kDXT3 = 0x24, kDXT5 = 0x26;

struct Texture {
    Bytes record, format, pixels, palette;
    std::string name() const { return std::string(reinterpret_cast<const char*>(&record[12]), strnlen(reinterpret_cast<const char*>(&record[12]), 24)); }
    uint32_t hash() const { return read32(&record[0x24]); }
};
struct TexturePack { Bytes header, dataHeader; std::vector<Texture> textures; };

inline size_t mipBytes(uint32_t w, uint32_t h, uint8_t compression) {
    if (compression == kA8R8G8B8) return size_t(w) * h * 4;
    size_t blocks = size_t(std::max(1u, (w + 3) / 4)) * std::max(1u, (h + 3) / 4);
    return blocks * (compression == kDXT1 ? 8 : 16);
}

// A standalone, uncompressed texture pack file (a TEX<n>TRACK.BIN): one 0xB3300000 chunk, padding after it.
inline std::string parsePack(const Bytes& file, TexturePack& pack) {
    std::vector<Chunk> top;
    if (!topChunks(file, top) || top.empty() || top[0].id != kTpk) return "not a texture pack";
    const size_t begin = top[0].offset + 8, end = begin + top[0].size;
    size_t info = 0, infoEnd = 0, data = 0, dataEnd = 0;
    for (size_t at = begin; at + 8 <= end;) {
        uint32_t id = read32(&file[at]), size = read32(&file[at + 4]);
        if (size > end - at - 8) return "texture pack chunk runs past its pack";
        if (id == kTpkInfo) { info = at + 8; infoEnd = at + 8 + size; }
        if (id == kTpkData) { data = at + 8; dataEnd = at + 8 + size; }
        at += 8 + size_t(size);
    }
    if (!info || !data) return "texture pack without info or data";
    const uint8_t *header = nullptr, *hashes = nullptr, *records = nullptr, *formats = nullptr;
    size_t hashBytes = 0, recordBytes = 0, formatBytes = 0;
    for (size_t at = info; at + 8 <= infoEnd;) {
        uint32_t id = read32(&file[at]), size = read32(&file[at + 4]);
        if (size > infoEnd - at - 8) return "texture pack info runs past its chunk";
        const uint8_t* p = &file[at + 8];
        if (id == kTpkHeader && size == kTexRecordBytes) header = p;
        else if (id == kTpkHashes) { hashes = p; hashBytes = size; }
        else if (id == kTpkRecords) { records = p; recordBytes = size; }
        else if (id == kTpkFormats) { formats = p; formatBytes = size; }
        else if (id == kTpkCompressed) return "a streaming texture pack (compressed textures)";
        at += 8 + size_t(size);
    }
    if (!header || !hashes || !records || !formats) return "texture pack tables missing";
    const size_t count = hashBytes / 8;
    if (recordBytes != count * kTexRecordBytes || formatBytes != count * kTexFormatBytes) return "texture pack tables disagree";
    const uint8_t* dataHeader = nullptr;
    size_t bodyPayload = 0, bodyEnd = 0;
    for (size_t at = data; at + 8 <= dataEnd;) {
        uint32_t id = read32(&file[at]), size = read32(&file[at + 4]);
        if (size > dataEnd - at - 8) return "texture data runs past its chunk";
        if (id == kTpkDataHeader && size == 24) dataHeader = &file[at + 8];
        if (id == kTpkDataBody) { bodyPayload = at + 8; bodyEnd = at + 8 + size; }
        at += 8 + size_t(size);
    }
    if (!dataHeader || !bodyPayload) return "texture data without header or body";
    // The game reads the texture data from the first 128-byte file boundary in the body; 0x11 filler before it.
    const size_t base = (bodyPayload + kAlign - 1) / kAlign * kAlign;
    pack.header.assign(header, header + kTexRecordBytes);
    pack.dataHeader.assign(dataHeader, dataHeader + 24);
    pack.textures.clear();
    for (size_t i = 0; i < count; ++i) {
        Texture t;
        t.record.assign(records + i * kTexRecordBytes, records + (i + 1) * kTexRecordBytes);
        t.format.assign(formats + i * kTexFormatBytes, formats + (i + 1) * kTexFormatBytes);
        uint32_t offset = read32(&t.record[0x30]), paletteOffset = read32(&t.record[0x34]),
                 size = read32(&t.record[0x38]), paletteSize = read32(&t.record[0x3C]);
        if (base + offset + size > bodyEnd || (paletteSize && base + paletteOffset + paletteSize > bodyEnd))
            return "texture " + t.name() + " lies outside the pack's data";
        t.pixels.assign(file.begin() + long(base + offset), file.begin() + long(base + offset + size));
        if (paletteSize) t.palette.assign(file.begin() + long(base + paletteOffset), file.begin() + long(base + paletteOffset + paletteSize));
        pack.textures.push_back(std::move(t));
    }
    return "";
}

inline void appendChunk(Bytes& out, uint32_t id, const Bytes& payload) {
    uint8_t h[8]; write32(h, id); write32(h + 4, uint32_t(payload.size()));
    out.insert(out.end(), h, h + 8);
    out.insert(out.end(), payload.begin(), payload.end());
}
inline void appendPadding(Bytes& out, size_t payload) { appendChunk(out, 0, Bytes(payload, 0)); }

// The pack as a file, laid out as the game's own (nfstex's TexturePack.serialize, uncompressed): the textures in
// hash order, palettes then pixel data 128-byte aligned, the info parent's header 16 bytes past a 128-byte
// boundary, the data parent on one, its body starting 8 bytes past one with 120 bytes of 0x11 filler, and the
// 4096-byte padding chunk the game's TEX files end with.
inline Bytes serializePack(TexturePack& pack) {
    std::stable_sort(pack.textures.begin(), pack.textures.end(), [](const Texture& a, const Texture& b) { return a.hash() < b.hash(); });
    Bytes hashes, palettes, pixels, records, formats;
    size_t cursor = 0;
    for (auto& t : pack.textures) {
        write32(&t.record[0x34], uint32_t(cursor));
        write32(&t.record[0x3C], uint32_t(t.palette.size()));
        palettes.insert(palettes.end(), t.palette.begin(), t.palette.end());
        cursor += t.palette.size();
    }
    cursor = (cursor + kAlign - 1) / kAlign * kAlign;
    pixels.assign(cursor - palettes.size(), 0);
    for (size_t i = 0; i < pack.textures.size(); ++i) {
        auto& t = pack.textures[i];
        write32(&t.record[0x30], uint32_t(cursor));
        write32(&t.record[0x38], uint32_t(t.pixels.size()));
        pixels.insert(pixels.end(), t.pixels.begin(), t.pixels.end());
        cursor += t.pixels.size();
        size_t aligned = (cursor + kAlign - 1) / kAlign * kAlign;
        if (i + 1 < pack.textures.size()) { pixels.insert(pixels.end(), aligned - cursor, 0); cursor = aligned; }
        uint8_t pair[8]; write32(pair, t.hash()); write32(pair + 4, 0);
        hashes.insert(hashes.end(), pair, pair + 8);
        records.insert(records.end(), t.record.begin(), t.record.end());
        formats.insert(formats.end(), t.format.begin(), t.format.end());
    }
    const size_t leadingPad = (16 + kAlign * 2 - (8 + 8)) % kAlign;
    const size_t infoSize = 8 + (8 + pack.header.size()) + (8 + hashes.size()) + (8 + records.size()) + (8 + formats.size());
    const size_t afterInfo = 8 + 8 + leadingPad + infoSize;
    const size_t outerPad = (kAlign * 2 - (afterInfo + 8) % kAlign) % kAlign;
    const size_t dataParentAt = afterInfo + 8 + outerPad;
    const size_t afterDataHeader = dataParentAt + 8 + 8 + pack.dataHeader.size();
    const size_t innerPad = (8 + kAlign * 2 - (afterDataHeader + 8 + 8) % kAlign) % kAlign;
    Bytes info, data, body;
    appendChunk(info, kTpkHeader, pack.header);
    appendChunk(info, kTpkHashes, hashes);
    appendChunk(info, kTpkRecords, records);
    appendChunk(info, kTpkFormats, formats);
    body.assign(kAlign - 8, 0x11);
    body.insert(body.end(), palettes.begin(), palettes.end());
    body.insert(body.end(), pixels.begin(), pixels.end());
    appendChunk(data, kTpkDataHeader, pack.dataHeader);
    appendPadding(data, innerPad);
    appendChunk(data, kTpkDataBody, body);
    Bytes tpk;
    appendPadding(tpk, leadingPad);
    appendChunk(tpk, kTpkInfo, info);
    appendPadding(tpk, outerPad);
    appendChunk(tpk, kTpkData, data);
    Bytes file;
    appendChunk(file, kTpk, tpk);
    appendPadding(file, 4096);
    return file;
}

// A texture of `width` x `height`, one mip, replacing the one named `name` or `replacing` (the base track's map),
// else added; the record and format of the replaced texture (or of `like`) keep the engine's flags.
inline bool setTexture(TexturePack& pack, const std::string& name, const std::string& replacing, uint32_t width,
                       uint32_t height, uint8_t compression, Bytes pixels) {
    Texture* target = nullptr;
    for (auto& t : pack.textures) if (t.name() == name || t.name() == replacing) { target = &t; break; }
    if (!target) {
        if (pack.textures.empty()) return false;
        pack.textures.push_back(pack.textures.back());
        target = &pack.textures.back();
    }
    if (pixels.size() != mipBytes(width, height, compression) || (width & (width - 1)) || (height & (height - 1))) return false;
    Bytes& r = target->record;
    std::memset(&r[12], 0, 24);
    std::memcpy(&r[12], name.data(), std::min<size_t>(name.size(), 23));
    write32(&r[0x24], binHash(name.c_str()));
    write16(&r[0x44], uint16_t(width));
    write16(&r[0x46], uint16_t(height));
    unsigned lw = 0, lh = 0;
    while ((1u << lw) < width) ++lw;
    while ((1u << lh) < height) ++lh;
    r[0x48] = uint8_t(lw); r[0x49] = uint8_t(lh);
    r[0x4A] = compression; r[0x4B] = 0;
    write16(&r[0x4C], 0);
    r[0x4E] = 1;
    write32(&r[0x40], uint32_t(pixels.size()));      // base level size
    // The format record keeps the replaced texture's flags (+8 and +16 vary per texture in the game's packs,
    // whatever the format); only the D3D format at +20 changes.
    Bytes& f = target->format;
    if (compression == kA8R8G8B8) write32(&f[20], 21);   // D3DFMT_A8R8G8B8
    else std::memcpy(&f[20], compression == kDXT1 ? "DXT1" : compression == kDXT3 ? "DXT3" : "DXT5", 4);
    target->pixels = std::move(pixels);
    target->palette.clear();
    return true;
}

// ---- the track map ------------------------------------------------------------------------------------------

// An image in the game's A8R8G8B8 byte order: B, G, R, A.
struct Image {
    uint32_t width = 0, height = 0;
    Bytes bgra;
    uint8_t* at(uint32_t x, uint32_t y) { return &bgra[(size_t(y) * width + x) * 4]; }
    const uint8_t* at(uint32_t x, uint32_t y) const { return &bgra[(size_t(y) * width + x) * 4]; }
};

// A stock race's map: its pixels and where they lie (the race record's calibration).
struct MapTile { Image image; MapPlacement place; };

// Road polygons of a free roam bundle: quads, x/y in metres (int16 at +8/+16 in 1/8 m).
struct Quad { float x[4], y[4]; };
inline std::vector<Quad> roadQuads(const Bytes& track) {
    std::vector<Quad> quads;
    std::vector<Chunk> top;
    if (!topChunks(track, top)) return quads;
    const Chunk* c = findChunk(top, kTrackPolygons);
    if (!c) return quads;
    const size_t count = c->size / kPolygonBytes;
    quads.reserve(count);
    for (size_t i = 0; i < count; ++i) {
        const uint8_t* p = &track[c->offset + 8 + i * kPolygonBytes];
        Quad q;
        for (int k = 0; k < 4; ++k) {
            int16_t x, y;
            std::memcpy(&x, p + 8 + 2 * k, 2);
            std::memcpy(&y, p + 16 + 2 * k, 2);
            q.x[k] = x / 8.0f; q.y[k] = y / 8.0f;
        }
        quads.push_back(q);
    }
    return quads;
}

// The square the map covers: the polygons' extent (2nd to 98th percentile, so a stray polygon does not shrink
// the rest), with a margin, as offX (left), offY (top) and width.
inline MapPlacement placeMap(const std::vector<Quad>& quads, float margin = 60.0f) {
    MapPlacement place;
    if (quads.empty()) return place;
    std::vector<float> xs, ys;
    xs.reserve(quads.size() * 4); ys.reserve(quads.size() * 4);
    for (const Quad& q : quads) for (int k = 0; k < 4; ++k) { xs.push_back(q.x[k]); ys.push_back(q.y[k]); }
    auto percentile = [](std::vector<float>& v, double p) {
        size_t i = std::min(v.size() - 1, size_t(p * double(v.size() - 1) + 0.5));
        std::nth_element(v.begin(), v.begin() + long(i), v.end());
        return v[i];
    };
    float x0 = percentile(xs, 0.02), x1 = percentile(xs, 0.98), y0 = percentile(ys, 0.02), y1 = percentile(ys, 0.98);
    // the full extent unless outliers stretch it: the percentiles plus a margin when that is smaller
    float fx0 = *std::min_element(xs.begin(), xs.end()), fx1 = *std::max_element(xs.begin(), xs.end());
    float fy0 = *std::min_element(ys.begin(), ys.end()), fy1 = *std::max_element(ys.begin(), ys.end());
    float span = std::max(x1 - x0, y1 - y0);
    if (fx0 > x0 - span * 0.1f) x0 = fx0;
    if (fx1 < x1 + span * 0.1f) x1 = fx1;
    if (fy0 > y0 - span * 0.1f) y0 = fy0;
    if (fy1 < y1 + span * 0.1f) y1 = fy1;
    float size = std::max(x1 - x0, y1 - y0) + 2 * margin;
    place.width = size;
    place.offX = (x0 + x1) / 2 - size / 2;
    place.offY = (y0 + y1) / 2 + size / 2;   // the top edge: v = (offY - y) / width
    return place;
}

// Bilinear sample of a tile at (u, v) in 0..1 of its own size; alpha premultiplied for blending.
inline void sampleTile(const Image& im, float u, float v, float out[4]) {
    float fx = u * im.width - 0.5f, fy = v * im.height - 0.5f;
    int x0 = int(std::floor(fx)), y0 = int(std::floor(fy));
    float ax = fx - x0, ay = fy - y0;
    for (int c = 0; c < 4; ++c) out[c] = 0;
    for (int dy = 0; dy < 2; ++dy) for (int dx = 0; dx < 2; ++dx) {
        int x = std::clamp(x0 + dx, 0, int(im.width) - 1), y = std::clamp(y0 + dy, 0, int(im.height) - 1);
        float w = (dx ? ax : 1 - ax) * (dy ? ay : 1 - ay);
        const uint8_t* p = im.at(uint32_t(x), uint32_t(y));
        float a = p[3] / 255.0f;
        out[0] += w * p[0] * a; out[1] += w * p[1] * a; out[2] += w * p[2] * a; out[3] += w * a;
    }
}

// The game's route colour on its maps (TRACK1001_MAP: B 255, G 190, R 150): roads are drawn in it, and a stock
// map's own route is taken out of its background (as the map's dark tone).
constexpr uint8_t kRouteB = 255, kRouteG = 190, kRouteR = 150;

// Fills a quad into a coverage mask of `size` x `size` (1 = covered): scanline, the pixel centres inside; a quad
// thinner than a pixel still marks the pixel its middle crosses.
inline void fillQuad(std::vector<uint8_t>& mask, uint32_t size, const float sx[4], const float sy[4]) {
    float minY = sy[0], maxY = sy[0];
    for (int k = 1; k < 4; ++k) { minY = std::min(minY, sy[k]); maxY = std::max(maxY, sy[k]); }
    const int y0 = std::max(0, int(std::floor(minY))), y1 = std::min(int(size) - 1, int(std::ceil(maxY)));
    for (int y = y0; y <= y1; ++y) {
        const float cy = y + 0.5f;
        float xs[4];
        int n = 0;
        for (int k = 0; k < 4; ++k) {
            const float ax = sx[k], ay = sy[k], bx = sx[(k + 1) % 4], by = sy[(k + 1) % 4];
            if ((ay <= cy && by > cy) || (by <= cy && ay > cy)) xs[n++] = ax + (cy - ay) / (by - ay) * (bx - ax);
        }
        for (int i = 1; i < n; ++i)   // at most 4 crossings: insertion sort
            for (int j = i; j > 0 && xs[j] < xs[j - 1]; --j) std::swap(xs[j], xs[j - 1]);
        for (int i = 0; i + 1 < n; i += 2) {
            int x0 = std::max(0, int(std::ceil(xs[i] - 0.5f))), x1 = std::min(int(size) - 1, int(std::floor(xs[i + 1] - 0.5f)));
            if (x1 < x0) {
                const int c = int((xs[i] + xs[i + 1]) / 2);
                if (c >= 0 && c < int(size)) mask[size_t(y) * size + size_t(c)] = 1;
                continue;
            }
            std::memset(&mask[size_t(y) * size + size_t(x0)], 1, size_t(x1 - x0 + 1));
        }
    }
}

// The map: the background from the stock maps, composed at a quarter of the size (their own detail is about 4-6
// m a pixel, so nothing is lost) and enlarged, coarse tiles first, finer ones over them, each fading out over its
// last 6 % so seams do not show; then the roads, anti-aliased (coverage from a mask at twice the size), with a
// soft darker border.
inline Image composeMap(const std::vector<MapTile>& tiles, const std::vector<Quad>& quads, const MapPlacement& place, uint32_t size) {
    Image map;
    map.width = map.height = size;
    map.bgra.assign(size_t(size) * size * 4, 0);
    const uint32_t bgSize = std::max(1u, size / 4);
    std::vector<float> bg(size_t(bgSize) * bgSize * 4, 0.0f);   // premultiplied B, G, R, A
    std::vector<const MapTile*> order;
    for (const MapTile& t : tiles) if (t.place.width > 0 && t.image.width && t.image.height) order.push_back(&t);
    std::stable_sort(order.begin(), order.end(), [](const MapTile* a, const MapTile* b) { return a->place.width > b->place.width; });
    for (const MapTile* t : order) {
        Image clean = t->image;
        for (uint32_t y = 0; y < clean.height; ++y) for (uint32_t x = 0; x < clean.width; ++x) {
            uint8_t* p = clean.at(x, y);
            if (p[3] > 150 && p[0] > 200) { p[0] = 45; p[1] = 38; p[2] = 12; p[3] = 50; }   // the race's route: background tone
        }
        const float toPixel = bgSize / place.width;
        const int px0 = std::max(0, int(std::floor((t->place.offX - place.offX) * toPixel)));
        const int px1 = std::min(int(bgSize), int(std::ceil((t->place.offX + t->place.width - place.offX) * toPixel)));
        const int py0 = std::max(0, int(std::floor((place.offY - t->place.offY) * toPixel)));
        const int py1 = std::min(int(bgSize), int(std::ceil((place.offY - t->place.offY + t->place.width) * toPixel)));
        for (int py = py0; py < py1; ++py) {
            const float wy = place.offY - (py + 0.5f) / bgSize * place.width;
            const float v = (t->place.offY - wy) / t->place.width;
            if (v < 0 || v >= 1) continue;
            for (int px = px0; px < px1; ++px) {
                const float wx = place.offX + (px + 0.5f) / bgSize * place.width;
                const float u = (wx - t->place.offX) / t->place.width;
                if (u < 0 || u >= 1) continue;
                float s[4];
                sampleTile(clean, u, v, s);
                const float edge = std::min(std::min(u, 1 - u), std::min(v, 1 - v));
                const float fade = std::clamp(edge / 0.06f, 0.0f, 1.0f);
                float* d = &bg[(size_t(py) * bgSize + size_t(px)) * 4];
                const float a = s[3] * fade;
                for (int c = 0; c < 3; ++c) d[c] = s[c] * fade + d[c] * (1 - a);
                d[3] = a + d[3] * (1 - a);
            }
        }
    }
    // Roads: coverage from a mask at twice the map's size.
    const uint32_t fine = size * 2;
    std::vector<uint8_t> mask(size_t(fine) * fine, 0);
    const float toFine = fine / place.width;
    for (const Quad& q : quads) {
        float sx[4], sy[4];
        for (int k = 0; k < 4; ++k) { sx[k] = (q.x[k] - place.offX) * toFine; sy[k] = (place.offY - q.y[k]) * toFine; }
        fillQuad(mask, fine, sx, sy);
    }
    std::vector<uint8_t> cover(size_t(size) * size, 0);   // 0..4
    for (uint32_t y = 0; y < size; ++y) for (uint32_t x = 0; x < size; ++x) {
        const uint8_t* r0 = &mask[size_t(2 * y) * fine + 2 * x];
        const uint8_t* r1 = r0 + fine;
        cover[size_t(y) * size + x] = uint8_t(r0[0] + r0[1] + r1[0] + r1[1]);
    }
    for (uint32_t y = 0; y < size; ++y) for (uint32_t x = 0; x < size; ++x) {
        // the background, enlarged (bilinear on the premultiplied values)
        float d[4];
        {
            const float fx = (x + 0.5f) / 4 - 0.5f, fy = (y + 0.5f) / 4 - 0.5f;
            const int x0 = int(std::floor(fx)), y0 = int(std::floor(fy));
            const float ax = fx - x0, ay = fy - y0;
            for (int c = 0; c < 4; ++c) d[c] = 0;
            for (int dy = 0; dy < 2; ++dy) for (int dx = 0; dx < 2; ++dx) {
                const int bx = std::clamp(x0 + dx, 0, int(bgSize) - 1), by = std::clamp(y0 + dy, 0, int(bgSize) - 1);
                const float w = (dx ? ax : 1 - ax) * (dy ? ay : 1 - ay);
                const float* b = &bg[(size_t(by) * bgSize + size_t(bx)) * 4];
                for (int c = 0; c < 4; ++c) d[c] += w * b[c];
            }
        }
        // border: the most covered neighbour beyond the pixel's own coverage
        const float own = cover[size_t(y) * size + x] / 4.0f;
        float near = own;
        for (int dy = -1; dy <= 1; ++dy) for (int dx = -1; dx <= 1; ++dx) {
            const int nx = int(x) + dx, ny = int(y) + dy;
            if (nx >= 0 && ny >= 0 && nx < int(size) && ny < int(size)) near = std::max(near, cover[size_t(ny) * size + size_t(nx)] / 4.0f);
        }
        const float border = (near - own) * 0.55f, road = own * 0.92f;
        if (border > 0) {
            const float bc[3] = {120, 90, 40};
            for (int c = 0; c < 3; ++c) d[c] = bc[c] * border + d[c] * (1 - border);
            d[3] = border + d[3] * (1 - border);
        }
        if (road > 0) {
            const float rc[3] = {kRouteB, kRouteG, kRouteR};
            for (int c = 0; c < 3; ++c) d[c] = rc[c] * road + d[c] * (1 - road);
            d[3] = road + d[3] * (1 - road);
        }
        uint8_t* o = map.at(x, y);
        const float alpha = std::clamp(d[3], 0.0f, 1.0f);
        for (int c = 0; c < 3; ++c) o[c] = uint8_t(std::clamp(alpha > 0 ? d[c] / alpha : 0.0f, 0.0f, 255.0f) + 0.5f);
        o[3] = uint8_t(alpha * 255.0f + 0.5f);
    }
    return map;
}

// ---- DXT5 ---------------------------------------------------------------------------------------------------

inline uint16_t to565(const float c[3]) {   // c: B, G, R 0..255
    unsigned b = unsigned(std::clamp(c[0], 0.0f, 255.0f) * 31 / 255 + 0.5f), g = unsigned(std::clamp(c[1], 0.0f, 255.0f) * 63 / 255 + 0.5f),
             r = unsigned(std::clamp(c[2], 0.0f, 255.0f) * 31 / 255 + 0.5f);
    return uint16_t((r << 11) | (g << 5) | b);
}
inline void from565(uint16_t v, float c[3]) {
    c[2] = float((v >> 11) & 31) * 255 / 31; c[1] = float((v >> 5) & 63) * 255 / 63; c[0] = float(v & 31) * 255 / 31;
}

// One 4x4 block: the colour end points along the block's principal axis (power iteration on its covariance),
// the alpha end points its extremes; each texel takes the nearest of the interpolated values.
inline void encodeBlock(const uint8_t texel[16][4], uint8_t out[16]) {
    // alpha: 8-value mode, a0 > a1
    uint8_t amin = 255, amax = 0;
    for (int i = 0; i < 16; ++i) { amin = std::min(amin, texel[i][3]); amax = std::max(amax, texel[i][3]); }
    out[0] = amax; out[1] = amin;
    uint64_t abits = 0;
    float avals[8];
    avals[0] = amax; avals[1] = amin;
    for (int k = 1; k <= 6; ++k) avals[k + 1] = ((7 - k) * float(amax) + k * float(amin)) / 7.0f;
    for (int i = 0; i < 16; ++i) {
        int best = 0; float bestD = 1e30f;
        for (int k = 0; k < 8; ++k) { float d = std::fabs(avals[k] - texel[i][3]); if (d < bestD) { bestD = d; best = k; } }
        if (amax == amin) best = 0;
        abits |= uint64_t(best) << (3 * i);
    }
    for (int k = 0; k < 6; ++k) out[2 + k] = uint8_t(abits >> (8 * k));
    // colour
    float mean[3] = {0, 0, 0}, wsum = 0;
    for (int i = 0; i < 16; ++i) { float w = 1.0f + texel[i][3]; for (int c = 0; c < 3; ++c) mean[c] += w * texel[i][c]; wsum += w; }
    for (int c = 0; c < 3; ++c) mean[c] /= wsum;
    float cov[6] = {0, 0, 0, 0, 0, 0};
    for (int i = 0; i < 16; ++i) {
        float w = 1.0f + texel[i][3];
        float d[3] = {texel[i][0] - mean[0], texel[i][1] - mean[1], texel[i][2] - mean[2]};
        cov[0] += w * d[0] * d[0]; cov[1] += w * d[0] * d[1]; cov[2] += w * d[0] * d[2];
        cov[3] += w * d[1] * d[1]; cov[4] += w * d[1] * d[2]; cov[5] += w * d[2] * d[2];
    }
    float axis[3] = {1, 1, 1};
    for (int it = 0; it < 8; ++it) {
        float n[3] = {cov[0] * axis[0] + cov[1] * axis[1] + cov[2] * axis[2], cov[1] * axis[0] + cov[3] * axis[1] + cov[4] * axis[2],
                      cov[2] * axis[0] + cov[4] * axis[1] + cov[5] * axis[2]};
        float len = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
        if (len < 1e-6f) break;
        for (int c = 0; c < 3; ++c) axis[c] = n[c] / len;
    }
    float lo = 1e30f, hi = -1e30f;
    for (int i = 0; i < 16; ++i) {
        float t = (texel[i][0] - mean[0]) * axis[0] + (texel[i][1] - mean[1]) * axis[1] + (texel[i][2] - mean[2]) * axis[2];
        lo = std::min(lo, t); hi = std::max(hi, t);
    }
    float c0[3], c1[3];
    for (int c = 0; c < 3; ++c) { c0[c] = mean[c] + axis[c] * hi; c1[c] = mean[c] + axis[c] * lo; }
    uint16_t e0 = to565(c0), e1 = to565(c1);
    if (e0 < e1) std::swap(e0, e1);
    uint32_t cbits = 0;
    if (e0 != e1) {
        float p[4][3];
        from565(e0, p[0]); from565(e1, p[1]);
        for (int c = 0; c < 3; ++c) { p[2][c] = (2 * p[0][c] + p[1][c]) / 3; p[3][c] = (p[0][c] + 2 * p[1][c]) / 3; }
        for (int i = 0; i < 16; ++i) {
            int best = 0; float bestD = 1e30f;
            for (int k = 0; k < 4; ++k) {
                float d = 0;
                for (int c = 0; c < 3; ++c) { float e = p[k][c] - texel[i][c]; d += e * e; }
                if (d < bestD) { bestD = d; best = k; }
            }
            cbits |= uint32_t(best) << (2 * i);
        }
    }
    write16(out + 8, e0); write16(out + 10, e1); write32(out + 12, cbits);
}

inline Bytes encodeDXT5(const Image& im) {
    Bytes out(mipBytes(im.width, im.height, kDXT5));
    size_t at = 0;
    for (uint32_t by = 0; by < im.height; by += 4) for (uint32_t bx = 0; bx < im.width; bx += 4) {
        uint8_t texel[16][4];
        for (int i = 0; i < 16; ++i) {
            uint32_t x = std::min(bx + uint32_t(i % 4), im.width - 1), y = std::min(by + uint32_t(i / 4), im.height - 1);
            std::memcpy(texel[i], im.at(x, y), 4);
        }
        encodeBlock(texel, &out[at]);
        at += 16;
    }
    return out;
}

// One DXT1/3/5 block into 16 texels (B, G, R, A), as Direct3D decodes it.
inline void decodeBlock(const uint8_t* block, uint8_t compression, uint8_t out[16][4]) {
    const uint8_t* colour = compression == kDXT1 ? block : block + 8;
    const uint16_t c0 = read16(colour), c1 = read16(colour + 2);
    float p[4][3];
    from565(c0, p[0]); from565(c1, p[1]);
    const bool fourColours = compression != kDXT1 || c0 > c1;
    for (int c = 0; c < 3; ++c) {
        if (fourColours) { p[2][c] = (2 * p[0][c] + p[1][c]) / 3; p[3][c] = (p[0][c] + 2 * p[1][c]) / 3; }
        else { p[2][c] = (p[0][c] + p[1][c]) / 2; p[3][c] = 0; }
    }
    const uint32_t indices = read32(colour + 4);
    uint8_t alpha[8] = {255, 255, 255, 255, 255, 255, 255, 255};
    uint64_t alphaBits = 0;
    if (compression == kDXT5) {
        alpha[0] = block[0]; alpha[1] = block[1];
        for (int k = 2; k < 8; ++k)
            alpha[k] = block[0] > block[1] ? uint8_t(((8 - k) * block[0] + (k - 1) * block[1]) / 7)
                     : k < 6 ? uint8_t(((6 - k) * block[0] + (k - 1) * block[1]) / 5) : k == 6 ? 0 : 255;
        for (int k = 0; k < 6; ++k) alphaBits |= uint64_t(block[2 + k]) << (8 * k);
    }
    for (int i = 0; i < 16; ++i) {
        const unsigned index = (indices >> (2 * i)) & 3;
        for (int c = 0; c < 3; ++c) out[i][c] = uint8_t(p[index][c] + 0.5f);
        if (compression == kDXT1) out[i][3] = !fourColours && index == 3 ? 0 : 255;
        else if (compression == kDXT3) out[i][3] = uint8_t(((block[i / 2] >> (4 * (i & 1))) & 15) * 17);
        else out[i][3] = alpha[(alphaBits >> (3 * i)) & 7];
    }
}

// The stock map of a texture pack (TRACK<n>_MAP) as an image: A8R8G8B8, or DXT1/3/5 (an HD pack's).
inline bool mapImage(const TexturePack& pack, const std::string& name, Image& out) {
    for (const auto& t : pack.textures) {
        if (t.name() != name) continue;
        const uint32_t w = read16(&t.record[0x44]), h = read16(&t.record[0x46]);
        const uint8_t compression = t.record[0x4A];
        if (!w || !h || w > 8192 || h > 8192) return false;
        if (compression == kA8R8G8B8) {
            if (t.pixels.size() < size_t(w) * h * 4) return false;
            out.width = w; out.height = h;
            out.bgra.assign(t.pixels.begin(), t.pixels.begin() + long(size_t(w) * h * 4));
            return true;
        }
        if (compression != kDXT1 && compression != kDXT3 && compression != kDXT5) return false;
        if (t.pixels.size() < mipBytes(w, h, compression)) return false;
        out.width = w; out.height = h;
        out.bgra.assign(size_t(w) * h * 4, 0);
        const size_t blockBytes = compression == kDXT1 ? 8 : 16;
        const uint32_t bw = std::max(1u, (w + 3) / 4), bh = std::max(1u, (h + 3) / 4);
        for (uint32_t by = 0; by < bh; ++by) for (uint32_t bx = 0; bx < bw; ++bx) {
            uint8_t texel[16][4];
            decodeBlock(&t.pixels[(size_t(by) * bw + bx) * blockBytes], compression, texel);
            for (int i = 0; i < 16; ++i) {
                const uint32_t x = bx * 4 + uint32_t(i % 4), y = by * 4 + uint32_t(i / 4);
                if (x < w && y < h) std::memcpy(out.at(x, y), texel[i], 4);
            }
        }
        return true;
    }
    return false;
}

// ---- JDLZ, as the game's streaming packs store each texture ----------------------------------------------------

// EA's JDLZ as Speed.exe's decoder (0x58D190) reads it: the header ("JDLZ", 2, 0x10, the raw size at +8, the packed
// size at +12) and the first two flag bytes, then tokens: a literal byte, or a 2-byte reference (distance 1-16 with
// a length up to 4098, or distance 17-2064 with a length up to 34). The decoder counts down the packed size, and it
// takes the next flags1 byte the moment 8 tokens are used and the next flags2 byte the moment 8 references are, so
// each is written right there; the stream ends as EA's do, with a dummy literal the decoder counts but never reads.
// (tools/nfstex/tpk.py jdlz_compress, checked against the game's decoder; tests/free_roam.cpp decodes as it does.)
inline Bytes jdlzCompress(const Bytes& raw) {
    const size_t n = raw.size();
    Bytes out(18, 0);
    size_t f1 = 16, f2 = 17;
    unsigned f1Bits = 0, f2Bits = 0;
    std::vector<int32_t> head(65536, -1), chain(n, -1);
    auto key = [&](size_t at) {
        return ((uint32_t(raw[at]) << 16 | uint32_t(raw[at + 1]) << 8 | raw[at + 2]) * 0x9E3779B1u >> 8) & 0xFFFFu;
    };
    auto insert = [&](size_t at) {
        if (at + 3 > n) return;
        const uint32_t k = key(at);
        chain[at] = head[k];
        head[k] = int32_t(at);
    };
    auto common = [&](size_t earlier, size_t at, size_t limit) {
        size_t length = 0;
        while (length < limit && raw[earlier + length] == raw[at + length]) ++length;
        return length;
    };
    size_t at = 0;
    while (at < n) {
        size_t length = 0, distance = 0;
        for (size_t d = 1; d <= 16 && d <= at; ++d) {   // runs and repeated blocks: a near reference
            const size_t l = common(at - d, at, std::min<size_t>(4098, n - at));
            if (l > length) { length = l; distance = d; }
        }
        if (at + 3 <= n && length < 34) {               // repeats further back: a far reference
            int32_t candidate = head[key(at)];
            for (int tries = 0; candidate >= 0 && tries < 32; ++tries, candidate = chain[size_t(candidate)]) {
                const size_t d = at - size_t(candidate);
                if (d > 2064) break;
                if (d <= 16) continue;
                const size_t l = common(size_t(candidate), at, std::min<size_t>(34, n - at));
                if (l > length) { length = l; distance = d; }
            }
        }
        insert(at);
        if (length >= 3) {
            out[f1] |= uint8_t(1u << f1Bits);
            if (distance <= 16) {
                out[f2] |= uint8_t(1u << f2Bits);
                out.push_back(uint8_t((((length - 3) >> 8) << 4) | (distance - 1)));
                out.push_back(uint8_t((length - 3) & 0xFF));
            } else {
                out.push_back(uint8_t((((distance - 17) >> 8) << 5) | (length - 3)));
                out.push_back(uint8_t((distance - 17) & 0xFF));
            }
            ++f2Bits;
            for (size_t skipped = at + 1; skipped < at + length; ++skipped) insert(skipped);
            at += length;
        } else {
            out.push_back(raw[at++]);
        }
        if (++f1Bits == 8) { f1 = out.size(); out.push_back(0); f1Bits = 0; }
        if (f2Bits == 8) { f2 = out.size(); out.push_back(0); f2Bits = 0; }
    }
    out.push_back(0);                       // the dummy literal: counted, never read
    if (++f1Bits == 8) out.push_back(0);    // it can complete a flags1 group, which the decoder refills
    std::memcpy(out.data(), "JDLZ", 4);
    out[4] = 2; out[5] = 0x10; out[6] = out[7] = 0;
    write32(&out[8], uint32_t(n));
    write32(&out[12], uint32_t(out.size()));
    return out;
}

// ---- streaming texture packs (Global/DYNTEX.BIN) --------------------------------------------------------------

// A pack the game loads textures from one at a time, by hash (Global/DYNTEX.BIN; HUDTEX and TSELECT are the same
// kind): a TEX file's info chunks plus a table (0x33310003) of {hash, file offset, size, 0x100, 0} sorted by hash,
// each texture's data (palette first) one JDLZ blob in the body. The records keep the layout an uncompressed pack
// would have (+0x30 the pixel data's offset, +0x34 the aligned end its palette would follow, +0x38/+0x3C sizes).
struct StreamedTexture {
    Bytes record, format, blob;
    uint32_t flags = 0x100;
    std::string name() const { return std::string(reinterpret_cast<const char*>(&record[12]), strnlen(reinterpret_cast<const char*>(&record[12]), 24)); }
    uint32_t hash() const { return read32(&record[0x24]); }
};
struct StreamedPack { Bytes header, dataHeader; std::vector<StreamedTexture> textures; };

inline std::string parseStreamedPack(const Bytes& file, StreamedPack& pack) {
    std::vector<Chunk> top;
    if (!topChunks(file, top) || top.empty() || top[0].id != kTpk) return "not a texture pack";
    const size_t begin = top[0].offset + 8, end = begin + top[0].size;
    size_t info = 0, infoEnd = 0, data = 0, dataEnd = 0;
    for (size_t at = begin; at + 8 <= end;) {
        const uint32_t id = read32(&file[at]), size = read32(&file[at + 4]);
        if (size > end - at - 8) return "texture pack chunk runs past its pack";
        if (id == kTpkInfo) { info = at + 8; infoEnd = at + 8 + size; }
        if (id == kTpkData) { data = at + 8; dataEnd = at + 8 + size; }
        at += 8 + size_t(size);
    }
    if (!info || !data) return "texture pack without info or data";
    const uint8_t *header = nullptr, *hashes = nullptr, *table = nullptr, *records = nullptr, *formats = nullptr;
    size_t hashBytes = 0, tableBytes = 0, recordBytes = 0, formatBytes = 0;
    for (size_t at = info; at + 8 <= infoEnd;) {
        const uint32_t id = read32(&file[at]), size = read32(&file[at + 4]);
        if (size > infoEnd - at - 8) return "texture pack info runs past its chunk";
        const uint8_t* p = &file[at + 8];
        if (id == kTpkHeader && size == kTexRecordBytes) header = p;
        else if (id == kTpkHashes) { hashes = p; hashBytes = size; }
        else if (id == kTpkCompressed) { table = p; tableBytes = size; }
        else if (id == kTpkRecords) { records = p; recordBytes = size; }
        else if (id == kTpkFormats) { formats = p; formatBytes = size; }
        at += 8 + size_t(size);
    }
    if (!header || !hashes || !records || !formats) return "texture pack tables missing";
    if (!table) return "not a streaming texture pack";
    const size_t count = hashBytes / 8;
    if (recordBytes != count * kTexRecordBytes || formatBytes != count * kTexFormatBytes || tableBytes != count * 20)
        return "texture pack tables disagree";
    const uint8_t* dataHeader = nullptr;
    for (size_t at = data; at + 8 <= dataEnd;) {
        const uint32_t id = read32(&file[at]), size = read32(&file[at + 4]);
        if (size > dataEnd - at - 8) return "texture data runs past its chunk";
        if (id == kTpkDataHeader && size == 24) dataHeader = &file[at + 8];
        at += 8 + size_t(size);
    }
    if (!dataHeader) return "texture data without its header";
    pack.header.assign(header, header + kTexRecordBytes);
    pack.dataHeader.assign(dataHeader, dataHeader + 24);
    pack.textures.clear();
    for (size_t i = 0; i < count; ++i) {
        StreamedTexture t;
        t.record.assign(records + i * kTexRecordBytes, records + (i + 1) * kTexRecordBytes);
        t.format.assign(formats + i * kTexFormatBytes, formats + (i + 1) * kTexFormatBytes);
        const uint8_t* entry = nullptr;
        for (size_t k = 0; k < count && !entry; ++k) if (read32(table + k * 20) == t.hash()) entry = table + k * 20;
        if (!entry) return "texture " + t.name() + " has no data";
        const uint32_t offset = read32(entry + 4), size = read32(entry + 8);
        if (offset < size_t(dataHeader - file.data()) || size < 16 || size_t(offset) + size > file.size())
            return "texture " + t.name() + " lies outside the pack's data";
        t.blob.assign(file.begin() + long(offset), file.begin() + long(offset + size));
        t.flags = read32(entry + 12);
        pack.textures.push_back(std::move(t));
    }
    return "";
}

// The pack as a file, laid out as the game's own (tools/nfstex tpk.py serialize, streaming): the textures in hash
// order, the info parent's header 16 bytes past a 128-byte boundary, the data parent on one, its body starting 8
// bytes past one with 120 bytes of 0x11 filler and then the blobs back to back, and the 4096-byte padding chunk the
// game's packs end with. A stock DYNTEX.BIN comes out byte for byte as it went in.
inline Bytes serializeStreamedPack(StreamedPack& pack) {
    std::stable_sort(pack.textures.begin(), pack.textures.end(), [](const StreamedTexture& a, const StreamedTexture& b) { return a.hash() < b.hash(); });
    const size_t count = pack.textures.size();
    size_t cursor = 0;
    for (size_t i = 0; i < count; ++i) {
        Bytes& r = pack.textures[i].record;
        const uint32_t pixels = read32(&r[0x38]), palette = read32(&r[0x3C]);
        write32(&r[0x30], uint32_t(cursor));
        cursor += pixels;
        size_t aligned = (cursor + kAlign - 1) / kAlign * kAlign;
        write32(&r[0x34], uint32_t(aligned));
        aligned += palette;
        if (i + 1 < count) cursor = aligned;
    }
    Bytes hashes, table, records, formats;
    const size_t leadingPad = (16 + kAlign * 2 - (8 + 8)) % kAlign;
    const size_t infoSize = 8 + (8 + pack.header.size()) + (8 + count * 8) + (8 + count * 20) + (8 + count * kTexRecordBytes) +
                            (8 + count * kTexFormatBytes);
    const size_t afterInfo = 8 + 8 + leadingPad + infoSize;
    const size_t outerPad = (kAlign * 2 - (afterInfo + 8) % kAlign) % kAlign;
    const size_t dataParentAt = afterInfo + 8 + outerPad;
    const size_t afterDataHeader = dataParentAt + 8 + 8 + pack.dataHeader.size();
    const size_t innerPad = (8 + kAlign * 2 - (afterDataHeader + 8 + 8) % kAlign) % kAlign;
    size_t blobAt = afterDataHeader + 8 + innerPad + 8 + (kAlign - 8);
    Bytes body(kAlign - 8, 0x11);
    for (const auto& t : pack.textures) {
        uint8_t pair[8], entry[20];
        write32(pair, t.hash()); write32(pair + 4, 0);
        hashes.insert(hashes.end(), pair, pair + 8);
        write32(entry, t.hash()); write32(entry + 4, uint32_t(blobAt)); write32(entry + 8, uint32_t(t.blob.size()));
        write32(entry + 12, t.flags); write32(entry + 16, 0);
        table.insert(table.end(), entry, entry + 20);
        records.insert(records.end(), t.record.begin(), t.record.end());
        formats.insert(formats.end(), t.format.begin(), t.format.end());
        body.insert(body.end(), t.blob.begin(), t.blob.end());
        blobAt += t.blob.size();
    }
    body.resize((body.size() - (kAlign - 8) + 3) / 4 * 4 + (kAlign - 8), 0);
    Bytes info, data;
    appendChunk(info, kTpkHeader, pack.header);
    appendChunk(info, kTpkHashes, hashes);
    appendChunk(info, kTpkCompressed, table);
    appendChunk(info, kTpkRecords, records);
    appendChunk(info, kTpkFormats, formats);
    appendChunk(data, kTpkDataHeader, pack.dataHeader);
    appendPadding(data, innerPad);
    appendChunk(data, kTpkDataBody, body);
    Bytes tpk;
    appendPadding(tpk, leadingPad);
    appendChunk(tpk, kTpkInfo, info);
    appendPadding(tpk, outerPad);
    appendChunk(tpk, kTpkData, data);
    Bytes file;
    appendChunk(file, kTpk, tpk);
    appendPadding(file, 4096);
    return file;
}

// ---- the select screen's picture -------------------------------------------------------------------------------
//
// SELECT LOCATION shows a track's map behind the list (MU_TrackMapScroller): 0x4BBC90 takes the background texture
// from 0x4B92A0 and hashes TRACK_<n>_SPLINE, the route drawn over it; 0x4BBA40 puts both on the scroller's images
// and has them loaded from the dynamic pack, Global/DYNTEX.BIN (0x53E9E0), the picture appearing when both are in.
// The pack has no TRACK_1000 and no free roam track's TRACK_<n>_SPLINE, so their picture stayed black. The host
// serves it with them added: Olympic City's background a copy of the whole city's (SPRINT_TRACKS_BACK, the sprints'
// map), the drift districts' the game's own; over it, as the route, the track's drivable roads in the background's
// frame, drawn as the game draws a drift course (white, a soft fill and bright edges; the screen tints it gold).

// The background 0x4B92A0 asks for: TRACK_<n> below 1100 and for the drags (1200-1299), the city for the sprints
// (SPRINT_TRACKS_BACK for 1103-1105, SPRINT_TRACKS_BACK2 for the others: its jump table at 0x4B9344),
// DRIFT_TRACKS_BACK for 1300-1304, DRIFT_TRACKS_BACK2 above.
inline std::string selectBackground(unsigned number) {
    if (number < 1100 || (number >= 1200 && number < 1300)) return "TRACK_" + std::to_string(number);
    if (number < 1200) return number >= 1103 && number <= 1105 ? "SPRINT_TRACKS_BACK" : "SPRINT_TRACKS_BACK2";
    return number < 1305 ? "DRIFT_TRACKS_BACK" : "DRIFT_TRACKS_BACK2";
}
inline std::string selectRoute(unsigned number) { return "TRACK_" + std::to_string(number) + "_SPLINE"; }

// Where a background lies in the world, over its texture (u, v in 0..1): north up, u = (x - x0) / sx and
// v = (y0 - y) / sy; turned (the drift backgrounds, north to the right): u = (y - x0) / sx, v = (y0 + x) / sy.
// Fitted to the stock races drawn over each (their TRACK_<n>_SPLINE against their own road polygons, all races of
// a background at once, by the distance between the two in metres): the sprints 1103-1105 over SPRINT_TRACKS_BACK
// (the route within 3 m of a road on the median), the drift races 1301-1304 and 1305-1308 over the two drift
// backgrounds (within 1 m; turned, as their art is). The scales differ by axis as the game's art does (the menu
// shows the square textures stretched).
struct SelectFrame { const char* background; bool turned; float x0, y0, sx, sy; };
inline const SelectFrame kSelectFrames[] = {
    {"SPRINT_TRACKS_BACK", false, -1084.45f, 1535.56f, 4035.52f, 4204.59f},
    {"DRIFT_TRACKS_BACK", true, -1576.09f, 341.37f, 590.45f, 476.15f},
    {"DRIFT_TRACKS_BACK2", true, -1578.61f, 303.34f, 567.90f, 447.49f},
};
inline void selectPoint(const SelectFrame& f, float x, float y, float& u, float& v) {
    if (!f.turned) { u = (x - f.x0) / f.sx; v = (f.y0 - y) / f.sy; }
    else { u = (y - f.x0) / f.sx; v = (f.y0 + x) / f.sy; }
}

// The art a track's picture shows: the game's choice, or (for a track the game would ask its own TRACK_<n> for)
// the stock background the host copies under that name.
inline std::string pictureSource(const Track& track) {
    const std::string background = selectBackground(track.number);
    return background == "TRACK_" + std::to_string(track.number) && track.pictureArt ? track.pictureArt : background;
}
inline const SelectFrame* pictureFrame(const Track& track) {
    const std::string art = pictureSource(track);
    for (const SelectFrame& f : kSelectFrames) if (art == f.background) return &f;
    return nullptr;
}

constexpr float kPictureFill = 0.32f, kPictureEdge = 1.6f;   // TRACK_1305_SPLINE: alpha 80 inside, 255 on the edges

// A route's size: the background's own (the game's 512, an HD pack's 1024), a power of two from 256 to 2048.
inline uint32_t pictureSize(uint32_t artWidth) {
    uint32_t size = 256;
    while (size < 2048 && size < artWidth) size *= 2;
    return size;
}

// The route a free roam track's picture shows: its roads in the frame, white; alpha a soft fill where a road
// covers the pixel and full on the roads' edges (coverage beyond its 3x3 minimum), anti-aliased from a mask at four
// times the size. Degenerate polygons (no area) are left out.
inline Image pictureRoads(const std::vector<Quad>& quads, const SelectFrame& frame, uint32_t size) {
    const uint32_t fine = size * 4;
    std::vector<uint8_t> mask(size_t(fine) * fine, 0);
    for (const Quad& q : quads) {
        float area = 0;
        for (int k = 0; k < 4; ++k) area += q.x[k] * q.y[(k + 1) % 4] - q.x[(k + 1) % 4] * q.y[k];
        if (!(std::fabs(area) > 0.01f)) continue;
        float sx[4], sy[4];
        for (int k = 0; k < 4; ++k) {
            selectPoint(frame, q.x[k], q.y[k], sx[k], sy[k]);
            sx[k] *= float(fine); sy[k] *= float(fine);
        }
        fillQuad(mask, fine, sx, sy);
    }
    std::vector<float> cover(size_t(size) * size, 0.0f);
    for (uint32_t y = 0; y < size; ++y) for (uint32_t x = 0; x < size; ++x) {
        unsigned sum = 0;
        for (uint32_t dy = 0; dy < 4; ++dy) {
            const uint8_t* row = &mask[size_t(4 * y + dy) * fine + 4 * x];
            sum += unsigned(row[0]) + row[1] + row[2] + row[3];
        }
        cover[size_t(y) * size + x] = float(sum) / 16.0f;
    }
    Image picture;
    picture.width = picture.height = size;
    picture.bgra.assign(size_t(size) * size * 4, 0);
    for (uint32_t y = 0; y < size; ++y) for (uint32_t x = 0; x < size; ++x) {
        const float own = cover[size_t(y) * size + x];
        if (own <= 0) continue;
        float least = own;
        for (int dy = -1; dy <= 1; ++dy) for (int dx = -1; dx <= 1; ++dx) {
            const int nx = int(x) + dx, ny = int(y) + dy;
            least = std::min(least, nx >= 0 && ny >= 0 && nx < int(size) && ny < int(size) ? cover[size_t(ny) * size + size_t(nx)] : 0.0f);
        }
        const float alpha = std::clamp(own * kPictureFill + (own - least) * kPictureEdge, 0.0f, 1.0f);
        uint8_t* p = picture.at(x, y);
        p[0] = p[1] = p[2] = 255;
        p[3] = uint8_t(alpha * 255.0f + 0.5f);
    }
    return picture;
}

// A texture for a streaming pack: `like`'s record and format (the engine's flags) under `name`, one level of
// `width` x `height` in `compression`, its data packed as the pack stores it.
inline StreamedTexture streamedTexture(const StreamedTexture& like, const std::string& name, uint32_t width, uint32_t height,
                                       uint8_t compression, const Bytes& pixels) {
    StreamedTexture t = like;
    Bytes& r = t.record;
    std::memset(&r[12], 0, 24);
    std::memcpy(&r[12], name.data(), std::min<size_t>(name.size(), 23));
    write32(&r[0x24], binHash(name.c_str()));
    write32(&r[0x38], uint32_t(pixels.size()));
    write32(&r[0x3C], 0);
    write32(&r[0x40], uint32_t(pixels.size()));
    write16(&r[0x44], uint16_t(width));
    write16(&r[0x46], uint16_t(height));
    unsigned lw = 0, lh = 0;
    while ((1u << lw) < width) ++lw;
    while ((1u << lh) < height) ++lh;
    r[0x48] = uint8_t(lw); r[0x49] = uint8_t(lh);
    r[0x4A] = compression; r[0x4B] = 0;
    write16(&r[0x4C], 0);
    r[0x4E] = 1;
    if (compression == kA8R8G8B8) write32(&t.format[20], 21);
    else std::memcpy(&t.format[20], compression == kDXT1 ? "DXT1" : compression == kDXT3 ? "DXT3" : "DXT5", 4);
    t.blob = jdlzCompress(pixels);
    return t;
}

// A free roam track to picture: its number and its road polygons (from its bundle).
struct Picture { uint16_t number = 0; std::vector<Quad> quads; };
struct PictureReport { unsigned tracks = 0, textures = 0; std::string error; std::vector<std::string> skipped; };

// The dynamic pack with the free roam tracks' pictures: for each, the background the game asks for (added as a
// copy of the stock art when it is the track's own TRACK_<n>) and its route, drawn at the background's resolution
// (DXT5, one level). A texture the pack already has stays as it is (a mod's own picture wins). A track whose art
// is missing gets nothing (its picture stays black, as before). Returns the file, or nothing with report.error set
// when the pack cannot be read.
inline Bytes makePicturePack(const Bytes& dyntex, const std::vector<Picture>& pictures, PictureReport& report) {
    StreamedPack pack;
    report = PictureReport{};
    report.error = parseStreamedPack(dyntex, pack);
    if (!report.error.empty()) return {};
    auto find = [&](const std::string& name) -> const StreamedTexture* {
        for (const auto& t : pack.textures) if (t.name() == name) return &t;
        return nullptr;
    };
    const StreamedTexture* routeLike = nullptr;
    for (const auto& t : pack.textures) {
        const std::string name = t.name();
        if (name.size() > 7 && name.compare(name.size() - 7, 7, "_SPLINE") == 0) { routeLike = &t; break; }
    }
    std::vector<StreamedTexture> added;
    for (const Picture& picture : pictures) {
        const Track* track = nullptr;
        for (const Track& t : kTracks) if (t.number == picture.number) track = &t;
        if (!track) continue;
        const std::string background = selectBackground(track->number), art = pictureSource(*track), route = selectRoute(track->number);
        const StreamedTexture *shown = find(background), *source = find(art);
        const SelectFrame* frame = pictureFrame(*track);
        if (!shown && !source) { report.skipped.push_back(std::to_string(track->number) + ": no " + art); continue; }
        if (!frame || picture.quads.empty()) { report.skipped.push_back(std::to_string(track->number) + ": no roads to draw"); continue; }
        if (!shown) {
            StreamedTexture copy = *source;
            std::memset(&copy.record[12], 0, 24);
            std::memcpy(&copy.record[12], background.data(), std::min<size_t>(background.size(), 23));
            write32(&copy.record[0x24], binHash(background.c_str()));
            added.push_back(std::move(copy));
        }
        const StreamedTexture& pictured = shown ? *shown : *source;
        if (!find(route)) {
            const uint32_t size = pictureSize(read16(&pictured.record[0x44]));
            added.push_back(streamedTexture(routeLike ? *routeLike : pictured, route, size, size, kDXT5,
                                            encodeDXT5(pictureRoads(picture.quads, *frame, size))));
        }
        ++report.tracks;
    }
    report.textures = unsigned(added.size());
    for (auto& t : added) pack.textures.push_back(std::move(t));
    return serializeStreamedPack(pack);
}

// ---- a free roam track, made ----------------------------------------------------------------------------------

// The map's size: about 1.6 m a pixel (a city road 6-10 pixels wide), a power of two from 256 to 2048.
inline uint32_t mapSize(float width) {
    uint32_t size = 256;
    while (size < 2048 && width / float(size) > 1.6f) size *= 2;
    return size;
}

struct MadeTrack {
    Bytes track;                 // TRACKB<number>: the bundle with the installed base race's section table
    Bytes tex;                   // TEX<number>TRACK.BIN with TRACK<number>_MAP; empty: serve the base race's as it is
    MapPlacement place;          // where the map lies; width 0: no map (the record keeps the base race's)
    uint32_t mapSize = 0;
    size_t quads = 0, tiles = 0;
    std::string error;           // the track cannot be offered
    std::string mapError;        // offered without its map
};

// A free roam track from Beta 2.0's bundle, the installed base race's bundle (decompressed) and TEX file, and the
// district's stock maps. The map never stops the track: without it the minimap stays off, as in Beta 2.0.
inline MadeTrack makeTrack(const Track& track, Bytes freeRoamBundle, const Bytes& baseBundle, const Bytes& baseTex,
                           const std::vector<MapTile>& tiles) {
    MadeTrack made;
    made.error = transplantSections(freeRoamBundle, baseBundle);
    if (!made.error.empty()) return made;
    made.track = std::move(freeRoamBundle);
    const std::vector<Quad> quads = roadQuads(made.track);
    made.quads = quads.size();
    if (quads.empty()) { made.mapError = "no road polygons"; return made; }
    TexturePack pack;
    made.mapError = parsePack(baseTex, pack);
    if (!made.mapError.empty()) return made;
    const MapPlacement place = placeMap(quads);
    if (!(place.width > 0)) { made.mapError = "no map extent"; return made; }
    made.mapSize = mapSize(place.width);
    made.tiles = tiles.size();
    const Image image = composeMap(tiles, quads, place, made.mapSize);
    if (!setTexture(pack, mapName(track.number), mapName(track.base), made.mapSize, made.mapSize, kDXT5, encodeDXT5(image))) {
        made.mapError = "the map texture could not be set";
        made.mapSize = 0;
        return made;
    }
    made.tex = serializePack(pack);
    made.place = place;
    return made;
}

}  // namespace free_roam
