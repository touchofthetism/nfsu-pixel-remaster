// Free roam (build 19). The game side's decisions (src/runtime/nfs_freeroam.h): the records it adds to the game's
// TrackInfo table, the names it merges into the language table, the breadcrumbs and which one a reset takes; the
// host side (android/app/src/main/cpp/free_roam.h): section table transplant, texture packs, the map and the made
// track. Everything is synthetic: bundles, records, packs and a guest memory built here. Also: the lifter's lists
// name every hook and fast path freeroam.c answers.
#include "../src/runtime/nfs_freeroam.h"
#include "../android/app/src/main/cpp/free_roam.h"
#include "../android/app/src/main/cpp/rim_sizes.h"   // the lenient JDLZ decoder the host uses
#include "../src/runtime/nfs_fastpath.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <tuple>
#include <vector>

static int checks = 0;
static void require(bool ok, const char* message) {
    ++checks;
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", message); std::exit(1); }
}

// ---- a guest memory: a flat block at a guest address, a bump allocator ----------------------------------------

struct Guest {
    static constexpr uint32_t kBase = 0x10000000u;
    std::vector<uint8_t> bytes = std::vector<uint8_t>(4u << 20, 0xCD);
    uint32_t next = kBase + (1u << 20);    // the first MB is the "game's" data, allocations after it
    uint32_t allocations = 0, failAfter = UINT32_MAX;
    uint8_t* at(uint32_t va) {
        if (va < kBase || va + 4 > kBase + bytes.size()) { std::fprintf(stderr, "FAIL: guest access %08x\n", va); std::exit(1); }
        return &bytes[va - kBase];
    }
    uint32_t read32(uint32_t va) { uint32_t v; std::memcpy(&v, at(va), 4); return v; }
    void write32(uint32_t va, uint32_t v) { std::memcpy(at(va), &v, 4); }
    std::string string(uint32_t va) { std::string s; while (*at(va)) s += char(*at(va++)); return s; }
    nfs_guest_memory memory() {
        return {[](void* c, uint32_t va) { return static_cast<Guest*>(c)->read32(va); },
                [](void* c, uint32_t va, uint32_t v) { static_cast<Guest*>(c)->write32(va, v); },
                [](void* c, uint32_t n) -> uint32_t {
                    Guest* g = static_cast<Guest*>(c);
                    if (g->allocations >= g->failAfter) return 0;
                    ++g->allocations;
                    uint32_t va = g->next;
                    g->next += (n + 15) & ~15u;
                    return va;
                },
                this};
    }
};

static const nfs_freeroam_track kTracks[] = {
    {1000, 1001, "L1RA", "Location1\\RegionA", "Olympic City", 100.0f, 900.0f, 3000.0f, 1003},
    {1300, 1301, "L1RB", "Location1\\RegionB", "Industrial Drift", 0, 0, 0, 1301},
    {1400, 1305, "L1RC", "Location1\\RegionC", "Parkade Drift", -50.0f, 25.0f, 512.0f, 1305},
};

// A stock-like record: name, number, modes, map placement; a pattern elsewhere so copies can be told apart.
static void stockRecord(uint8_t* r, unsigned number, const char* name, const char* region) {
    for (unsigned i = 0; i < NFS_TRACK_RECORD_BYTES; ++i) r[i] = uint8_t(number * 7 + i);
    std::memset(r, 0, 0x20); std::memcpy(r, name, std::strlen(name));
    std::memset(r + 0x40, 0, 8); std::memcpy(r + 0x40, region, std::strlen(region));
    r[0x68] = 1; r[0x69] = 0; r[0x6A] = 2; r[0x6B] = 0;   /* as the stock races have them */
    const float laps[2] = {71600.0f, 78380.0f}; std::memcpy(r + 0x7C, laps, 8);   /* 1001's default best laps (ms) */
    r[0x6C] = uint8_t(number); r[0x6D] = uint8_t(number >> 8);
    r[0x6E] = r[0x6C]; r[0x6F] = r[0x6D];
    const uint32_t modes = 0x71; std::memcpy(r + 0x74, &modes, 4);
    const float place[3] = {731.0f, -359.0f, 1525.0f}; std::memcpy(r + 0x8C, place, 12);
}

static void testRecord() {
    uint8_t base[NFS_TRACK_RECORD_BYTES], out[NFS_TRACK_RECORD_BYTES];
    stockRecord(base, 1001, "CT1 - Market St.", "L1RA");
    nfs_freeroam_record(out, base, &kTracks[0]);
    require(std::string(reinterpret_cast<char*>(out)) == "Olympic City", "record: the name at +0");
    require(std::string(reinterpret_cast<char*>(out + 0x20)) == "Location1\\Track_1000", "record: its directory");
    require(std::string(reinterpret_cast<char*>(out + 0x40)) == "L1RA" && std::string(reinterpret_cast<char*>(out + 0x48)) == "Location1\\RegionA",
            "record: region and region directory");
    require(out[0x6C] == (1000 & 0xFF) && out[0x6D] == (1000 >> 8) && out[0x6E] == out[0x6C] && out[0x6F] == out[0x6D], "record: the number twice");
    uint32_t modes; std::memcpy(&modes, out + 0x74, 4);
    require(modes == NFS_TRACK_MODE_FREE_RUN && modes == 0x10, "record: offered in Free Run (race type 3, bit 0x10) only");
    require(out[0x6A] == 2 && out[0x68] == 1, "record: the base's other fields kept, as in Beta 2.0's records");
    float lapForward = 1, lapReverse = 1;
    std::memcpy(&lapForward, out + 0x7C, 4); std::memcpy(&lapReverse, out + 0x80, 4);
    require(lapForward == 0 && lapReverse == 0, "record: no lap times (SELECT LOCATION shows the base race's otherwise)");
    require(out[0x68] == base[0x68] && out[0x69] == base[0x69], "record: valid and looping as the base");
    float place[3]; std::memcpy(place, out + 0x8C, 12);
    require(place[0] == 100.0f && place[1] == 900.0f && place[2] == 3000.0f && out[0x98] == 0 && out[0x99] == 0, "record: the made map's placement, no rotation");
    require(std::string(reinterpret_cast<char*>(out + 0xDC)) == "Olympic City", "record: the second name");
    for (unsigned i = 0x78; i < 0x8C; ++i)
        if (i < 0x7C || i >= 0x84) require(out[i] == base[i], "record: the base's other lengths and scores kept");
    for (unsigned i = 0x9A; i < 0xDC; ++i) require(out[i] == base[i], "record: the base's camera, difficulty and traffic kept");
    nfs_freeroam_record(out, base, &kTracks[1]);   // width 0: the base's map placement stays
    std::memcpy(place, out + 0x8C, 12);
    require(place[0] == 731.0f && place[1] == -359.0f && place[2] == 1525.0f, "record: no map keeps the base's placement");
    nfs_freeroam_track longName = kTracks[0];
    std::memset(longName.name, 'x', sizeof longName.name);   // no terminator at all
    nfs_freeroam_record(out, base, &longName);
    require(std::strlen(reinterpret_cast<char*>(out)) == 0x1F && out[0x1F] == 0, "record: a long name is cut, terminated");
}

static void testIcons() {
    require(nfs_freeroam_icon_hash(1003) == nfs_bin_hash("TRACK_1003_ICON") && nfs_freeroam_icon_hash(7) == nfs_bin_hash("TRACK_7_ICON"),
            "icons: TRACK_<n>_ICON hashed as the game does");
    nfs_freeroam_track tracks[3] = {kTracks[0], kTracks[1], kTracks[2]};
    tracks[0].icon = 1003; tracks[1].icon = 1301; tracks[2].icon = 0;
    require(nfs_freeroam_entry_icon(tracks, 3, nfs_bin_hash("TRACK_1000_ICON")) == nfs_bin_hash("TRACK_1003_ICON"), "icons: Olympic City shows 1003's");
    require(nfs_freeroam_entry_icon(tracks, 3, nfs_bin_hash("TRACK_1300_ICON")) == nfs_bin_hash("TRACK_1301_ICON"), "icons: 1300 shows 1301's");
    require(nfs_freeroam_entry_icon(tracks, 3, nfs_bin_hash("TRACK_1400_ICON")) == nfs_bin_hash("TRACK_1400_ICON"), "icons: none set, the game's own");
    require(nfs_freeroam_entry_icon(tracks, 3, nfs_bin_hash("TRACK_1001_ICON")) == nfs_bin_hash("TRACK_1001_ICON"), "icons: a race's own untouched");
    require(nfs_freeroam_entry_icon(tracks, 0, nfs_bin_hash("TRACK_1000_ICON")) == nfs_bin_hash("TRACK_1000_ICON"), "icons: free roam off");
    // SELECT LOCATION's reversed loop skips a record whose race types have the drift bit
    require(nfs_freeroam_reverse_types(tracks, 3, 1000, NFS_TRACK_MODE_FREE_RUN) == (NFS_TRACK_MODE_FREE_RUN | NFS_TRACK_MODE_DRIFT),
            "reverse: a free roam track gets the drift bit (one direction)");
    require(nfs_freeroam_reverse_types(tracks, 3, 1400, 0x10) == 0x18, "reverse: every free roam track");
    require(nfs_freeroam_reverse_types(tracks, 3, 1001, 0x71) == 0x71, "reverse: a circuit keeps its reversed entry");
    require(nfs_freeroam_reverse_types(tracks, 3, 1305, 0x08) == 0x08, "reverse: a drift race as it is");
    require(nfs_freeroam_reverse_types(tracks, 0, 1000, 0x10) == 0x10, "reverse: free roam off");
}

