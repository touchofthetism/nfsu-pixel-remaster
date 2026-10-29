// The Garage decisions (src/runtime/nfs_garage.h): the branch override table against config/branches.txt and
// config/hooks.txt, each feature's forced outcomes, and the two register results the hooks leave.
#include "../src/runtime/nfs_garage.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <set>
#include <vector>
#include <sstream>
#include <string>
static void require(bool ok, const char* message) { if (!ok) { std::fprintf(stderr, "FAIL: %s\n", message); std::exit(1); } }

// The VAs a config list names (first column, '#' comments), as scripts/run_lift.py reads them.
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

int main() {
    // The table: sorted, unique, one known feature each, so the binary search and the counters hold.
    const unsigned count = NFS_BRANCH_OVERRIDE_COUNT;
    require(count == 20, "twenty overridden jumps");
    std::set<uint32_t> table;
    for (unsigned i = 0; i < count; ++i) {
        const nfs_branch_override& site = k_nfs_branch_overrides[i];
        if (i) require(k_nfs_branch_overrides[i - 1].va < site.va, "sorted by VA, no duplicates");
        require(site.feature && (site.feature & (site.feature - 1)) == 0 && (site.feature & NFS_GARAGE_ALL) == site.feature,
                "one feature bit each");
        require(site.feature != NFS_GARAGE_UNLOCK_ALL && site.feature != NFS_GARAGE_SPECIAL_VINYLS && site.feature != NFS_GARAGE_RACE_BRAKES &&
                site.feature != NFS_GARAGE_MORE_PAINTS, "unlock everything is a byte, special vinyls, race brakes and more paints are hooks");
        require(site.take == 0 || site.take == 1, "a jump is forced one way");
        require(site.va >= 0x00401000u && site.va < 0x00690000u, "inside Speed.exe's code");
        require(nfs_branch_override_index(site.va) == int(i), "found by its VA");
        table.insert(site.va);
    }
    require(nfs_branch_override_index(0x00400000u) == -1 && nfs_branch_override_index(0x0057EAF7u) == -1, "anything else is unknown");
    // The lifter wraps exactly the jumps the table decides.
    require(configList("branches.txt") == table, "config/branches.txt lists exactly the table's VAs");
    std::set<uint32_t> hooks = configList("hooks.txt");
    require(hooks.count(NFS_GARAGE_HOOK_HIDDEN_VINYL) && hooks.count(NFS_GARAGE_HOOK_BRAKE_LOD) && hooks.count(NFS_GARAGE_HOOK_PAINT_TYPES),
            "the three hooks are in config/hooks.txt");
    for (uint32_t va : table) require(!hooks.count(va), "no site is both a hook and a branch");

    // Off, every jump keeps its lifted outcome; on, the feature's jumps take the table's way, the others not.
    for (unsigned i = 0; i < count; ++i) {
        const nfs_branch_override& site = k_nfs_branch_overrides[i];
        for (int taken = 0; taken <= 1; ++taken) {
            require(nfs_branch_decision(0, site.va, taken) == taken, "no feature: the game's outcome");
            require(nfs_branch_decision(NFS_GARAGE_ALL & ~site.feature, site.va, taken) == taken, "other features leave it alone");
            require(nfs_branch_decision(site.feature, site.va, taken) == site.take, "its feature forces it");
            require(nfs_branch_decision(NFS_GARAGE_ALL, site.va, taken) == site.take, "all on forces it");
        }
    }
    require(nfs_branch_decision(NFS_GARAGE_ALL, 0x00401000u, 1) == 1 && nfs_branch_decision(NFS_GARAGE_ALL, 0x00401000u, 0) == 0,
            "an unknown VA keeps its outcome");
    // The patches each feature stands for (sites read from the mods' code, see nfs_garage.h).
    auto forced = [&](uint32_t feature) {
        std::set<std::pair<uint32_t, int>> out;
        for (unsigned i = 0; i < count; ++i)
            if (k_nfs_branch_overrides[i].feature == feature) out.insert({k_nfs_branch_overrides[i].va, k_nfs_branch_overrides[i].take});
        return out;
    };
    require(forced(NFS_GARAGE_HOOD_VINYLS) == std::set<std::pair<uint32_t, int>>{
                {0x004E465Bu, 1}, {0x00508AD1u, 1}, {0x00508ADDu, 1}, {0x00508AE9u, 1}, {0x00508AF5u, 1}},
            "hood vinyls: the gate's STOCK jump and the four empty-layer jumps of the hood install");
    require(forced(NFS_GARAGE_VINYL_LAYERS) == std::set<std::pair<uint32_t, int>>{
                {0x004E46E4u, 1}, {0x004E46F2u, 1}, {0x004E46FEu, 1}, {0x004E470Au, 1},
                {0x004E4791u, 1}, {0x004E479Bu, 1}, {0x004E47A5u, 1}, {0x004E47AFu, 1}},
            "vinyl layers: Extra Options' No Restrictions, both halves");
    require(forced(NFS_GARAGE_HOOD_DECALS) == std::set<std::pair<uint32_t, int>>{
                {0x00508B86u, 1}, {0x00508F52u, 1}, {0x005104FEu, 1}, {0x0057EAF6u, 0}, {0x0057EB16u, 0}, {0x0057EBA6u, 0}},
            "hood decals: no question, no removal, the decal menu open, EXCLUDEDECAL never found");
    require(forced(NFS_GARAGE_ALL_DECALS) == std::set<std::pair<uint32_t, int>>{{0x0051222Au, 1}}, "all decals: one jump");

    // Hooks: special vinyls clear al (the hidden test's answer), race brakes clear cl (the LOD set); the rest stays.
    require(nfs_garage_hidden_vinyl_eax(NFS_GARAGE_SPECIAL_VINYLS, 0x12345601u) == 0x12345600u, "special vinyls: not hidden");
    require(nfs_garage_hidden_vinyl_eax(NFS_GARAGE_ALL & ~NFS_GARAGE_SPECIAL_VINYLS, 0x12345601u) == 0x12345601u, "off: the game's answer");
    require(nfs_garage_brake_lod_ecx(NFS_GARAGE_RACE_BRAKES, 1u) == 0u, "race brakes: the first, full-detail set");
    require(nfs_garage_brake_lod_ecx(0, 1u) == 1u && nfs_garage_brake_lod_ecx(NFS_GARAGE_RACE_BRAKES, 0u) == 0u, "off, or already 0: unchanged");
    require(NFS_GARAGE_HOOK_HIDDEN_VINYL == 0x004E27D8u && NFS_GARAGE_HOOK_BRAKE_LOD == 0x005702F8u, "the instructions after the patched ones");

    // More paints: ARCHIE's lists for the base paint, the spoiler, the rims and the muffler tips; the calipers,
    // the vinyl's colours and the engine accents keep the game's own one palette.
    auto types = [](uint32_t target) {
        const uint8_t* list = nullptr;
        unsigned count = nfs_garage_paint_types(target, &list);
        return std::vector<int>(list, list + count);
    };
    require(types(0) == std::vector<int>{0, 1, 2, 4, 5, 7} && types(3) == types(0), "base and spoiler: gloss, metallic, pearl, calipers, rims, mufflers");
    require(types(1) == std::vector<int>{5, 0, 1, 2, 4}, "rims: their own palette first");
    require(types(6) == std::vector<int>{7, 0, 1, 2, 4, 5}, "muffler tips: their own palette first");
    for (uint32_t target : {2u, 4u, 5u, 7u, 0xFFFFFFFFu}) require(types(target).empty(), "the other targets, and out of range: unchanged");

    // The list as 0x4D7B70 leaves it, in a fake guest memory: a sentinel head and nodes {next, prev, type}.
    struct Fake {
        std::map<uint32_t, uint32_t> words;
        uint32_t next = 0x02000000u;
        int allocations = 0, failAfter = 1000;
    } fake;
    nfs_guest_memory memory = {
        [](void* ctx, uint32_t va) { auto& m = static_cast<Fake*>(ctx)->words; auto it = m.find(va); return it == m.end() ? 0xCDCDCDCDu : it->second; },
        [](void* ctx, uint32_t va, uint32_t value) { static_cast<Fake*>(ctx)->words[va] = value; },
        [](void* ctx, uint32_t bytes) -> uint32_t {
            Fake& f = *static_cast<Fake*>(ctx);
            if (f.allocations++ >= f.failAfter) return 0;
            uint32_t at = f.next; f.next += (bytes + 15u) & ~15u; return at;
        },
        &fake};
    const uint32_t head = 0x01000000u;
    auto link = [&](std::vector<uint32_t> listTypes) {   // as the game builds it: each node linked at the tail
        fake.words.clear(); fake.allocations = 0;
        fake.words[head] = head; fake.words[head + 4] = head;
        for (uint32_t type : listTypes) {
            uint32_t node = fake.next; fake.next += 16;
            uint32_t tail = fake.words[head + 4];
            fake.words[node + 8] = type; fake.words[tail] = node; fake.words[head + 4] = node; fake.words[node + 4] = tail; fake.words[node] = head;
        }
    };
    auto walk = [&]() {   // forward, checking every back link on the way
        std::vector<int> out;
        uint32_t previous = head;
        for (uint32_t node = fake.words[head]; node != head && out.size() < 100; previous = node, node = fake.words[node]) {
            require(fake.words[node + 4] == previous, "each node's prev is the one before it");
            out.push_back(int(fake.words[node + 8]));
        }
        require(fake.words[head + 4] == previous, "the head's prev is the last node");
        return out;
    };
    const uint8_t* base = nullptr;
    unsigned baseCount = nfs_garage_paint_types(0, &base);
    link({0, 1, 2});   // the career's full body list
    require(nfs_garage_append_paint_types(&memory, head, base, baseCount) == 3 && walk() == std::vector<int>{0, 1, 2, 4, 5, 7},
            "body: calipers, rims and mufflers added after the game's three");
    link({1});         // early in the career: metallic only
    require(nfs_garage_append_paint_types(&memory, head, base, baseCount) == 5 && walk() == std::vector<int>{1, 0, 2, 4, 5, 7},
            "the game's own first, then the rest in ARCHIE's order");
    link({});          // an empty list (the sentinel only)
    require(nfs_garage_append_paint_types(&memory, head, base, baseCount) == 6 && walk() == std::vector<int>{0, 1, 2, 4, 5, 7}, "from empty");
    link({0, 1, 2, 4, 5, 7});
    require(nfs_garage_append_paint_types(&memory, head, base, baseCount) == 0 && fake.allocations == 0, "complete already: nothing allocated");
    link({5});
    fake.failAfter = 2;
    require(nfs_garage_append_paint_types(&memory, head, base, baseCount) == 2 && walk() == std::vector<int>{5, 0, 1},
            "an allocation failure stops the appending with the list intact");
    fake.failAfter = 1000;
    link({0}); fake.words[fake.words[head]] = 0;   // a broken list: left alone
    require(nfs_garage_append_paint_types(&memory, head, base, baseCount) == 0 && fake.allocations == 0, "a null link: nothing added");
    link({0}); fake.words[fake.words[head]] = fake.words[head];   // a node pointing at itself: no endless walk
    require(nfs_garage_append_paint_types(&memory, head, base, baseCount) == 0, "a loop that never reaches the head: nothing added");
    require(NFS_GARAGE_HOOK_PAINT_TYPES == 0x004D7D74u && NFS_GARAGE_GAME_MALLOC == 0x0067121Au && NFS_PAINT_NODE_BYTES == 12,
            "the hook before the count, the game's malloc, its node size");

    // The bits match the launcher (GameOptions.GARAGE_KEYS, same order) and the telemetry keys.
    const char* keys[] = {"unlock", "specialVinyls", "vinylLayers", "hoodVinyls", "hoodDecals", "allDecals", "raceBrakes", "morePaints"};
    require(sizeof keys / sizeof keys[0] == NFS_GARAGE_FEATURE_COUNT && NFS_GARAGE_ALL == (1u << NFS_GARAGE_FEATURE_COUNT) - 1,
            "eight features, all bits");
    for (unsigned i = 0; i < NFS_GARAGE_FEATURE_COUNT; ++i) {
        require(std::strcmp(nfs_garage_feature_key(i), keys[i]) == 0, "telemetry key in launcher order");
        require(std::strlen(nfs_garage_feature_name(i)) > 8, "a readable name for the log");
    }
    require(std::strcmp(nfs_garage_feature_key(NFS_GARAGE_FEATURE_COUNT), "?") == 0, "out of range is marked");
    require(NFS_GARAGE_UNLOCK_FLAG == 0x00735EC1u, "Extra Options' _UnlockEverythingThatCanBeLocked");
    std::puts("garage: ok");
    return 0;
}