static void testHashes() {
    require(nfs_bin_hash("") == 0xFFFFFFFFu && nfs_bin_hash("A") == 32u, "BinHash: the game's seed and step");
    uint32_t keys[2];
    nfs_freeroam_name_keys(1000, keys);
    require(keys[0] == nfs_bin_hash("TRACK_NAME_1000") && keys[1] == nfs_bin_hash("REVERSE_TRACK_NAME_1000"), "name keys of 1000");
    nfs_freeroam_name_keys(7, keys);
    require(keys[0] == nfs_bin_hash("TRACK_NAME_7"), "name keys of a short number");
    require(free_roam::binHash("TRACK1000_MAP") == nfs_bin_hash("TRACK1000_MAP"), "host and game side hash alike");
}

// The game's table: `count` records at 0x10000100 (1001, 1301, 1305 among stock ones).
static uint32_t gameTable(Guest& g, const std::vector<unsigned>& numbers) {
    const uint32_t table = Guest::kBase + 0x100;
    for (size_t i = 0; i < numbers.size(); ++i) {
        uint8_t r[NFS_TRACK_RECORD_BYTES];
        stockRecord(r, numbers[i], "stock", numbers[i] < 1300 ? "L1RA" : numbers[i] < 1305 ? "L1RB" : "L1RC");
        std::memcpy(g.at(table + uint32_t(i) * NFS_TRACK_RECORD_BYTES), r, NFS_TRACK_RECORD_BYTES);
    }
    return table;
}

static void testAddRecords() {
    {   // stock GLOBALB: the three appended, in a copy; the game's own records untouched
        Guest g; nfs_guest_memory m = g.memory();
        const std::vector<unsigned> numbers = {1001, 1002, 1301, 1302, 1305, 1308};
        uint32_t table = gameTable(g, numbers), count = uint32_t(numbers.size());
        const std::vector<uint8_t> before(g.bytes.begin(), g.bytes.begin() + 0x100 + numbers.size() * NFS_TRACK_RECORD_BYTES);
        require(nfs_freeroam_add_records(&m, kTracks, 3, &table, &count) == 3, "stock: three records written");
        require(count == numbers.size() + 3 && table != Guest::kBase + 0x100, "stock: appended in a new table");
        require(std::equal(before.begin(), before.end(), g.bytes.begin()), "stock: the game's chunk is not written");
        for (size_t i = 0; i < numbers.size(); ++i)
            require(std::memcmp(g.at(table + uint32_t(i) * NFS_TRACK_RECORD_BYTES), g.at(Guest::kBase + 0x100 + uint32_t(i) * NFS_TRACK_RECORD_BYTES),
                                NFS_TRACK_RECORD_BYTES) == 0, "stock: the game's records copied first, in order");
        for (unsigned k = 0; k < 3; ++k) {
            const uint32_t r = table + uint32_t(numbers.size() + k) * NFS_TRACK_RECORD_BYTES;
            require((g.read32(r + 0x6C) & 0xFFFF) == kTracks[k].number, "stock: the free roam numbers in order");
            require(g.string(r) == kTracks[k].name, "stock: their names");
            uint8_t base[NFS_TRACK_RECORD_BYTES], expected[NFS_TRACK_RECORD_BYTES];
            const uint32_t at = Guest::kBase + 0x100 + uint32_t(std::find(numbers.begin(), numbers.end(), kTracks[k].base) - numbers.begin()) * NFS_TRACK_RECORD_BYTES;
            std::memcpy(base, g.at(at), sizeof base);
            nfs_freeroam_record(expected, base, &kTracks[k]);
            require(std::memcmp(g.at(r), expected, NFS_TRACK_RECORD_BYTES) == 0, "stock: each from its own base record");
        }
        // The hook runs again on a reload of the same table: in place now, nothing appended.
        uint32_t again = table, againCount = count;
        const uint32_t allocations = g.allocations;
        require(nfs_freeroam_add_records(&m, kTracks, 3, &again, &againCount) == 3 && again == table && againCount == count && g.allocations == allocations,
                "a table that has them: rewritten in place, no copy");
    }
    {   // Beta 2.0's GLOBALB has 1000, 1300 and 1400 already: rewritten in place
        Guest g; nfs_guest_memory m = g.memory();
        const std::vector<unsigned> numbers = {1000, 1001, 1300, 1301, 1305, 1400};
        uint32_t table = gameTable(g, numbers), count = uint32_t(numbers.size());
        require(nfs_freeroam_add_records(&m, kTracks, 3, &table, &count) == 3 && table == Guest::kBase + 0x100 && count == 6 && g.allocations == 0,
                "Beta 2.0's table: in place, no allocation");
        require(g.string(table) == "Olympic City" && (g.read32(table + 0x74)) == NFS_TRACK_MODE_FREE_RUN, "Beta 2.0's 1000: ours now, Free Run only");
    }
    {   // not the game's table (a base record missing): nothing at all
        Guest g; nfs_guest_memory m = g.memory();
        const std::vector<unsigned> numbers = {1001, 1301};
        uint32_t table = gameTable(g, numbers), count = 2;
        require(nfs_freeroam_add_records(&m, kTracks, 3, &table, &count) == 0 && count == 2 && table == Guest::kBase + 0x100 && g.allocations == 0,
                "no base record: the table stays");
    }
    {   // the game's malloc fails: the table stays as it is
        Guest g; nfs_guest_memory m = g.memory(); g.failAfter = 0;
        const std::vector<unsigned> numbers = {1001, 1301, 1305};
        uint32_t table = gameTable(g, numbers), count = 3;
        require(nfs_freeroam_add_records(&m, kTracks, 3, &table, &count) == 0 && count == 3 && table == Guest::kBase + 0x100, "no memory: unchanged");
    }
    {   // degenerate inputs
        Guest g; nfs_guest_memory m = g.memory();
        uint32_t table = 0, count = 5;
        require(nfs_freeroam_add_records(&m, kTracks, 3, &table, &count) == 0, "no table: nothing");
        table = Guest::kBase + 0x100; count = 0;
        require(nfs_freeroam_add_records(&m, kTracks, 3, &table, &count) == 0, "empty table: nothing");
        count = 5000;
        require(nfs_freeroam_add_records(&m, kTracks, 3, &table, &count) == 0, "absurd count: nothing");
    }
}

// The language: `pairs` sorted {hash, string} at 0x10000100, strings after.
static uint32_t languageTable(Guest& g, std::vector<std::pair<std::string, std::string>> entries) {
    std::sort(entries.begin(), entries.end(), [](const auto& a, const auto& b) { return nfs_bin_hash(a.first.c_str()) < nfs_bin_hash(b.first.c_str()); });
    const uint32_t table = Guest::kBase + 0x100;
    uint32_t strings = Guest::kBase + 0x8000;
    for (size_t i = 0; i < entries.size(); ++i) {
        g.write32(table + uint32_t(i) * 8, nfs_bin_hash(entries[i].first.c_str()));
        g.write32(table + uint32_t(i) * 8 + 4, strings);
        std::memcpy(g.at(strings), entries[i].second.c_str(), entries[i].second.size() + 1);
        strings += uint32_t(entries[i].second.size() + 1);
    }
    return table;
}
static std::string lookup(Guest& g, uint32_t table, uint32_t pairs, const char* key) {   // as 0x59FB80: binary search
    const uint32_t hash = nfs_bin_hash(key);
    uint32_t lo = 0, hi = pairs;
    while (lo < hi) {
        const uint32_t mid = (lo + hi) / 2, h = g.read32(table + mid * 8);
        if (h == hash) return g.string(g.read32(table + mid * 8 + 4));
        if (h < hash) lo = mid + 1; else hi = mid;
    }
    return "<missing>";
}

static void testAddNames() {
    Guest g; nfs_guest_memory m = g.memory();
    std::vector<std::pair<std::string, std::string>> entries = {
        {"TRACK_NAME_1001", "Market St."}, {"TRACK_NAME_1301", "Drift 1"}, {"REVERSE_TRACK_NAME_1001", "Market St. Rev"},
        {"HELLO", "Hello"}, {"TRACK_NAME_1400", "the game's own"}, {"ZZZ", "last"}, {"AAA", "first"}};
    uint32_t table = languageTable(g, entries), pairs = uint32_t(entries.size());
    uint32_t names[NFS_FREEROAM_MAX_TRACKS] = {0, 0, 0};
    require(nfs_freeroam_add_names(&m, kTracks, 3, names, &table, &pairs) == 5, "names: six keys, one the language has");
    require(pairs == entries.size() + 5 && table != Guest::kBase + 0x100, "names: merged into a copy");
    for (uint32_t i = 1; i < pairs; ++i) require(g.read32(table + (i - 1) * 8) < g.read32(table + i * 8), "names: still sorted, no duplicate hash");
    require(lookup(g, table, pairs, "TRACK_NAME_1000") == "Olympic City" && lookup(g, table, pairs, "REVERSE_TRACK_NAME_1000") == "Olympic City",
            "names: 1000 both ways");
    require(lookup(g, table, pairs, "TRACK_NAME_1300") == "Industrial Drift", "names: 1300");
    require(lookup(g, table, pairs, "TRACK_NAME_1400") == "the game's own", "names: the language's own entry wins");
    require(lookup(g, table, pairs, "REVERSE_TRACK_NAME_1400") == "Parkade Drift", "names: the reverse key still added");
    for (const auto& e : entries) require(lookup(g, table, pairs, e.first.c_str()) == e.second, "names: every game string still found");
    // A second load (another language): the strings made once are reused.
    uint32_t table2 = languageTable(g, entries), pairs2 = uint32_t(entries.size());
    const uint32_t saved[3] = {names[0], names[1], names[2]};
    require(nfs_freeroam_add_names(&m, kTracks, 3, names, &table2, &pairs2) == 5 && std::equal(saved, saved + 3, names), "names: made once, reused");
    // Edge: an empty language table or no allocator.
    uint32_t none = 0, zero = 0;
    require(nfs_freeroam_add_names(&m, kTracks, 3, names, &none, &zero) == 0, "names: no table, nothing");
    nfs_guest_memory noAlloc = m; noAlloc.alloc = nullptr;
    uint32_t t3 = languageTable(g, entries), p3 = uint32_t(entries.size());
    require(nfs_freeroam_add_names(&noAlloc, kTracks, 3, names, &t3, &p3) == 0 && p3 == entries.size(), "names: no allocator, unchanged");
}

static void testMergeEdges() {
    Guest g; nfs_guest_memory m = g.memory();
    const uint32_t table = Guest::kBase + 0x100, out = Guest::kBase + 0x2000;
    for (uint32_t i = 0; i < 4; ++i) { g.write32(table + i * 8, 100 + 10 * i); g.write32(table + i * 8 + 4, 1000 + i); }
    const uint32_t before[4] = {1, 1001, 5, 1002};   // {hash, value}: both before the table's first
    require(nfs_merge_strings(&m, table, 4, before, 2, out) == 6 && g.read32(out) == 1 && g.read32(out + 8) == 5 && g.read32(out + 16) == 100,
            "merge: extras ahead of the table");
    const uint32_t after[4] = {999, 7, 130, 8};      // one after everything, one equal to the last
    require(nfs_merge_strings(&m, table, 4, after, 2, out) == 5 && g.read32(out + 32) == 999 && g.read32(out + 28) == 1003, "merge: equal hash keeps the table's");
    require(nfs_merge_strings(&m, table, 4, nullptr, 0, out) == 4, "merge: nothing to add");
}

static nfs_car_state drivingCar(float x, float y) {
    nfs_car_state c{};
    c.x = x; c.y = y; c.z = 10.0f;
    c.vx = 20.0f; c.vy = 0.0f; c.vz = 0.0f;
    c.fx = 1.0f; c.fy = 0.0f; c.up_z = 1.0f;
    c.reset_start = 0; c.reset_length = 0;
    return c;
}

static void testCrumbs() {
    nfs_crumb_trail trail{};
    nfs_crumb crumb;
    nfs_car_state car = drivingCar(1, 2);
    require(nfs_freeroam_crumb(&trail, &car, 1000, &crumb) == 1 && crumb.x == 1 && crumb.y == 2 && crumb.tick == 1000, "a driving car leaves a crumb");
    nfs_crumb_add(&trail, &crumb);
    require(nfs_freeroam_crumb(&trail, &car, 1000 + NFS_CRUMB_INTERVAL - 1, &crumb) == 0, "not within a second of the last");
    require(nfs_freeroam_crumb(&trail, &car, 1000 + NFS_CRUMB_INTERVAL, &crumb) == 1, "a second later, the next");
    nfs_car_state slow = car; slow.vx = 3.0f;
    require(nfs_freeroam_crumb(&trail, &slow, 90000, &crumb) == 0, "too slow: stuck or crawling, no crumb");
    nfs_car_state rolled = car; rolled.up_z = 0.2f;
    require(nfs_freeroam_crumb(&trail, &rolled, 90000, &crumb) == 0, "on its side: no crumb");
    nfs_car_state roof = car; roof.up_z = -1.0f;
    require(nfs_freeroam_crumb(&trail, &roof, 90000, &crumb) == 0, "on its roof: no crumb");
    nfs_car_state climbing = car; climbing.fx = 0.3f;
    require(nfs_freeroam_crumb(&trail, &climbing, 90000, &crumb) == 0, "nose up a wall: no crumb");
    nfs_car_state flashing = car; flashing.reset_start = 88000; flashing.reset_length = 1.0f;
    require(nfs_freeroam_crumb(&trail, &flashing, 90000, &crumb) == 0, "within a reset's flash: where the game put it");
    require(nfs_freeroam_crumb(&trail, &flashing, 92001, &crumb) == 1, "after the flash: crumbs again");
    nfs_car_state never = car; never.reset_start = 0x7FFFFFFFu; never.reset_length = 1e9f;
    require(nfs_freeroam_crumb(&trail, &never, 95000, &crumb) == 1, "0x7FFFFFFF: no flash");
    nfs_car_state nan = car; nan.x = std::nanf("");
    require(nfs_freeroam_crumb(&trail, &nan, 99000, &crumb) == 0, "NaN position: no crumb");
    nfs_car_state inf = car; inf.z = INFINITY;
    require(nfs_freeroam_crumb(&trail, &inf, 99000, &crumb) == 0, "infinite position: no crumb");
    nfs_car_state wild = car; wild.vx = 5000.0f;
    require(nfs_freeroam_crumb(&trail, &wild, 99000, &crumb) == 0, "an absurd speed: no crumb");

    // Picking: the newest at least min_age old, else the oldest; none in an empty trail.
    nfs_crumb_trail t{};
    require(nfs_crumb_pick(&t, 100000, 12000) == nullptr, "pick: none in an empty trail");
    for (uint32_t s = 0; s < 10; ++s) {
        nfs_crumb c{float(s), 0, 0, 1, 0, s * 4000u};
        nfs_crumb_add(&t, &c);
    }
    const nfs_crumb* p = nfs_crumb_pick(&t, 9 * 4000u, 12000);
    require(p && p->x == 6.0f, "pick: the newest three seconds old");
    p = nfs_crumb_pick(&t, 9 * 4000u + 3999u, 12000);
    require(p && p->x == 6.0f, "pick: not a younger one");
    p = nfs_crumb_pick(&t, 36500, 40000);
    require(p && p->x == 0.0f, "pick: all too young, the oldest");
    p = nfs_crumb_pick(&t, 1000, 12000);
    require(p && p->x == 0.0f, "pick: crumbs from after now are never taken");
    // The trail belongs to a race: another record, or the clock starting again, begins a new one.
    uint32_t record = 0;
    nfs_crumb_trail r{};
    require(nfs_crumb_trail_for(&r, &record, 0x1234, 100) == 1 && record == 0x1234, "trail: the first race");
    nfs_crumb one{1, 2, 3, 1, 0, 5000};
    nfs_crumb_add(&r, &one);
    require(nfs_crumb_trail_for(&r, &record, 0x1234, 9000) == 0 && r.count == 1, "trail: the same race goes on");
    require(nfs_crumb_trail_for(&r, &record, 0x1234, 4000) == 1 && r.count == 0, "trail: a restart (the clock went back) begins again");
    nfs_crumb_add(&r, &one);
    require(nfs_crumb_trail_for(&r, &record, 0x5678, 9000) == 1 && r.count == 0 && record == 0x5678, "trail: another race begins again");
    // The ring keeps the newest 64.
    for (uint32_t s = 10; s < 200; ++s) {
        nfs_crumb c{float(s), 0, 0, 1, 0, s * 4000u};
        nfs_crumb_add(&t, &c);
    }
    require(t.count == NFS_CRUMBS, "the ring holds 64");
    p = nfs_crumb_pick(&t, 199 * 4000u, 1u << 30);
    require(p && p->x == 136.0f, "pick: the oldest kept is 64 back");
    p = nfs_crumb_pick(&t, 199 * 4000u, 0);
    require(p && p->x == 199.0f, "pick: min age 0, the newest");
}

// ---- host side ------------------------------------------------------------------------------------------------

using free_roam::Bytes;
static void chunk(Bytes& out, uint32_t id, const Bytes& payload) { free_roam::appendChunk(out, id, payload); }
static Bytes u32s(std::initializer_list<uint32_t> values) {
    Bytes b; for (uint32_t v : values) { uint8_t w[4]; std::memcpy(w, &v, 4); b.insert(b.end(), w, w + 4); }
    return b;
}
// A section table of `n` entries: names SEC<i>, number, flag, file; offsets/sizes from `seed`.
static Bytes sectionTable(unsigned n, uint32_t seed) {
    Bytes t(n * 56, 0);
    for (unsigned i = 0; i < n; ++i) {
        uint8_t* e = &t[i * 56];
        char name[16];
        std::snprintf(name, sizeof name, "SEC%u", i % 1000);
        std::memcpy(e, name, std::min<size_t>(std::strlen(name), 7));
        free_roam::write16(e + 8, uint16_t(2400 + i));
        e[0x0A] = uint8_t(i & 1);
        free_roam::write32(e + 0x10, i % 3);
        free_roam::write32(e + 0x14, seed + i * 0x1000);
        free_roam::write32(e + 0x18, 0x800 + i + seed);
        free_roam::write32(e + 0x1C, 0x400 + i);
        free_roam::write32(e + 0x20, seed ^ i);
    }
    return t;
}
static Bytes bundle(const Bytes& table, uint32_t streamSeed, const Bytes& polygons) {
    Bytes b;
    chunk(b, 0x00034110u, Bytes(40, 7));
    chunk(b, free_roam::kSectionTable, table);
    chunk(b, free_roam::kStreamSizes, u32s({streamSeed, streamSeed * 2, 0}));
    if (!polygons.empty()) chunk(b, free_roam::kTrackPolygons, polygons);
    Bytes nested;
    chunk(nested, 0x00034201u, Bytes(4, 1));
    chunk(b, 0x80034100u, nested);
    return b;
}
static Bytes quadPolygons(const std::vector<std::array<float, 4>>& rects) {   // x0, y0, x1, y1 in metres
    Bytes p;
    for (const auto& r : rects) {
        uint8_t q[32] = {0};
        const float xs[4] = {r[0], r[2], r[2], r[0]}, ys[4] = {r[1], r[1], r[3], r[3]};
        for (int k = 0; k < 4; ++k) {
            int16_t x = int16_t(std::lround(xs[k] * 8)), y = int16_t(std::lround(ys[k] * 8));
            std::memcpy(q + 8 + 2 * k, &x, 2);
            std::memcpy(q + 16 + 2 * k, &y, 2);
        }
        p.insert(p.end(), q, q + 32);
    }
    return p;
}

static void testChunks() {
    Bytes b = bundle(sectionTable(3, 10), 5, {});
    std::vector<free_roam::Chunk> top;
    require(free_roam::topChunks(b, top) && top.size() == 4, "chunks: the top level");
    free_roam::Chunk c;
    require(free_roam::findNested(b, 0x00034201u, c) && free_roam::read32(&b[c.offset + 8]) == 0x01010101u, "chunks: found inside a container");
    require(!free_roam::findNested(b, 0x12345678u, c), "chunks: a missing id");
    Bytes broken = b; free_roam::write32(&broken[4], 0x7FFFFFFFu);
    require(!free_roam::topChunks(broken, top), "chunks: a size past the end");
    require(!free_roam::findNested(broken, 0x00034201u, c), "chunks: nested search stops at damage");
}

static void testTransplant() {
    const Bytes freeRoamTable = sectionTable(6, 100), installedTable = sectionTable(6, 777);
    Bytes track = bundle(freeRoamTable, 5, quadPolygons({{0, 0, 10, 10}}));
    const Bytes base = bundle(installedTable, 9, {});
    const Bytes untouched = track;
    require(free_roam::transplantSections(track, base).empty(), "transplant: same sections, moved");
    std::vector<free_roam::Chunk> t; free_roam::topChunks(track, t);
    const free_roam::Chunk* table = free_roam::findChunk(t, free_roam::kSectionTable);
    const free_roam::Chunk* sizes = free_roam::findChunk(t, free_roam::kStreamSizes);
    require(std::equal(installedTable.begin(), installedTable.end(), track.begin() + long(table->offset + 8)), "transplant: the installed table");
    require(free_roam::read32(&track[sizes->offset + 8]) == 9 && free_roam::read32(&track[sizes->offset + 12]) == 18, "transplant: the installed stream sizes");
    require(track.size() == untouched.size(), "transplant: nothing else moves");
    for (const auto& field : {0, 8, 0x0A, 0x10}) {   // name, number, flag, file: a different section
        Bytes other = sectionTable(6, 777);
        other[3 * 56 + size_t(field)] ^= 0x5A;
        Bytes again = untouched;
        require(!free_roam::transplantSections(again, bundle(other, 9, {})).empty() && again == untouched, "transplant: another section refused, unchanged");
    }
    Bytes again = untouched;
    require(!free_roam::transplantSections(again, bundle(sectionTable(5, 777), 9, {})).empty() && again == untouched, "transplant: other count refused");
    Bytes noTable; chunk(noTable, 0x1234, Bytes(8, 0));
    require(!free_roam::transplantSections(again, noTable).empty() && again == untouched, "transplant: no table refused");
}

// A texture pack with `textures` A8R8G8B8 textures named T<i> of 4x4, and the stock map TRACK1001_MAP.
static free_roam::TexturePack syntheticPack() {
    free_roam::TexturePack pack;
    pack.header.assign(124, 0); std::memcpy(&pack.header[0], "TRACK", 5);
    pack.dataHeader.assign(24, 0); pack.dataHeader[0] = 1;
    auto add = [&](const std::string& name, uint32_t w, uint32_t h, uint8_t fill, uint8_t flag) {
        free_roam::Texture t;
        t.record.assign(124, 0);
        std::memcpy(&t.record[12], name.c_str(), name.size());
        free_roam::write32(&t.record[0x24], free_roam::binHash(name.c_str()));
        free_roam::write16(&t.record[0x44], uint16_t(w)); free_roam::write16(&t.record[0x46], uint16_t(h));
        t.record[0x4A] = free_roam::kA8R8G8B8; t.record[0x4E] = 1;
        t.format.assign(32, 0); t.format[8] = flag; t.format[16] = uint8_t(flag + 1); free_roam::write32(&t.format[20], 21);
        t.pixels.assign(size_t(w) * h * 4, fill);
        pack.textures.push_back(std::move(t));
    };
    add("SKY_A", 8, 4, 0x11, 3);
    add("TRACK1001_MAP", 4, 4, 0x80, 9);
    add("SKY_B", 4, 8, 0x22, 5);
    return pack;
}

static void testPacks() {
    free_roam::TexturePack pack = syntheticPack();
    const Bytes file = free_roam::serializePack(pack);
    free_roam::TexturePack parsed;
    require(free_roam::parsePack(file, parsed).empty() && parsed.textures.size() == 3, "pack: parses");
    require(free_roam::serializePack(parsed) == file, "pack: a round trip is byte-identical");
    for (size_t i = 1; i < parsed.textures.size(); ++i) require(parsed.textures[i - 1].hash() < parsed.textures[i].hash(), "pack: by hash");
    for (const auto& t : parsed.textures) require(free_roam::read32(&t.record[0x30]) % 128 == 0, "pack: pixel data 128-aligned");
    std::vector<free_roam::Chunk> top; free_roam::topChunks(file, top);
    require(top.size() == 2 && top[1].id == 0 && top[1].size == 4096, "pack: the trailing padding chunk");
    // The map replaces the base race's, under the free roam track's name.
    Bytes dxt(free_roam::mipBytes(8, 8, free_roam::kDXT5), 0x5A);
    require(!free_roam::setTexture(parsed, "TRACK1000_MAP", "TRACK1001_MAP", 8, 6, free_roam::kDXT5, dxt), "setTexture: not a power of two refused");
    require(!free_roam::setTexture(parsed, "TRACK1000_MAP", "TRACK1001_MAP", 8, 8, free_roam::kDXT5, Bytes(3)), "setTexture: wrong size refused");
    require(free_roam::setTexture(parsed, "TRACK1000_MAP", "TRACK1001_MAP", 8, 8, free_roam::kDXT5, dxt), "setTexture: replaces");
    const Bytes withMap = free_roam::serializePack(parsed);
    free_roam::TexturePack again;
    require(free_roam::parsePack(withMap, again).empty() && again.textures.size() == 3, "setTexture: still three textures");
    bool found = false;
    for (const auto& t : again.textures) {
        require(t.name() != "TRACK1001_MAP", "setTexture: the base race's map is gone");
        if (t.name() != "TRACK1000_MAP") continue;
        found = true;
        require(t.hash() == free_roam::binHash("TRACK1000_MAP") && free_roam::read16(&t.record[0x44]) == 8 && t.record[0x48] == 3, "setTexture: name, hash, size");
        require(t.record[0x4A] == free_roam::kDXT5 && std::memcmp(&t.format[20], "DXT5", 4) == 0 && t.format[8] == 9 && t.format[16] == 10,
                "setTexture: DXT5, the replaced texture's format flags kept");
        require(t.pixels == dxt && free_roam::read32(&t.record[0x40]) == dxt.size(), "setTexture: the pixels");
    }
    require(found, "setTexture: the map is there");
    require(free_roam::setTexture(again, "NEW_TEX", "", 4, 4, free_roam::kA8R8G8B8, Bytes(64, 1)) && again.textures.size() == 4, "setTexture: adds");
    Bytes notPack = file; free_roam::write32(&notPack[0], 0x12345678u);
    require(!free_roam::parsePack(notPack, parsed).empty(), "pack: not a pack refused");
    Bytes cut(file.begin(), file.begin() + 300);
    require(!free_roam::parsePack(cut, parsed).empty(), "pack: a cut file refused");
}

static void testDxt() {
    free_roam::Image im; im.width = im.height = 16; im.bgra.resize(16 * 16 * 4);
    for (uint32_t y = 0; y < 16; ++y) for (uint32_t x = 0; x < 16; ++x) {
        uint8_t* p = im.at(x, y);
        p[0] = uint8_t(x * 16); p[1] = uint8_t(y * 16); p[2] = uint8_t(128); p[3] = uint8_t((x + y) * 8);
    }
    const Bytes dxt = free_roam::encodeDXT5(im);
    require(dxt.size() == 16 * 16, "DXT5: 16 bytes a block");
    double error = 0, alphaError = 0;
    for (uint32_t by = 0; by < 4; ++by) for (uint32_t bx = 0; bx < 4; ++bx) {
        uint8_t texel[16][4];
        free_roam::decodeBlock(&dxt[(by * 4 + bx) * 16], free_roam::kDXT5, texel);
        for (int i = 0; i < 16; ++i) {
            const uint8_t* p = im.at(bx * 4 + uint32_t(i % 4), by * 4 + uint32_t(i / 4));
            for (int c = 0; c < 3; ++c) error = std::max(error, std::fabs(double(texel[i][c]) - p[c]));
            alphaError = std::max(alphaError, std::fabs(double(texel[i][3]) - p[3]));
        }
    }
    std::printf("  DXT5 round trip of a 2D gradient: colour error %.0f, alpha error %.0f\n", error, alphaError);
    require(error <= 40 && alphaError <= 4, "DXT5: a 2D gradient within a block's line and the 8 alpha levels");
    // Along one direction (what a map's roads and blocks mostly are) the line fits: within 565 rounding.
    for (uint32_t y = 0; y < 16; ++y) for (uint32_t x = 0; x < 16; ++x) {
        uint8_t* p = im.at(x, y);
        p[0] = uint8_t(x * 16); p[1] = uint8_t(x * 8); p[2] = uint8_t(255 - x * 12); p[3] = 255;
    }
    const Bytes line = free_roam::encodeDXT5(im);
    error = 0;
    for (uint32_t by = 0; by < 4; ++by) for (uint32_t bx = 0; bx < 4; ++bx) {
        uint8_t texel[16][4];
        free_roam::decodeBlock(&line[(by * 4 + bx) * 16], free_roam::kDXT5, texel);
        for (int i = 0; i < 16; ++i) {
            const uint8_t* p = im.at(bx * 4 + uint32_t(i % 4), by * 4 + uint32_t(i / 4));
            for (int c = 0; c < 3; ++c) error = std::max(error, std::fabs(double(texel[i][c]) - p[c]));
            require(texel[i][3] == 255, "DXT5: opaque stays opaque");
        }
    }
    std::printf("  DXT5 round trip of a 1D gradient: colour error %.0f\n", error);
    require(error <= 10, "DXT5: a 1D gradient survives encoding");
    // DXT1: three-colour mode (c0 <= c1) has a transparent black index 3; four-colour mode does not.
    uint8_t block[8] = {0x00, 0x00, 0xFF, 0xFF, 0xE4, 0xE4, 0xE4, 0xE4};   // c0 black, c1 white; indices 0,1,2,3 per row
    uint8_t texel[16][4];
    free_roam::decodeBlock(block, free_roam::kDXT1, texel);
    require(texel[0][3] == 255 && texel[3][3] == 0 && texel[3][0] == 0 && texel[2][1] > 120 && texel[2][1] < 135, "DXT1: three colours and transparent");
    // A DXT1 map decodes into an image.
    free_roam::TexturePack pack = syntheticPack();
    Bytes one(8, 0); one[0] = 0x1F;   // c0 = pure blue, indices 0
    free_roam::setTexture(pack, "TRACK1001_MAP", "", 4, 4, free_roam::kDXT1, one);
    free_roam::Image map;
    require(free_roam::mapImage(pack, "TRACK1001_MAP", map) && map.width == 4 && map.at(1, 1)[0] == 255 && map.at(1, 1)[2] == 0 && map.at(1, 1)[3] == 255,
            "mapImage: a DXT1 map");
    require(!free_roam::mapImage(pack, "TRACK9999_MAP", map), "mapImage: none");
}

static void testMap() {
    // Roads: a long horizontal street and a stray polygon far away.
    std::vector<std::array<float, 4>> rects;
    for (int i = 0; i < 100; ++i) rects.push_back({float(i * 10), 0, float(i * 10 + 10), 12});
    const Bytes polys = quadPolygons(rects);
    Bytes track = bundle(sectionTable(2, 1), 1, polys);
    auto quads = free_roam::roadQuads(track);
    require(quads.size() == 100 && quads[5].x[0] == 50.0f && quads[5].y[2] == 12.0f, "roads: read from 1/8 m");
    free_roam::MapPlacement place = free_roam::placeMap(quads);
    require(place.width >= 1000 && place.offX <= 0 && place.offX + place.width >= 1000 && place.offY >= 12 && place.offY - place.width <= 0,
            "placement: covers the roads");
    // a stray polygon 30 km away (1 % of them) does not shrink the city to a dot
    auto stray = quads;
    { free_roam::Quad q = quads[0]; for (int j = 0; j < 4; ++j) q.x[j] += 30000; stray.push_back(q); }
    const free_roam::MapPlacement robust = free_roam::placeMap(stray);
    require(robust.width < 2000 && robust.offX <= 0, "placement: a stray polygon ignored");
    require(free_roam::mapSize(3200) == 2048 && free_roam::mapSize(800) == 512 && free_roam::mapSize(10) == 256 && free_roam::mapSize(1e6f) == 2048,
            "map size: about 1.6 m a pixel");
    // A background tile: a stock map placed over the street's left half, all a dark teal.
    free_roam::MapTile tile;
    tile.image.width = tile.image.height = 8; tile.image.bgra.assign(8 * 8 * 4, 0);
    for (size_t i = 0; i < 64; ++i) { tile.image.bgra[i * 4] = 90; tile.image.bgra[i * 4 + 1] = 80; tile.image.bgra[i * 4 + 2] = 10; tile.image.bgra[i * 4 + 3] = 120; }
    tile.place = {place.offX, place.offY, place.width / 2};
    const uint32_t size = 256;
    const free_roam::Image map = free_roam::composeMap({tile}, quads, place, size);
    require(map.width == size && map.bgra.size() == size_t(size) * size * 4, "map: its size");
    auto pixel = [&](float x, float y) {
        const uint32_t px = uint32_t((x - place.offX) / place.width * size), py = uint32_t((place.offY - y) / place.width * size);
        return map.at(std::min(px, size - 1), std::min(py, size - 1));
    };
    const uint8_t* road = pixel(500, 6);
    require(road[0] > 200 && road[1] > 150 && road[3] > 200, "map: a road in the route colour");
    const uint8_t* ground = pixel(200, place.offY - place.width * 0.25f);
    require(ground[3] > 60 && ground[3] < 200 && ground[0] < 130, "map: the stock map's background where it lies");
    const uint8_t* empty = pixel(place.offX + place.width * 0.9f, place.offY - place.width * 0.9f);
    require(empty[3] == 0, "map: nothing where neither lies");
}

// GLOBALB-like: TrackInfo nested in a container.
static Bytes globalb(const std::vector<std::tuple<unsigned, const char*, uint32_t, float>>& tracks) {
    Bytes records;
    for (const auto& [number, region, modes, width] : tracks) {
        uint8_t r[NFS_TRACK_RECORD_BYTES];
        stockRecord(r, number, "stock", region);
        std::memcpy(r + 0x74, &modes, 4);
        std::memcpy(r + 0x94, &width, 4);
        records.insert(records.end(), r, r + NFS_TRACK_RECORD_BYTES);
    }
    Bytes inner, out;
    chunk(inner, 0x00034200u, Bytes(16, 0));
    chunk(inner, free_roam::kTrackInfo, records);
    chunk(out, 0x00030201u, Bytes(8, 0));
    chunk(out, 0x80034000u, inner);
    return out;
}

static void testRecords() {
    const Bytes g = globalb({{1001, "L1RA", 0x71, 1525}, {1002, "L1RA", 0x71, 1525}, {1201, "L1RB", 0x02, 3350}, {1301, "L1RB", 0x08, 151},
                             {1000, "L1RA", 0x71, 3000}, {1099, "L1RX", 0x00, 3350}, {1104, "L1RA", 0x71, 0}, {1302, "L1RB", 0x08, 234}});
    const auto records = free_roam::trackRecords(g);
    require(records.size() == 8 && records[3].number == 1301 && records[3].region == "L1RB" && records[3].mapWidth == 151.0f, "records: read");
    const auto city = free_roam::mapSources(records, free_roam::kTracks[0]);
    require(city.size() == 2 && city[0].number == 1001 && city[1].number == 1002, "map sources: the district's races, not 1000 itself, not width 0");
    const auto industrial = free_roam::mapSources(records, free_roam::kTracks[1]);
    require(industrial.size() == 2 && industrial[0].number == 1301 && industrial[1].number == 1302, "map sources: drags have no minimap");
    require(free_roam::trackRecords(Bytes(16, 0)).empty(), "records: none in a file without them");
}

static void testMakeTrack() {
    std::vector<std::array<float, 4>> rects;
    for (int i = 0; i < 40; ++i) rects.push_back({float(i * 25), 0, float(i * 25 + 25), 10});
    const Bytes freeRoamBundle = bundle(sectionTable(4, 50), 3, quadPolygons(rects));
    const Bytes base = bundle(sectionTable(4, 900), 7, {});
    free_roam::TexturePack stock = syntheticPack();
    const Bytes tex = free_roam::serializePack(stock);
    free_roam::MadeTrack made = free_roam::makeTrack(free_roam::kTracks[0], freeRoamBundle, base, tex, {});
    require(made.error.empty() && made.mapError.empty() && !made.track.empty() && !made.tex.empty(), "made: the track and its map");
    require(made.quads == 40 && made.mapSize == free_roam::mapSize(made.place.width) && made.place.width > 1000, "made: the map's size and placement");
    free_roam::TexturePack texOut;
    require(free_roam::parsePack(made.tex, texOut).empty(), "made: its texture pack parses");
    bool map = false;
    for (const auto& t : texOut.textures) map |= t.name() == "TRACK1000_MAP" && free_roam::read16(&t.record[0x44]) == made.mapSize;
    require(map, "made: TRACK1000_MAP in it");
    Bytes check = freeRoamBundle;
    require(free_roam::transplantSections(check, base).empty() && check == made.track, "made: the bundle is the transplanted one");
    // The map cannot be made: the track still is.
    made = free_roam::makeTrack(free_roam::kTracks[0], freeRoamBundle, base, Bytes(100, 0), {});
    require(made.error.empty() && !made.mapError.empty() && !made.track.empty() && made.tex.empty() && made.place.width == 0, "made: without a map");
    // The tables differ: no track.
    made = free_roam::makeTrack(free_roam::kTracks[0], freeRoamBundle, bundle(sectionTable(3, 900), 7, {}), tex, {});
    require(!made.error.empty() && made.track.empty(), "made: another base race's table, not offered");
    // No polygons: no map, the track is still made.
    made = free_roam::makeTrack(free_roam::kTracks[0], bundle(sectionTable(4, 50), 3, {}), base, tex, {});
    require(made.error.empty() && made.mapError == "no road polygons" && made.tex.empty(), "made: no roads, no map");
}

// ---- the select screen's picture ----------------------------------------------------------------------------------

// Speed.exe's JDLZ decoder (0x58D190, tools/nfstex/tpk.py jdlz_game_decode): it counts down the packed size, refills
// flags1 right after the 8th token and flags2 right after the 8th reference, and does not bound its copies. False
// for anything it would get wrong (a read past the blob, a count gone negative, a copy past the output, a reference
// before the output, an incomplete output). `consumed`: the bytes it read.
static bool gameDecode(const Bytes& blob, Bytes& out, size_t& consumed) {
    if (blob.size() < 0x12 || std::memcmp(blob.data(), "JDLZ", 4) != 0 || blob[4] != 2) return false;
    const uint32_t rawSize = free_roam::read32(&blob[8]), packed = free_roam::read32(&blob[12]);
    if (packed != blob.size()) return false;
    out.assign(rawSize, 0);
    unsigned flags1 = blob[0x10] | 0x100u, flags2 = blob[0x11] | 0x100u;
    size_t pos = 0x12, written = 0;
    long remaining = long(packed) - 0x12;
    while (remaining) {
        if (flags1 & 1) {
            if (pos + 2 > packed) return false;
            const unsigned b1 = blob[pos], b0 = blob[pos + 1];
            size_t length, distance;
            if (flags2 & 1) { length = (((b1 & 0xF0u) << 4) | b0) + 3; distance = (b1 & 0x0Fu) + 1; }
            else { length = (b1 & 0x1Fu) + 3; distance = (((b1 & 0xE0u) << 3) | b0) + 0x11; }
            if (distance > written || written + length > rawSize) return false;
            for (size_t i = 0; i < length; ++i, ++written) out[written] = out[written - distance];
            flags2 >>= 1;
            pos += 2;
            remaining -= 2;
        } else {
            if (written < rawSize) {
                if (pos >= packed) return false;
                out[written++] = blob[pos++];
            }
            remaining -= 1;
        }
        flags1 >>= 1;
        if (flags1 == 1) { if (pos >= packed) return false; flags1 = blob[pos++] | 0x100u; remaining -= 1; }
        if (flags2 == 1) { if (pos >= packed) return false; flags2 = blob[pos++] | 0x100u; remaining -= 1; }
        if (remaining < 0) return false;
    }
    consumed = pos;
    return written == rawSize;
}

static uint32_t lcg(uint32_t& state) { state = state * 1664525u + 1013904223u; return state >> 8; }

static void testJdlz() {
    std::vector<Bytes> inputs;
    uint32_t seed = 7;
    inputs.push_back({});
    for (size_t n : {1, 2, 3, 7, 8, 9, 15, 16, 17, 18, 33}) {
        Bytes b(n);
        for (auto& c : b) c = uint8_t(lcg(seed));
        inputs.push_back(b);
    }
    // every length up to 300 of mixed data: whatever token or flag group the stream ends on
    for (size_t n = 1; n <= 300; ++n) {
        Bytes b(n);
        for (size_t i = 0; i < n; ++i) b[i] = uint8_t(i % 7 < 3 ? 0 : lcg(seed) & 3);
        inputs.push_back(b);
    }
    inputs.push_back(Bytes(100000, 0));                        // a long run: references of 4098
    Bytes noise(50000);
    for (auto& c : noise) c = uint8_t(lcg(seed));
    inputs.push_back(noise);
    Bytes blocks;                                               // DXT-like: repeated 16-byte blocks, some changed
    for (int i = 0; i < 4096; ++i) {
        uint8_t block[16] = {0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xFF, 0xFF, 0xFF, 0, 0, 0, 0};
        if (i % 37 == 0) for (auto& c : block) c = uint8_t(lcg(seed));
        blocks.insert(blocks.end(), block, block + 16);
    }
    inputs.push_back(blocks);
    Bytes far;                                                  // repeats 17-2064 bytes back: far references
    for (int i = 0; i < 3000; ++i) far.push_back(uint8_t(i % 1000 < 40 ? (i % 40) * 3 : lcg(seed)));
    inputs.push_back(far);
    bool all = true, oneUnread = true;
    for (const Bytes& in : inputs) {
        const Bytes packed = free_roam::jdlzCompress(in);
        Bytes out; size_t consumed = 0;
        const bool ok = gameDecode(packed, out, consumed) && out == in;
        all &= ok;
        oneUnread &= ok && consumed == packed.size() - 1;
        Bytes lenient;
        all &= rim_sizes::jdlzDecompress(packed, lenient) && lenient == in;
    }
    require(all, "JDLZ: every input comes back through the game's decoder and the lenient one");
    require(oneUnread, "JDLZ: ends as EA's streams do, one counted byte the decoder never reads");
    require(free_roam::jdlzCompress(Bytes(100000, 0)).size() < 200, "JDLZ: a run packs to almost nothing");
    require(free_roam::jdlzCompress(blocks).size() < blocks.size() / 8, "JDLZ: repeated blocks pack");
    require(free_roam::jdlzCompress(noise).size() <= noise.size() + noise.size() / 8 + 32, "JDLZ: noise grows by its flag bytes only");
}

// A streaming pack (as Global/DYNTEX.BIN): textures named as given, DXT5 of 8x8, their data JDLZ blobs.
static free_roam::StreamedTexture streamed(const std::string& name, uint8_t fill, uint32_t width = 8) {
    free_roam::StreamedTexture t;
    t.record.assign(124, 0);
    std::memcpy(&t.record[12], name.c_str(), std::min<size_t>(name.size(), 23));
    free_roam::write32(&t.record[0x24], free_roam::binHash(name.c_str()));
    free_roam::write32(&t.record[0x28], 0x1A93CF);   // the engine's own fields come along in a copy
    const Bytes pixels(free_roam::mipBytes(width, width, free_roam::kDXT5), fill);
    free_roam::write32(&t.record[0x38], uint32_t(pixels.size()));
    free_roam::write16(&t.record[0x44], uint16_t(width)); free_roam::write16(&t.record[0x46], uint16_t(width));
    t.record[0x4A] = free_roam::kDXT3; t.record[0x4E] = 4;
    t.format.assign(32, 0); t.format[8] = fill; std::memcpy(&t.format[20], "DXT3", 4);
    t.blob = free_roam::jdlzCompress(pixels);
    return t;
}
static free_roam::StreamedPack streamedPack(const std::vector<std::string>& names) {
    free_roam::StreamedPack pack;
    pack.header.assign(124, 0); std::memcpy(&pack.header[4], "DYNAMIC", 7);
    pack.dataHeader.assign(24, 0); pack.dataHeader[0] = 1;
    uint8_t fill = 1;
    for (const auto& n : names) pack.textures.push_back(streamed(n, fill++));
    return pack;
}
static Bytes streamedFile(const std::vector<std::string>& names) {
    free_roam::StreamedPack pack = streamedPack(names);
    return free_roam::serializeStreamedPack(pack);
}
static Bytes blobPixels(const free_roam::StreamedTexture& t) {
    Bytes out; size_t consumed = 0;
    return gameDecode(t.blob, out, consumed) ? out : Bytes{};
}

static void testStreamedPack() {
    free_roam::StreamedPack pack = streamedPack({"ZETA", "ALPHA", "TRACK_1001_SPLINE", "SPRINT_TRACKS_BACK"});
    const Bytes file = free_roam::serializeStreamedPack(pack);
    free_roam::StreamedPack parsed;
    require(free_roam::parseStreamedPack(file, parsed).empty() && parsed.textures.size() == 4, "streamed pack: parses");
    require(free_roam::serializeStreamedPack(parsed) == file, "streamed pack: a round trip is byte-identical");
    for (size_t i = 1; i < parsed.textures.size(); ++i) require(parsed.textures[i - 1].hash() < parsed.textures[i].hash(), "streamed pack: by hash");
    // the layout the game's files have: the data parent on a 128-byte boundary, the blobs after 120 bytes of filler
    std::vector<free_roam::Chunk> top; free_roam::topChunks(file, top);
    require(top.size() == 2 && top[0].id == free_roam::kTpk && top[1].id == 0 && top[1].size == 4096, "streamed pack: one pack, the padding chunk");
    free_roam::Chunk data, body, table;
    require(free_roam::findNested(file, free_roam::kTpkData, data) && data.offset % 128 == 0, "streamed pack: data parent 128-aligned");
    require(free_roam::findNested(file, free_roam::kTpkDataBody, body) && (body.offset + 8) % 128 == 8 && file[body.offset + 8] == 0x11 &&
            file[body.offset + 8 + 119] == 0x11, "streamed pack: body 8 past a boundary, 120 bytes of filler");
    require(free_roam::findNested(file, free_roam::kTpkCompressed, table) && table.size == 4 * 20, "streamed pack: the blob table");
    uint32_t expect = uint32_t(body.offset + 8 + 120);
    bool tableOk = true, pixelsOk = true;
    for (size_t i = 0; i < 4; ++i) {
        const uint8_t* e = &file[table.offset + 8 + i * 20];
        tableOk &= free_roam::read32(e) == parsed.textures[i].hash() && free_roam::read32(e + 4) == expect &&
                   free_roam::read32(e + 8) == parsed.textures[i].blob.size() && free_roam::read32(e + 12) == 0x100;
        expect += free_roam::read32(e + 8);
        pixelsOk &= blobPixels(parsed.textures[i]).size() == free_roam::read32(&parsed.textures[i].record[0x38]);
    }
    require(tableOk, "streamed pack: table entries {hash, file offset, size, 0x100} back to back");
    require(pixelsOk, "streamed pack: each blob decodes to its record's size");
    bool records = true;
    uint32_t cursor = 0;
    for (const auto& t : parsed.textures) {   // the uncompressed layout: pixel offsets, the aligned ends after them
        records &= free_roam::read32(&t.record[0x30]) == cursor && free_roam::read32(&t.record[0x34]) == (cursor + 0x80 + 127) / 128 * 128;
        cursor = (cursor + 0x80 + 127) / 128 * 128;
    }
    require(records, "streamed pack: records laid out as uncompressed");
    // not a streaming pack, damaged ones
    free_roam::TexturePack plain = syntheticPack();
    require(free_roam::parseStreamedPack(free_roam::serializePack(plain), parsed) == "not a streaming texture pack", "streamed pack: a TEX file refused");
    Bytes notPack = file; free_roam::write32(&notPack[0], 0x12345678u);
    require(!free_roam::parseStreamedPack(notPack, parsed).empty(), "streamed pack: not a pack refused");
    Bytes outside = file; free_roam::write32(&outside[table.offset + 8 + 4], uint32_t(file.size()));
    require(!free_roam::parseStreamedPack(outside, parsed).empty(), "streamed pack: a blob outside the file refused");
    Bytes cut(file.begin(), file.begin() + 200);
    require(!free_roam::parseStreamedPack(cut, parsed).empty(), "streamed pack: a cut file refused");
}

static void testPicture() {
    // the background 0x4B92A0 asks for
    const std::pair<unsigned, const char*> backgrounds[] = {
        {1000, "TRACK_1000"}, {1001, "TRACK_1001"}, {1099, "TRACK_1099"}, {1101, "SPRINT_TRACKS_BACK2"}, {1102, "SPRINT_TRACKS_BACK2"},
        {1103, "SPRINT_TRACKS_BACK"}, {1105, "SPRINT_TRACKS_BACK"}, {1106, "SPRINT_TRACKS_BACK2"}, {1109, "SPRINT_TRACKS_BACK2"},
        {1201, "TRACK_1201"}, {1214, "TRACK_1214"}, {1300, "DRIFT_TRACKS_BACK"}, {1304, "DRIFT_TRACKS_BACK"}, {1305, "DRIFT_TRACKS_BACK2"},
        {1308, "DRIFT_TRACKS_BACK2"}, {1400, "DRIFT_TRACKS_BACK2"}};
    bool mapping = true;
    for (const auto& [number, name] : backgrounds) mapping &= free_roam::selectBackground(number) == name;
    require(mapping, "picture: the background the game asks for, by track number");
    require(free_roam::pictureSource(free_roam::kTracks[0]) == "SPRINT_TRACKS_BACK" && free_roam::pictureSource(free_roam::kTracks[1]) == "DRIFT_TRACKS_BACK" &&
            free_roam::pictureSource(free_roam::kTracks[2]) == "DRIFT_TRACKS_BACK2", "picture: the city's art copied, the drift districts' the game's");
    for (const auto& t : free_roam::kTracks) require(free_roam::pictureFrame(t) != nullptr, "picture: every free roam track has a frame");
    // frames: north up, and turned (north to the right)
    const free_roam::SelectFrame up{"T", false, 100, 900, 400, 800}, turned{"T", true, 100, 900, 400, 800};
    float u, v;
    free_roam::selectPoint(up, 100, 900, u, v);
    require(u == 0 && v == 0, "frame: its corner");
    free_roam::selectPoint(up, 500, 100, u, v);
    require(u == 1 && v == 1, "frame: the far corner, y down the texture");
    free_roam::selectPoint(turned, -900, 100, u, v);
    require(u == 0 && v == 0, "frame turned: its corner");
    free_roam::selectPoint(turned, -100, 500, u, v);
    require(u == 1 && v == 1, "frame turned: u follows y, v follows x");
    // the route: a 10 m road across a 512 m frame at 1 m a pixel
    const free_roam::SelectFrame metre{"T", false, 0, 512, 512, 512};
    std::vector<std::array<float, 4>> rects = {{100, 300, 200, 310}};
    Bytes polys = quadPolygons(rects);
    polys.resize(polys.size() + 32, 0);            // a degenerate polygon at (0, 0): left out
    for (int k = 0; k < 4; ++k) { int16_t x = 50 * 8, y = 460 * 8; std::memcpy(&polys[32 + 8 + 2 * k], &x, 2); std::memcpy(&polys[32 + 16 + 2 * k], &y, 2); }
    const auto quads = free_roam::roadQuads(bundle(sectionTable(2, 1), 1, polys));
    const free_roam::Image picture = free_roam::pictureRoads(quads, metre, 512);
    require(picture.width == 512 && picture.height == 512, "route: the game's size");
    const uint8_t* inside = picture.at(150, 207);     // v = (512 - y): the road covers rows 202-211
    const uint8_t* edge = picture.at(150, 202);
    const uint8_t* outside = picture.at(150, 190);
    require(inside[0] == 255 && inside[1] == 255 && inside[2] == 255, "route: white (the screen tints it)");
    require(std::abs(int(inside[3]) - 82) <= 2, "route: a soft fill inside, as the game's drift courses");
    require(edge[3] == 255 && picture.at(100, 207)[3] == 255 && picture.at(199, 207)[3] == 255, "route: bright edges");
    require(outside[3] == 0 && picture.at(250, 207)[3] == 0, "route: nothing beside the road");
    require(picture.at(50, 52)[3] == 0, "route: a degenerate polygon draws nothing");
    const free_roam::Image sideways = free_roam::pictureRoads(quads, free_roam::SelectFrame{"T", true, 0, 0, 512, 512}, 512);
    require(free_roam::pictureSize(512) == 512 && free_roam::pictureSize(1024) == 1024 && free_roam::pictureSize(8) == 256 &&
            free_roam::pictureSize(700) == 1024 && free_roam::pictureSize(8192) == 2048, "route: the background's size, 256 to 2048");
    require(sideways.at(305, 150)[3] > 60 && sideways.at(150, 207)[3] == 0, "route: drawn turned in a turned frame");
}

static void testPicturePack() {
    // the city's art as an HD pack has it (1024), the drift backgrounds as the game's (512)
    free_roam::StreamedPack stock = streamedPack({"GAME_TIPS_NITROUS", "SPRINT_TRACKS_BACK", "DRIFT_TRACKS_BACK", "DRIFT_TRACKS_BACK2",
                                                  "TRACK_1001_SPLINE", "TRACK_1001"});
    for (auto& t : stock.textures) {
        const uint16_t width = t.name() == "SPRINT_TRACKS_BACK" ? 1024 : 512;
        free_roam::write16(&t.record[0x44], width); free_roam::write16(&t.record[0x46], width);
    }
    const Bytes dyntex = free_roam::serializeStreamedPack(stock);
    free_roam::StreamedPack before;
    free_roam::parseStreamedPack(dyntex, before);
    // roads where each frame shows them: the city's downtown, each drift district's middle
    auto roads = [](float x, float y) { return free_roam::roadQuads(bundle(sectionTable(2, 1), 1, quadPolygons({{x, y, x + 60, y + 12}}))); };
    std::vector<free_roam::Picture> pictures(3);
    pictures[0].number = 1000; pictures[0].quads = roads(900, -600);
    pictures[1].number = 1300; pictures[1].quads = roads(-70, -1280);
    pictures[2].number = 1400; pictures[2].quads = roads(-60, -1290);
    free_roam::PictureReport report;
    const Bytes made = free_roam::makePicturePack(dyntex, pictures, report);
    require(report.error.empty() && report.tracks == 3 && report.textures == 4 && report.skipped.empty(), "pictures: three tracks, four textures");
    free_roam::StreamedPack after;
    require(free_roam::parseStreamedPack(made, after).empty() && after.textures.size() == 10, "pictures: the pack parses, ten textures");
    auto find = [](const free_roam::StreamedPack& p, const std::string& name) -> const free_roam::StreamedTexture* {
        for (const auto& t : p.textures) if (t.name() == name) return &t;
        return nullptr;
    };
    bool kept = true;
    for (const auto& t : before.textures) kept &= find(after, t.name()) && find(after, t.name())->blob == t.blob;
    require(kept, "pictures: the pack's own textures as they were");
    const auto *city = find(after, "TRACK_1000"), *art = find(after, "SPRINT_TRACKS_BACK");
    require(city && art && city->blob == art->blob && city->hash() == free_roam::binHash("TRACK_1000") &&
            std::equal(art->record.begin() + 0x38, art->record.end(), city->record.begin() + 0x38) && city->format == art->format,
            "pictures: Olympic City's background a copy of the city's, under its own name");
    for (const auto& [number, size] : {std::pair<unsigned, uint32_t>{1000, 1024}, {1300, 512}, {1400, 512}}) {
        const auto* t = find(after, free_roam::selectRoute(number));
        require(t != nullptr, "pictures: each track's route added");
        if (!t) continue;
        const Bytes pixels = blobPixels(*t);
        require(pixels.size() == size_t(size) * size, "pictures: the route at its background's size, packed as the game reads it");
        require(free_roam::read16(&t->record[0x44]) == size && free_roam::read16(&t->record[0x46]) == size && (1u << t->record[0x48]) == size &&
                t->record[0x4A] == free_roam::kDXT5 && t->record[0x4E] == 1 && free_roam::read32(&t->record[0x38]) == size * size &&
                std::memcmp(&t->format[20], "DXT5", 4) == 0 && free_roam::read32(&t->record[0x28]) == 0x1A93CF,
                "pictures: a DXT5 texture, one level, the engine's fields from a stock route");
        // the road is drawn: some block with alpha, most without
        size_t lit = 0;
        for (size_t b = 0; b + 16 <= pixels.size(); b += 16) lit += pixels[b] != 0;
        require(lit > 0 && lit < pixels.size() / 16 / 20, "pictures: the road drawn, the rest clear");
    }
    require(!find(after, "TRACK_1300") && !find(after, "TRACK_1400"), "pictures: the drift districts use the game's own backgrounds");
    // the city's art missing: the city gets nothing, the others theirs
    const Bytes noCity = streamedFile({"DRIFT_TRACKS_BACK", "DRIFT_TRACKS_BACK2"});
    free_roam::makePicturePack(noCity, pictures, report);
    require(report.tracks == 2 && report.textures == 2 && report.skipped.size() == 1 && report.skipped[0] == "1000: no SPRINT_TRACKS_BACK",
            "pictures: a track whose art is missing is skipped");
    // a pack that has a route already (a mod's) keeps it
    const Bytes modded = streamedFile({"SPRINT_TRACKS_BACK", "DRIFT_TRACKS_BACK", "DRIFT_TRACKS_BACK2", "TRACK_1300_SPLINE"});
    free_roam::StreamedPack moddedPack; free_roam::parseStreamedPack(modded, moddedPack);
    free_roam::StreamedPack keptPack; free_roam::parseStreamedPack(free_roam::makePicturePack(modded, pictures, report), keptPack);
    require(find(keptPack, "TRACK_1300_SPLINE") && find(keptPack, "TRACK_1300_SPLINE")->blob == find(moddedPack, "TRACK_1300_SPLINE")->blob &&
            report.textures == 3, "pictures: a route the pack has stays");
    // no roads, a pack that is not one
    std::vector<free_roam::Picture> none(1);
    none[0].number = 1300;
    free_roam::makePicturePack(dyntex, none, report);
    require(report.tracks == 0 && report.skipped.size() == 1 && report.skipped[0] == "1300: no roads to draw", "pictures: a track without roads skipped");
    require(free_roam::makePicturePack(Bytes(64, 0), pictures, report).empty() && !report.error.empty(), "pictures: no pack, no file");
}

// ---- the lifter's lists -----------------------------------------------------------------------------------------

static std::set<uint32_t> configList(const char* name) {
    std::string path = __FILE__;
    path = path.substr(0, path.find_last_of('/')) + "/../config/" + name;
    std::ifstream file(path);
    require(bool(file), "config list readable");
    std::set<uint32_t> out;
    std::string line;
    while (std::getline(file, line)) {
        line = line.substr(0, line.find('#'));
        std::istringstream fields(line);
        std::string va;
        if (fields >> va) out.insert(uint32_t(std::stoul(va, nullptr, 16)));
    }
    return out;
}

static void testConfig() {
    const auto hooks = configList("hooks.txt");
    require(hooks.count(NFS_TRACKINFO_LOADED) && hooks.count(NFS_LANGUAGE_LOADED), "both free roam hooks are in config/hooks.txt");
    const auto fastpaths = configList("fastpaths.txt");
    require(hooks.count(NFS_LIST_ICON_HASHED) && hooks.count(NFS_LIST_REVERSE_TEST), "both track list hooks are in config/hooks.txt");
    require(fastpaths.count(NFS_WRONG_WAY_TEST) && fastpaths.count(NFS_PLAYER_UPDATE) && fastpaths.count(NFS_PLAYER_RESET),
            "the three free roam fast paths are in config/fastpaths.txt");
    require(!fastpaths.count(0x00554400u), "no fast path on the online list's entry check (SELECT LOCATION does not ask it)");
    require(fastpaths.size() <= NFS_FASTPATH_SLOTS, "every fast path has a slot");
    require(free_roam::kTrackCount == NFS_FREEROAM_MAX_TRACKS, "host and game side agree on the track count");
}

int main() {
    testRecord();
    testHashes();
    testIcons();
    testAddRecords();
    testAddNames();
    testMergeEdges();
    testCrumbs();
    testChunks();
    testTransplant();
    testPacks();
    testDxt();
    testMap();
    testRecords();
    testMakeTrack();
    testJdlz();
    testStreamedPack();
    testPicture();
    testPicturePack();
    testConfig();
    std::printf("free_roam: ok (%d checks)\n", checks);
    return 0;
}
